#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/tpch_q12.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr size_t kDefaultOrderCapacity = 6000001;

enum class PriorityClass : uint8_t {
	Absent = 0,
	High = 1,
	Low = 2,
	Neither = 3,
};

enum class ShipModeSlot : uint8_t {
	Neither = 0,
	Mode0 = 1,
	Mode1 = 2,
};

struct ShipModeAggregate {
	std::string shipmode;
	int64_t high_line_count = 0;
	int64_t low_line_count = 0;
	bool active = false;
};

struct Q12OutputRow {
	std::string shipmode;
	int64_t high_line_count = 0;
	int64_t low_line_count = 0;
};

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

bool StringEquals(const duckdb::string_t &value, const std::string &expected) {
	return value.GetSize() == expected.size() &&
	       std::memcmp(value.GetData(), expected.data(), expected.size()) == 0;
}

bool IsHighPriority(const duckdb::string_t &value) {
	static constexpr const char kUrgent[] = "1-URGENT";
	static constexpr const char kHigh[] = "2-HIGH";
	const auto size = value.GetSize();
	const auto data = value.GetData();
	if (size == sizeof(kUrgent) - 1 && std::memcmp(data, kUrgent, sizeof(kUrgent) - 1) == 0) {
		return true;
	}
	if (size == sizeof(kHigh) - 1 && std::memcmp(data, kHigh, sizeof(kHigh) - 1) == 0) {
		return true;
	}
	return false;
}

PriorityClass ClassifyPriority(const duckdb::string_t &value) {
	return IsHighPriority(value) ? PriorityClass::High : PriorityClass::Low;
}

ShipModeSlot ClassifyShipMode(const duckdb::string_t &value, const std::string &mode1, const std::string &mode2,
                              bool modes_equal) {
	if (StringEquals(value, mode1)) {
		return ShipModeSlot::Mode0;
	}
	if (!modes_equal && StringEquals(value, mode2)) {
		return ShipModeSlot::Mode1;
	}
	return ShipModeSlot::Neither;
}

class OrderPriorityStore {
public:
	OrderPriorityStore() {
		classes.assign(kDefaultOrderCapacity, static_cast<uint8_t>(PriorityClass::Absent));
		capacity = classes.size();
	}

	void ReserveAtLeast(size_t min_capacity) {
		EnsureCapacity(min_capacity);
	}

	void Store(int64_t orderkey, PriorityClass priority) {
		if (orderkey < 0) {
			overflow[orderkey] = priority;
			return;
		}
		const auto key = static_cast<size_t>(orderkey);
		if (key >= capacity) {
			if (orderkey > 1'000'000'000) {
				overflow[orderkey] = priority;
				return;
			}
			EnsureCapacity(key + 1);
		}
		classes[key] = static_cast<uint8_t>(priority);
	}

	void StoreDenseNoGrow(int64_t orderkey, PriorityClass priority,
	                      std::unordered_map<int64_t, PriorityClass> &local_overflow) {
		if (orderkey < 0 || static_cast<size_t>(orderkey) >= capacity || orderkey > 1'000'000'000) {
			local_overflow[orderkey] = priority;
			return;
		}
		classes[static_cast<size_t>(orderkey)] = static_cast<uint8_t>(priority);
	}

	void MergeOverflow(const std::unordered_map<int64_t, PriorityClass> &local_overflow) {
		for (const auto &entry : local_overflow) {
			Store(entry.first, entry.second);
		}
	}

	PriorityClass Probe(int64_t orderkey) const {
		if (orderkey < 0) {
			const auto it = overflow.find(orderkey);
			return it == overflow.end() ? PriorityClass::Absent : it->second;
		}
		const auto key = static_cast<size_t>(orderkey);
		if (key < capacity) {
			return static_cast<PriorityClass>(classes[key]);
		}
		const auto it = overflow.find(orderkey);
		return it == overflow.end() ? PriorityClass::Absent : it->second;
	}

	bool CanProbeUnchecked(int64_t orderkey) const {
		return orderkey >= 0 && static_cast<size_t>(orderkey) < capacity;
	}

	PriorityClass ProbeNonNegUnchecked(uint64_t orderkey) const {
		return static_cast<PriorityClass>(classes[orderkey]);
	}

	size_t Capacity() const {
		return capacity;
	}

private:
	void EnsureCapacity(size_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		size_t target = new_capacity;
		if (capacity > 0) {
			const size_t doubled = capacity > (std::numeric_limits<size_t>::max() / 2)
			                           ? std::numeric_limits<size_t>::max()
			                           : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		classes.resize(target, static_cast<uint8_t>(PriorityClass::Absent));
		capacity = classes.size();
	}

	size_t capacity = 0;
	std::vector<uint8_t> classes;
	std::unordered_map<int64_t, PriorityClass> overflow;
};

void ConsumeOrdersChunkParallel(OrderPriorityStore &store, duckdb::DataChunk &input,
                                std::unordered_map<int64_t, PriorityClass> &local_overflow) {
	auto &orderkey_vector = input.data[Q12OrdersChunkLayout::ORDERKEY_INDEX];
	auto &priority_vector = input.data[Q12OrdersChunkLayout::ORDERPRIORITY_INDEX];

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(priority_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(priority_vector);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			store.StoreDenseNoGrow(orderkeys[row], ClassifyPriority(priorities[row]), local_overflow);
		}
		return;
	}

	if (IsFlatAllValid(orderkey_vector) && IsDictionaryOverFlat(priority_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto &priority_sel = duckdb::DictionaryVector::SelVector(priority_vector);
		auto &priority_child = duckdb::DictionaryVector::Child(priority_vector);
		auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(priority_child);
		auto &priority_validity = duckdb::FlatVector::Validity(priority_child);
		PriorityClass class_cache[STANDARD_VECTOR_SIZE];
		bool class_valid[STANDARD_VECTOR_SIZE] = {};
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto priority_index = priority_sel.get_index(row);
			PriorityClass priority = PriorityClass::Neither;
			if (priority_index < STANDARD_VECTOR_SIZE && class_valid[priority_index]) {
				priority = class_cache[priority_index];
			} else {
				if (!priority_validity.AllValid() && !priority_validity.RowIsValidUnsafe(priority_index)) {
					priority = PriorityClass::Neither;
				} else {
					priority = ClassifyPriority(priorities[priority_index]);
				}
				if (priority_index < STANDARD_VECTOR_SIZE) {
					class_cache[priority_index] = priority;
					class_valid[priority_index] = true;
				}
			}
			store.StoreDenseNoGrow(orderkeys[row], priority, local_overflow);
		}
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(priority_vector)) {
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &priority_sel = duckdb::DictionaryVector::SelVector(priority_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &priority_child = duckdb::DictionaryVector::Child(priority_vector);
		auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(priority_child);
		auto &priority_validity = duckdb::FlatVector::Validity(priority_child);
		const bool shared_sel = &orderkey_sel == &priority_sel;
		PriorityClass class_cache[STANDARD_VECTOR_SIZE];
		bool class_valid[STANDARD_VECTOR_SIZE] = {};
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto priority_index = shared_sel ? orderkey_index : priority_sel.get_index(row);
			PriorityClass priority = PriorityClass::Neither;
			if (priority_index < STANDARD_VECTOR_SIZE && class_valid[priority_index]) {
				priority = class_cache[priority_index];
			} else {
				if (!priority_validity.AllValid() && !priority_validity.RowIsValidUnsafe(priority_index)) {
					priority = PriorityClass::Neither;
				} else {
					priority = ClassifyPriority(priorities[priority_index]);
				}
				if (priority_index < STANDARD_VECTOR_SIZE) {
					class_cache[priority_index] = priority;
					class_valid[priority_index] = true;
				}
			}
			store.StoreDenseNoGrow(orderkeys[orderkey_index], priority, local_overflow);
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat priority_format;
	orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
	priority_vector.ToUnifiedFormat(input.size(), priority_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto priorities = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(priority_format);
	for (duckdb::idx_t row = 0; row < input.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto priority_index = priority_format.sel->get_index(row);
		if (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) {
			continue;
		}
		PriorityClass priority = PriorityClass::Neither;
		if (priority_format.validity.AllValid() || priority_format.validity.RowIsValidUnsafe(priority_index)) {
			priority = ClassifyPriority(priorities[priority_index]);
		}
		store.StoreDenseNoGrow(orderkeys[orderkey_index], priority, local_overflow);
	}
}

void ProbeAndAggregate(const OrderPriorityStore &store, ShipModeAggregate fixed_slots[2], int64_t orderkey,
                       ShipModeSlot slot) {
	const auto priority = store.CanProbeUnchecked(orderkey)
	                          ? store.ProbeNonNegUnchecked(static_cast<uint64_t>(orderkey))
	                          : store.Probe(orderkey);
	if (priority == PriorityClass::Absent || priority == PriorityClass::Neither) {
		return;
	}
	ShipModeAggregate *target = nullptr;
	if (slot == ShipModeSlot::Mode0) {
		target = &fixed_slots[0];
	} else if (slot == ShipModeSlot::Mode1) {
		target = &fixed_slots[1];
	} else {
		return;
	}
	if (priority == PriorityClass::High) {
		target->high_line_count += 1;
	} else {
		target->low_line_count += 1;
	}
}

void ProcessAlignedLineitemParallel(const OrderPriorityStore &store, const Q12Parameters &params, bool modes_equal,
                                    ShipModeAggregate fixed_slots[2], const int64_t *orderkeys,
                                    const duckdb::SelectionVector *orderkey_sel, const duckdb::string_t *shipmodes,
                                    const duckdb::SelectionVector *shipmode_sel, const duckdb::date_t *commitdates,
                                    const duckdb::SelectionVector *commit_sel, const duckdb::date_t *receiptdates,
                                    const duckdb::SelectionVector *receipt_sel, const duckdb::date_t *shipdates,
                                    const duckdb::SelectionVector *ship_sel, duckdb::idx_t count,
                                    bool require_receipt_residual, ShipModeSlot *shipmode_cache = nullptr,
                                    bool *shipmode_cache_valid = nullptr) {
	const bool identity = orderkey_sel == nullptr;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto shipmode_index = identity ? row : shipmode_sel->get_index(row);
		ShipModeSlot slot = ShipModeSlot::Neither;
		if (shipmode_cache && shipmode_index < STANDARD_VECTOR_SIZE && shipmode_cache_valid[shipmode_index]) {
			slot = shipmode_cache[shipmode_index];
		} else {
			slot = ClassifyShipMode(shipmodes[shipmode_index], params.shipmode1, params.shipmode2, modes_equal);
			if (shipmode_cache && shipmode_index < STANDARD_VECTOR_SIZE) {
				shipmode_cache[shipmode_index] = slot;
				shipmode_cache_valid[shipmode_index] = true;
			}
		}
		if (slot == ShipModeSlot::Neither) {
			continue;
		}
		const auto commit_index = identity ? row : commit_sel->get_index(row);
		const auto receipt_index = identity ? row : receipt_sel->get_index(row);
		const auto ship_index = identity ? row : ship_sel->get_index(row);
		const auto commitdate = commitdates[commit_index];
		const auto receiptdate = receiptdates[receipt_index];
		const auto shipdate = shipdates[ship_index];
		if (!(commitdate < receiptdate) || !(shipdate < commitdate)) {
			continue;
		}
		if (require_receipt_residual && !(receiptdate >= params.start_date && receiptdate < params.end_date)) {
			continue;
		}
		const auto orderkey_index = identity ? row : orderkey_sel->get_index(row);
		ProbeAndAggregate(store, fixed_slots, orderkeys[orderkey_index], slot);
	}
}

void ConsumeLineitemChunk(const OrderPriorityStore &store, const Q12Parameters &params, bool modes_equal,
                          ShipModeAggregate fixed_slots[2], duckdb::DataChunk &input) {
	auto &orderkey_vector = input.data[Q12LineitemChunkLayout::ORDERKEY_INDEX];
	auto &shipmode_vector = input.data[Q12LineitemChunkLayout::SHIPMODE_INDEX];
	auto &commit_vector = input.data[Q12LineitemChunkLayout::COMMITDATE_INDEX];
	auto &receipt_vector = input.data[Q12LineitemChunkLayout::RECEIPTDATE_INDEX];
	auto &ship_vector = input.data[Q12LineitemChunkLayout::SHIPDATE_INDEX];

	// DICTIONARY receipt from base storage already passed native receipt filters (same trust as Q3).
	const bool skip_receipt_residual =
	    receipt_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR;

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(shipmode_vector) && IsFlatAllValid(commit_vector) &&
	    IsFlatAllValid(receipt_vector) && IsFlatAllValid(ship_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto shipmodes = duckdb::FlatVector::GetData<duckdb::string_t>(shipmode_vector);
		auto commitdates = duckdb::FlatVector::GetData<duckdb::date_t>(commit_vector);
		auto receiptdates = duckdb::FlatVector::GetData<duckdb::date_t>(receipt_vector);
		auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(ship_vector);
		ProcessAlignedLineitemParallel(store, params, modes_equal, fixed_slots, orderkeys, nullptr, shipmodes, nullptr,
		                               commitdates, nullptr, receiptdates, nullptr, shipdates, nullptr, input.size(),
		                               !skip_receipt_residual);
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(shipmode_vector) &&
	    IsDictionaryOverFlat(commit_vector) && IsDictionaryOverFlat(receipt_vector) &&
	    IsDictionaryOverFlat(ship_vector)) {
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &shipmode_sel = duckdb::DictionaryVector::SelVector(shipmode_vector);
		auto &commit_sel = duckdb::DictionaryVector::SelVector(commit_vector);
		auto &receipt_sel = duckdb::DictionaryVector::SelVector(receipt_vector);
		auto &ship_sel = duckdb::DictionaryVector::SelVector(ship_vector);
		const bool shared_sel = &orderkey_sel == &shipmode_sel && &orderkey_sel == &commit_sel &&
		                        &orderkey_sel == &receipt_sel && &orderkey_sel == &ship_sel;
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &shipmode_child = duckdb::DictionaryVector::Child(shipmode_vector);
		auto shipmodes = duckdb::FlatVector::GetData<duckdb::string_t>(shipmode_child);
		auto &shipmode_validity = duckdb::FlatVector::Validity(shipmode_child);
		auto commitdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(commit_vector));
		auto receiptdates =
		    duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(receipt_vector));
		auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(ship_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &commit_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(commit_vector));
		auto &receipt_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(receipt_vector));
		auto &ship_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(ship_vector));
		ShipModeSlot shipmode_cache[STANDARD_VECTOR_SIZE];
		bool shipmode_cache_valid[STANDARD_VECTOR_SIZE] = {};

		if (shared_sel && orderkey_validity.AllValid() && shipmode_validity.AllValid() && commit_validity.AllValid() &&
		    receipt_validity.AllValid() && ship_validity.AllValid()) {
			ProcessAlignedLineitemParallel(store, params, modes_equal, fixed_slots, orderkeys, &orderkey_sel, shipmodes,
			                               &shipmode_sel, commitdates, &commit_sel, receiptdates, &receipt_sel,
			                               shipdates, &ship_sel, input.size(), !skip_receipt_residual, shipmode_cache,
			                               shipmode_cache_valid);
			return;
		}

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto shipmode_index = shipmode_sel.get_index(row);
			const auto commit_index = commit_sel.get_index(row);
			const auto receipt_index = receipt_sel.get_index(row);
			const auto ship_index = ship_sel.get_index(row);
			if ((!orderkey_validity.AllValid() && !orderkey_validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!shipmode_validity.AllValid() && !shipmode_validity.RowIsValidUnsafe(shipmode_index)) ||
			    (!commit_validity.AllValid() && !commit_validity.RowIsValidUnsafe(commit_index)) ||
			    (!receipt_validity.AllValid() && !receipt_validity.RowIsValidUnsafe(receipt_index)) ||
			    (!ship_validity.AllValid() && !ship_validity.RowIsValidUnsafe(ship_index))) {
				continue;
			}
			ShipModeSlot slot = ShipModeSlot::Neither;
			if (shipmode_index < STANDARD_VECTOR_SIZE && shipmode_cache_valid[shipmode_index]) {
				slot = shipmode_cache[shipmode_index];
			} else {
				slot = ClassifyShipMode(shipmodes[shipmode_index], params.shipmode1, params.shipmode2, modes_equal);
				if (shipmode_index < STANDARD_VECTOR_SIZE) {
					shipmode_cache[shipmode_index] = slot;
					shipmode_cache_valid[shipmode_index] = true;
				}
			}
			if (slot == ShipModeSlot::Neither) {
				continue;
			}
			const auto commitdate = commitdates[commit_index];
			const auto receiptdate = receiptdates[receipt_index];
			const auto shipdate = shipdates[ship_index];
			if (!(commitdate < receiptdate) || !(shipdate < commitdate)) {
				continue;
			}
			if (!skip_receipt_residual && !(receiptdate >= params.start_date && receiptdate < params.end_date)) {
				continue;
			}
			ProbeAndAggregate(store, fixed_slots, orderkeys[orderkey_index], slot);
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat shipmode_format;
	duckdb::UnifiedVectorFormat commit_format;
	duckdb::UnifiedVectorFormat receipt_format;
	duckdb::UnifiedVectorFormat ship_format;
	orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
	shipmode_vector.ToUnifiedFormat(input.size(), shipmode_format);
	commit_vector.ToUnifiedFormat(input.size(), commit_format);
	receipt_vector.ToUnifiedFormat(input.size(), receipt_format);
	ship_vector.ToUnifiedFormat(input.size(), ship_format);

	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto shipmodes = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipmode_format);
	auto commitdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(commit_format);
	auto receiptdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(receipt_format);
	auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(ship_format);

	for (duckdb::idx_t row = 0; row < input.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto shipmode_index = shipmode_format.sel->get_index(row);
		const auto commit_index = commit_format.sel->get_index(row);
		const auto receipt_index = receipt_format.sel->get_index(row);
		const auto ship_index = ship_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!shipmode_format.validity.AllValid() && !shipmode_format.validity.RowIsValidUnsafe(shipmode_index)) ||
		    (!commit_format.validity.AllValid() && !commit_format.validity.RowIsValidUnsafe(commit_index)) ||
		    (!receipt_format.validity.AllValid() && !receipt_format.validity.RowIsValidUnsafe(receipt_index)) ||
		    (!ship_format.validity.AllValid() && !ship_format.validity.RowIsValidUnsafe(ship_index))) {
			continue;
		}
		const auto slot =
		    ClassifyShipMode(shipmodes[shipmode_index], params.shipmode1, params.shipmode2, modes_equal);
		if (slot == ShipModeSlot::Neither) {
			continue;
		}
		const auto commitdate = commitdates[commit_index];
		const auto receiptdate = receiptdates[receipt_index];
		const auto shipdate = shipdates[ship_index];
		if (!(commitdate < receiptdate) || !(shipdate < commitdate)) {
			continue;
		}
		if (!skip_receipt_residual && !(receiptdate >= params.start_date && receiptdate < params.end_date)) {
			continue;
		}
		ProbeAndAggregate(store, fixed_slots, orderkeys[orderkey_index], slot);
	}
}

class Q12ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q12ParallelKernel(Q12Parameters params_p) : params(std::move(params_p)) {
		modes_equal = params.shipmode1 == params.shipmode2;
		fixed_slots[0].shipmode = params.shipmode1;
		fixed_slots[0].active = true;
		if (!modes_equal) {
			fixed_slots[1].shipmode = params.shipmode2;
			fixed_slots[1].active = true;
		}
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q12 parallel expects orders + lineitem bound inputs");
		}

		using clock = std::chrono::steady_clock;
		OrderPriorityStore order_priority;
		const auto worker_count = ParallelWorkerCount(context);
		if (const char *audit = std::getenv("ACCEL_PARALLEL_AUDIT"); audit && audit[0] == '1') {
			std::cerr << "ACCEL_PARALLEL_AUDIT q12 parallel_build workers=" << worker_count
			          << " phases=orders_fill,lineitem,merge\n";
		}

		order_priority.ReserveAtLeast(
		    static_cast<size_t>(EstimateTpchDenseOrderCapacity(context, *bound_inputs[0])));

		const auto orders_t0 = clock::now();
		std::vector<CacheLinePadded<std::unordered_map<int64_t, PriorityClass>>> order_overflows(worker_count);
		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     ConsumeOrdersChunkParallel(order_priority, chunk, order_overflows[worker_id].value);
		                     });
		for (const auto &local : order_overflows) {
			order_priority.MergeOverflow(local.value);
		}
		const auto orders_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();

		std::vector<CacheLinePadded<std::array<ShipModeAggregate, 2>>> workers(worker_count);
		for (auto &w : workers) {
			w.value[0].shipmode = params.shipmode1;
			w.value[0].active = true;
			if (!modes_equal) {
				w.value[1].shipmode = params.shipmode2;
				w.value[1].active = true;
			}
		}

		const auto lineitem_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     ConsumeLineitemChunk(order_priority, params, modes_equal, workers[worker_id].value.data(),
			                                          chunk);
		                     });
		const auto lineitem_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

		const auto merge_t0 = clock::now();
		for (const auto &w : workers) {
			for (int i = 0; i < 2; i++) {
				if (!fixed_slots[i].active) {
					continue;
				}
				fixed_slots[i].high_line_count += w.value[i].high_line_count;
				fixed_slots[i].low_line_count += w.value[i].low_line_count;
			}
		}
		const auto merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			std::cerr << "q12_parallel_profile orders_ns=" << orders_ns << " lineitem_ns=" << lineitem_ns
			          << " merge_ns=" << merge_ns << " worker_count=" << worker_count
			          << " capacity=" << order_priority.Capacity() << "\n";
		}
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		ordered_rows.clear();
		for (auto &slot : fixed_slots) {
			if (!slot.active) {
				continue;
			}
			if (slot.high_line_count == 0 && slot.low_line_count == 0) {
				continue;
			}
			Q12OutputRow row;
			row.shipmode = slot.shipmode;
			row.high_line_count = slot.high_line_count;
			row.low_line_count = slot.low_line_count;
			ordered_rows.push_back(std::move(row));
		}
		std::sort(ordered_rows.begin(), ordered_rows.end(),
		          [](const Q12OutputRow &lhs, const Q12OutputRow &rhs) { return lhs.shipmode < rhs.shipmode; });
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= ordered_rows.size()) {
			return false;
		}
		const auto remaining = ordered_rows.size() - output_offset;
		const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = ordered_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value(result.shipmode));
			output.SetValue(1, row, duckdb::Value::HUGEINT(duckdb::hugeint_t(result.high_line_count)));
			output.SetValue(2, row, duckdb::Value::HUGEINT(duckdb::hugeint_t(result.low_line_count)));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q12Parameters params;
	bool modes_equal = false;
	ShipModeAggregate fixed_slots[2];
	std::vector<Q12OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ12ParallelKernel(const Q12Parameters &params) {
	return duckdb::make_uniq<Q12ParallelKernel>(params);
}

} // namespace accelerator_codegen
