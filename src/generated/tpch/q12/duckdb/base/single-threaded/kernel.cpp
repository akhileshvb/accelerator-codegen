#include "tpch/q12/duckdb/base/query.hpp"
#include "tpch/q12/duckdb/base/simd_lineitem.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
// TPC-H orderkeys are dense 1..O(SF). Grow geometrically; overflow covers negatives / absurd keys.
constexpr size_t kDefaultOrderCapacity = 6000001;

using Clock = std::chrono::steady_clock;

// Dense array encoding: 0=absent keeps probe O(1) without a presence bitmap.
enum class PriorityClass : uint8_t {
	Absent = 0,
	High = 1,
	Low = 2,
	Neither = 3, // NULL priority → both CASE expressions yield ELSE 0
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

struct Q12RuntimeConfig {
	bool dense_orders = true;
	bool orders_dict_fast = true;
	bool lineitem_dict_fast = true;
	bool skip_base_receipt_residual = false;
};

#ifdef ACCEL_EXPERIMENTAL_Q12_OPTS
bool ReadBoolEnv(const char *name, bool default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}
#endif

Q12RuntimeConfig ReadQ12RuntimeConfig() {
	Q12RuntimeConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q12_OPTS
	config.dense_orders = ReadBoolEnv("ACCEL_Q12_DENSE_ORDERS", true);
	config.orders_dict_fast = ReadBoolEnv("ACCEL_Q12_ORDERS_DICT_FAST", true);
	config.lineitem_dict_fast = ReadBoolEnv("ACCEL_Q12_LINEITEM_DICT_FAST", true);
	config.skip_base_receipt_residual = ReadBoolEnv("ACCEL_Q12_SKIP_BASE_RECEIPT_RESIDUAL", false);
#endif
	return config;
}

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

void RecordLayout(Q12LayoutHistogram *histogram, duckdb::Vector &vector) {
	if (!histogram) {
		return;
	}
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		if (duckdb::FlatVector::Validity(vector).AllValid()) {
			histogram->flat_all_valid += 1;
		} else {
			histogram->flat_nullable += 1;
		}
		return;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			if (duckdb::FlatVector::Validity(child).AllValid()) {
				histogram->dict_flat_all_valid += 1;
			} else {
				histogram->dict_flat_nullable += 1;
			}
		} else {
			histogram->other += 1;
		}
		return;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		if (!duckdb::ConstantVector::IsNull(vector)) {
			histogram->constant_all_valid += 1;
		} else {
			histogram->other += 1;
		}
		return;
	default:
		histogram->other += 1;
		return;
	}
}

void TrackPath(Q12InputProfile *profile, duckdb::DataChunk &input, int path) {
	if (!profile) {
		return;
	}
	profile->total_chunks += 1;
	profile->total_rows += input.size();
	switch (path) {
	case 0:
		profile->flat_fast_path_chunks += 1;
		profile->flat_fast_path_rows += input.size();
		return;
	case 1:
		profile->dict_fast_path_chunks += 1;
		profile->dict_fast_path_rows += input.size();
		return;
	default:
		profile->generic_fallback_chunks += 1;
		profile->generic_fallback_rows += input.size();
		return;
	}
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
	explicit OrderPriorityStore(bool use_dense_p) : use_dense(use_dense_p) {
	}

	int64_t InitializeNs() {
		if (!use_dense || initialized) {
			return 0;
		}
		const auto start = Clock::now();
		classes.assign(kDefaultOrderCapacity, static_cast<uint8_t>(PriorityClass::Absent));
		capacity = classes.size();
		initialized = true;
		const auto end = Clock::now();
		return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
	}

	void Store(int64_t orderkey, PriorityClass priority) {
		if (!use_dense || orderkey < 0) {
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
		if (classes[key] == static_cast<uint8_t>(PriorityClass::Absent)) {
			stored_count += 1;
		}
		classes[key] = static_cast<uint8_t>(priority);
	}

	inline void StoreNonNegUnchecked(uint64_t orderkey, PriorityClass priority) {
		if (classes[orderkey] == static_cast<uint8_t>(PriorityClass::Absent)) {
			stored_count += 1;
		}
		classes[orderkey] = static_cast<uint8_t>(priority);
	}

	bool CanStoreUnchecked(int64_t orderkey) const {
		return use_dense && orderkey >= 0 && static_cast<size_t>(orderkey) < capacity;
	}

	PriorityClass Probe(int64_t orderkey) const {
		if (!use_dense || orderkey < 0) {
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

	inline void PrefetchProbe(int64_t orderkey) const {
		if (orderkey >= 0 && static_cast<size_t>(orderkey) < capacity) {
			ACCEL_PREFETCH(classes.data() + orderkey);
		}
	}

	inline PriorityClass ProbeNonNegUnchecked(uint64_t orderkey) const {
		return static_cast<PriorityClass>(classes[orderkey]);
	}

	duckdb::idx_t Cardinality() const {
		return stored_count + overflow.size();
	}

	duckdb::idx_t MemoryBytes() const {
		return classes.size() + overflow.size() * sizeof(typename decltype(overflow)::value_type) +
		       overflow.bucket_count() * sizeof(void *);
	}

	duckdb::idx_t OverflowSize() const {
		return overflow.size();
	}

	bool UsesDense() const {
		return use_dense;
	}

	size_t Capacity() const {
		return capacity;
	}

	uint8_t *Data() {
		return classes.data();
	}

private:
	void EnsureCapacity(size_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		size_t target = new_capacity;
		if (capacity > 0) {
			const size_t doubled = capacity > (std::numeric_limits<size_t>::max() / 2) ? std::numeric_limits<size_t>::max()
			                                                                          : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		classes.resize(target, static_cast<uint8_t>(PriorityClass::Absent));
		capacity = classes.size();
	}

	bool use_dense;
	bool initialized = false;
	size_t capacity = 0;
	duckdb::idx_t stored_count = 0;
	std::vector<uint8_t> classes;
	std::unordered_map<int64_t, PriorityClass> overflow;
};

class Q12Kernel final : public KernelInstance {
public:
	explicit Q12Kernel(Q12Parameters params_p, Q12ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(ReadQ12RuntimeConfig()),
	      order_priority(config.dense_orders) {
		modes_equal = params.shipmode1 == params.shipmode2;
		fixed_slots[0].shipmode = params.shipmode1;
		fixed_slots[0].active = true;
		if (!modes_equal) {
			fixed_slots[1].shipmode = params.shipmode2;
			fixed_slots[1].active = true;
		}
		const auto init_ns = order_priority.InitializeNs();
		if (profile) {
			profile->orders_state_init_ns += init_ns;
			profile->used_dense_orders = order_priority.UsesDense();
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q12InputId>(input_id)) {
		case Q12InputId::ORDERS:
			ConsumeOrdersChunk(input);
			return;
		case Q12InputId::LINEITEM:
			ConsumeLineitemChunk(input);
			return;
		}
		throw duckdb::InternalException("Q12 kernel received unexpected input id %llu",
		                                static_cast<unsigned long long>(input_id));
	}

	void Finalize() override {
		if (finalized) {
			return;
		}

		const auto order_start = Clock::now();
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
		for (const auto &entry : generic_aggregates) {
			if (entry.second.high_line_count == 0 && entry.second.low_line_count == 0) {
				continue;
			}
			Q12OutputRow row;
			row.shipmode = entry.first;
			row.high_line_count = entry.second.high_line_count;
			row.low_line_count = entry.second.low_line_count;
			ordered_rows.push_back(std::move(row));
		}
		std::sort(ordered_rows.begin(), ordered_rows.end(),
		          [](const Q12OutputRow &lhs, const Q12OutputRow &rhs) { return lhs.shipmode < rhs.shipmode; });
		const auto order_end = Clock::now();

		if (profile) {
			profile->orders_map_cardinality = order_priority.Cardinality();
			profile->orders_map_memory_bytes = order_priority.MemoryBytes();
			profile->orders_overflow_entries = order_priority.OverflowSize();
			profile->emitted_rows = ordered_rows.size();
			profile->finalize_order_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(order_end - order_start).count();
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= ordered_rows.size()) {
			return false;
		}

		const auto emit_start = Clock::now();
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
		const auto emit_end = Clock::now();
		if (profile) {
			profile->finalize_output_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(emit_end - emit_start).count();
		}
		return true;
	}

private:
	void CountPriority(PriorityClass priority) {
		if (!profile) {
			return;
		}
		switch (priority) {
		case PriorityClass::High:
			profile->orders_high_priority += 1;
			break;
		case PriorityClass::Low:
			profile->orders_low_priority += 1;
			break;
		case PriorityClass::Neither:
			profile->orders_null_priority += 1;
			break;
		case PriorityClass::Absent:
			break;
		}
	}

	void StorePriority(int64_t orderkey, PriorityClass priority) {
		order_priority.Store(orderkey, priority);
		CountPriority(priority);
	}

	void ConsumeOrdersChunk(duckdb::DataChunk &input) {
		const auto build_start = Clock::now();
		if (profile) {
			RecordLayout(&profile->layout_o_orderkey, input.data[Q12OrdersChunkLayout::ORDERKEY_INDEX]);
			RecordLayout(&profile->layout_o_orderpriority, input.data[Q12OrdersChunkLayout::ORDERPRIORITY_INDEX]);
			profile->orders_rows_scanned += input.size();
		}

		auto &orderkey_vector = input.data[Q12OrdersChunkLayout::ORDERKEY_INDEX];
		auto &priority_vector = input.data[Q12OrdersChunkLayout::ORDERPRIORITY_INDEX];

		if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(priority_vector)) {
			TrackPath(profile ? &profile->orders : nullptr, input, 0);
			ConsumeOrdersFlatFlat(orderkey_vector, priority_vector, input.size(), build_start);
			return;
		}

		if (config.orders_dict_fast && IsFlatAllValid(orderkey_vector) && IsDictionaryOverFlat(priority_vector)) {
			TrackPath(profile ? &profile->orders : nullptr, input, 1);
			ConsumeOrdersFlatDictPriority(orderkey_vector, priority_vector, input.size(), build_start);
			return;
		}

		if (config.orders_dict_fast && IsDictionaryOverFlat(orderkey_vector) &&
		    IsDictionaryOverFlat(priority_vector)) {
			TrackPath(profile ? &profile->orders : nullptr, input, 1);
			ConsumeOrdersDictDict(orderkey_vector, priority_vector, input.size(), build_start);
			return;
		}

		TrackPath(profile ? &profile->orders : nullptr, input, 2);
		ConsumeOrdersGeneric(orderkey_vector, priority_vector, input.size(), build_start);
	}

	void ConsumeOrdersFlatFlat(duckdb::Vector &orderkey_vector, duckdb::Vector &priority_vector, duckdb::idx_t count,
	                           Clock::time_point build_start) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(priority_vector);
		const auto decode_end = Clock::now();
		if (profile) {
			profile->orders_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end - build_start).count();
		}
		const auto store_start = Clock::now();
		if (order_priority.UsesDense() && order_priority.Capacity() > 0) {
			const auto capacity = order_priority.Capacity();
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto orderkey = orderkeys[row];
				const auto priority = ClassifyPriority(priorities[row]);
				if (orderkey >= 0 && static_cast<size_t>(orderkey) < capacity) {
					order_priority.StoreNonNegUnchecked(static_cast<uint64_t>(orderkey), priority);
					CountPriority(priority);
				} else {
					StorePriority(orderkey, priority);
				}
			}
		} else {
			for (duckdb::idx_t row = 0; row < count; row++) {
				StorePriority(orderkeys[row], ClassifyPriority(priorities[row]));
			}
		}
		const auto store_end = Clock::now();
		if (profile) {
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - store_start).count();
			profile->orders_priority_classify_ns += loop_ns / 2;
			profile->orders_store_insert_ns += loop_ns - loop_ns / 2;
			profile->orders_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - build_start).count();
		}
	}

	void ConsumeOrdersFlatDictPriority(duckdb::Vector &orderkey_vector, duckdb::Vector &priority_vector,
	                                   duckdb::idx_t count, Clock::time_point build_start) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto &priority_sel = duckdb::DictionaryVector::SelVector(priority_vector);
		auto &priority_child = duckdb::DictionaryVector::Child(priority_vector);
		auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(priority_child);
		auto &priority_validity = duckdb::FlatVector::Validity(priority_child);

		const auto decode_end = Clock::now();
		if (profile) {
			profile->orders_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end - build_start).count();
		}

		const auto classify_start = Clock::now();
		if (!profile && order_priority.UsesDense() && priority_sel.data() &&
		    count <= STANDARD_VECTOR_SIZE) {
			uint8_t lut[q12simd::kLutBytes];
			if (q12simd::BuildSlotLut(priority_sel.data(), count, lut, [&](uint32_t idx) {
				    if (!priority_validity.AllValid() && !priority_validity.RowIsValidUnsafe(idx)) {
					    return static_cast<uint8_t>(PriorityClass::Neither);
				    }
				    return static_cast<uint8_t>(ClassifyPriority(priorities[idx]));
			    })) {
				// Non-profiling fast path: LUT-classified priorities stored branch-free into the dense array.
				auto *dense = order_priority.Data();
				auto dense_capacity = order_priority.Capacity();
				const uint32_t *psel = priority_sel.data();
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto orderkey = orderkeys[row];
					const auto cls = lut[psel[row]];
					if (ACCEL_LIKELY(static_cast<uint64_t>(orderkey) < dense_capacity)) {
						dense[orderkey] = cls;
					} else {
						StorePriority(orderkey, static_cast<PriorityClass>(cls));
						dense = order_priority.Data(); // the store may have grown the array
						dense_capacity = order_priority.Capacity();
					}
				}
				return;
			}
		}
		PriorityClass class_cache[STANDARD_VECTOR_SIZE];
		// Dictionary children for TPC-H priorities are tiny; classify selected child indices on demand.
		bool class_valid[STANDARD_VECTOR_SIZE] = {};
		duckdb::idx_t cache_hits = 0;
		const auto store_start = Clock::now();
		if (profile) {
			profile->orders_priority_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(store_start - classify_start).count();
		}

		auto *classes = order_priority.UsesDense() ? order_priority.Data() : nullptr;
		const auto capacity = order_priority.Capacity();

		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto priority_index = priority_sel.get_index(row);
			PriorityClass priority = PriorityClass::Neither;
			if (priority_index < STANDARD_VECTOR_SIZE && class_valid[priority_index]) {
				priority = class_cache[priority_index];
				cache_hits += 1;
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

			const auto orderkey = orderkeys[row];
			if (classes && orderkey >= 0 && static_cast<size_t>(orderkey) < capacity) {
				order_priority.StoreNonNegUnchecked(static_cast<uint64_t>(orderkey), priority);
				CountPriority(priority);
			} else {
				StorePriority(orderkey, priority);
			}
		}
		const auto store_end = Clock::now();
		if (profile) {
			profile->orders_dict_cache_entries += cache_hits;
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - store_start).count();
			profile->orders_priority_classify_ns += loop_ns / 2;
			profile->orders_store_insert_ns += loop_ns - loop_ns / 2;
			profile->orders_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - build_start).count();
		}
	}

	void ConsumeOrdersDictDict(duckdb::Vector &orderkey_vector, duckdb::Vector &priority_vector, duckdb::idx_t count,
	                           Clock::time_point build_start) {
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &priority_sel = duckdb::DictionaryVector::SelVector(priority_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &priority_child = duckdb::DictionaryVector::Child(priority_vector);
		auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(priority_child);
		auto &priority_validity = duckdb::FlatVector::Validity(priority_child);
		const bool shared_sel = &orderkey_sel == &priority_sel;
		if (profile && shared_sel) {
			profile->orders_shared_sel_chunks += 1;
		}

		const auto decode_end = Clock::now();
		if (profile) {
			profile->orders_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end - build_start).count();
		}

		PriorityClass class_cache[STANDARD_VECTOR_SIZE];
		bool class_valid[STANDARD_VECTOR_SIZE] = {};
		auto *classes = order_priority.UsesDense() ? order_priority.Data() : nullptr;
		const auto capacity = order_priority.Capacity();
		const auto store_start = Clock::now();
		for (duckdb::idx_t row = 0; row < count; row++) {
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
			const auto orderkey = orderkeys[orderkey_index];
			if (classes && orderkey >= 0 && static_cast<size_t>(orderkey) < capacity) {
				order_priority.StoreNonNegUnchecked(static_cast<uint64_t>(orderkey), priority);
				CountPriority(priority);
			} else {
				StorePriority(orderkey, priority);
			}
		}
		const auto store_end = Clock::now();
		if (profile) {
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - store_start).count();
			profile->orders_priority_classify_ns += loop_ns / 2;
			profile->orders_store_insert_ns += loop_ns - loop_ns / 2;
			profile->orders_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - build_start).count();
		}
	}

	void ConsumeOrdersGeneric(duckdb::Vector &orderkey_vector, duckdb::Vector &priority_vector, duckdb::idx_t count,
	                          Clock::time_point build_start) {
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat priority_format;
		orderkey_vector.ToUnifiedFormat(count, orderkey_format);
		priority_vector.ToUnifiedFormat(count, priority_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto priorities = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(priority_format);
		const auto decode_end = Clock::now();
		if (profile) {
			profile->orders_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end - build_start).count();
		}
		const auto store_start = Clock::now();
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto priority_index = priority_format.sel->get_index(row);
			if (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) {
				continue;
			}
			PriorityClass priority = PriorityClass::Neither;
			if (priority_format.validity.AllValid() || priority_format.validity.RowIsValidUnsafe(priority_index)) {
				priority = ClassifyPriority(priorities[priority_index]);
			}
			StorePriority(orderkeys[orderkey_index], priority);
		}
		const auto store_end = Clock::now();
		if (profile) {
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - store_start).count();
			profile->orders_priority_classify_ns += loop_ns / 2;
			profile->orders_store_insert_ns += loop_ns - loop_ns / 2;
			profile->orders_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(store_end - build_start).count();
		}
	}

	void ConsumeLineitemChunk(duckdb::DataChunk &input) {
		const auto decode_start = Clock::now();
		if (profile) {
			RecordLayout(&profile->layout_l_orderkey, input.data[Q12LineitemChunkLayout::ORDERKEY_INDEX]);
			RecordLayout(&profile->layout_l_shipmode, input.data[Q12LineitemChunkLayout::SHIPMODE_INDEX]);
			RecordLayout(&profile->layout_l_commitdate, input.data[Q12LineitemChunkLayout::COMMITDATE_INDEX]);
			RecordLayout(&profile->layout_l_receiptdate, input.data[Q12LineitemChunkLayout::RECEIPTDATE_INDEX]);
			RecordLayout(&profile->layout_l_shipdate, input.data[Q12LineitemChunkLayout::SHIPDATE_INDEX]);
			profile->lineitem_rows_scanned += input.size();
		}

		auto &orderkey_vector = input.data[Q12LineitemChunkLayout::ORDERKEY_INDEX];
		auto &shipmode_vector = input.data[Q12LineitemChunkLayout::SHIPMODE_INDEX];
		auto &commit_vector = input.data[Q12LineitemChunkLayout::COMMITDATE_INDEX];
		auto &receipt_vector = input.data[Q12LineitemChunkLayout::RECEIPTDATE_INDEX];
		auto &ship_vector = input.data[Q12LineitemChunkLayout::SHIPDATE_INDEX];

		if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(shipmode_vector) && IsFlatAllValid(commit_vector) &&
		    IsFlatAllValid(receipt_vector) && IsFlatAllValid(ship_vector)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, 0);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto shipmodes = duckdb::FlatVector::GetData<duckdb::string_t>(shipmode_vector);
			auto commitdates = duckdb::FlatVector::GetData<duckdb::date_t>(commit_vector);
			auto receiptdates = duckdb::FlatVector::GetData<duckdb::date_t>(receipt_vector);
			auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(ship_vector);
			if (profile) {
				profile->lineitem_scan_decode_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decode_start).count();
			}
			ProcessAlignedLineitem(orderkeys, nullptr, shipmodes, nullptr, commitdates, nullptr, receiptdates, nullptr,
			                       shipdates, nullptr, input.size(), /*require_receipt_residual=*/true);
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(orderkey_vector) &&
		    IsDictionaryOverFlat(shipmode_vector) && IsDictionaryOverFlat(commit_vector) &&
		    IsDictionaryOverFlat(receipt_vector) && IsDictionaryOverFlat(ship_vector)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, 1);
			ConsumeLineitemDict(orderkey_vector, shipmode_vector, commit_vector, receipt_vector, ship_vector,
			                    input.size(), decode_start);
			return;
		}

		TrackPath(profile ? &profile->lineitem : nullptr, input, 2);
		ConsumeLineitemGeneric(orderkey_vector, shipmode_vector, commit_vector, receipt_vector, ship_vector,
		                       input.size(), decode_start);
	}

	void ConsumeLineitemDict(duckdb::Vector &orderkey_vector, duckdb::Vector &shipmode_vector,
	                         duckdb::Vector &commit_vector, duckdb::Vector &receipt_vector,
	                         duckdb::Vector &ship_vector, duckdb::idx_t count, Clock::time_point decode_start) {
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &shipmode_sel = duckdb::DictionaryVector::SelVector(shipmode_vector);
		auto &commit_sel = duckdb::DictionaryVector::SelVector(commit_vector);
		auto &receipt_sel = duckdb::DictionaryVector::SelVector(receipt_vector);
		auto &ship_sel = duckdb::DictionaryVector::SelVector(ship_vector);
		const bool shared_sel = &orderkey_sel == &shipmode_sel && &orderkey_sel == &commit_sel &&
		                        &orderkey_sel == &receipt_sel && &orderkey_sel == &ship_sel;
		if (profile && shared_sel) {
			profile->lineitem_shared_sel_chunks += 1;
		}

		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &shipmode_child = duckdb::DictionaryVector::Child(shipmode_vector);
		auto shipmodes = duckdb::FlatVector::GetData<duckdb::string_t>(shipmode_child);
		auto &shipmode_validity = duckdb::FlatVector::Validity(shipmode_child);
		auto commitdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(commit_vector));
		auto receiptdates =
		    duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(receipt_vector));
		auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(ship_vector));
		auto &commit_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(commit_vector));
		auto &receipt_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(receipt_vector));
		auto &ship_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(ship_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));

		ShipModeSlot shipmode_cache[STANDARD_VECTOR_SIZE];
		bool shipmode_cache_valid[STANDARD_VECTOR_SIZE] = {};

		// Dictionary vectors from base storage that passed native receipt filters can skip residual
		// only when explicitly enabled. Default keeps residual for transaction-local safety.
		const bool skip_receipt_residual =
		    config.skip_base_receipt_residual && receipt_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR;

		if (profile) {
			profile->lineitem_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decode_start).count();
		}

		if (!profile && orderkey_validity.AllValid() && commit_validity.AllValid() && receipt_validity.AllValid() &&
		    ship_validity.AllValid() && orderkey_sel.data() && shipmode_sel.data() &&
		    commit_sel.data() && receipt_sel.data() && ship_sel.data() && count <= STANDARD_VECTOR_SIZE) {
			uint8_t lut[q12simd::kLutBytes];
			if (q12simd::BuildSlotLut(shipmode_sel.data(), count, lut, [&](uint32_t idx) {
				    // NULL shipmode (e.g. the dictionary's reserved NULL entry) never matches: row dropped.
				    if (!shipmode_validity.AllValid() && !shipmode_validity.RowIsValidUnsafe(idx)) {
					    return static_cast<uint8_t>(ShipModeSlot::Neither);
				    }
				    return static_cast<uint8_t>(
				        ClassifyShipMode(shipmodes[idx], params.shipmode1, params.shipmode2, modes_equal));
			    })) {
				int64_t survivor_keys[STANDARD_VECTOR_SIZE + simd::kSelSlack];
				uint8_t survivor_slots[STANDARD_VECTOR_SIZE + simd::kSelSlack];
				const auto survivors = q12simd::FilterDict(
				    orderkey_sel.data(), shipmode_sel.data(), commit_sel.data(), receipt_sel.data(), ship_sel.data(),
				    count, orderkeys, lut, reinterpret_cast<const int32_t *>(commitdates),
				    reinterpret_cast<const int32_t *>(receiptdates), reinterpret_cast<const int32_t *>(shipdates),
				    params.start_date.days, params.end_date.days, !skip_receipt_residual, survivor_keys,
				    survivor_slots);
				for (size_t i = 0; i < survivors; i++) {
					order_priority.PrefetchProbe(survivor_keys[i]);
				}
				for (size_t i = 0; i < survivors; i++) {
					ProbeAndAggregate(survivor_keys[i], static_cast<ShipModeSlot>(survivor_slots[i]));
				}
				return;
			}
		}

		if (shared_sel && orderkey_validity.AllValid() && shipmode_validity.AllValid() && commit_validity.AllValid() &&
		    receipt_validity.AllValid() && ship_validity.AllValid()) {
			ProcessAlignedLineitem(orderkeys, &orderkey_sel, shipmodes, &shipmode_sel, commitdates, &commit_sel,
			                       receiptdates, &receipt_sel, shipdates, &ship_sel, count, !skip_receipt_residual,
			                       shipmode_cache, shipmode_cache_valid);
			return;
		}

		for (duckdb::idx_t row = 0; row < count; row++) {
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
				if (profile) {
					profile->lineitem_dict_cache_entries += 1;
				}
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
			if (profile) {
				profile->shipmode_survivors += 1;
			}

			const auto commitdate = commitdates[commit_index];
			const auto receiptdate = receiptdates[receipt_index];
			const auto shipdate = shipdates[ship_index];
			if (!(commitdate < receiptdate)) {
				continue;
			}
			if (profile) {
				profile->commit_lt_receipt_survivors += 1;
			}
			if (!(shipdate < commitdate)) {
				continue;
			}
			if (profile) {
				profile->ship_lt_commit_survivors += 1;
			}
			if (!skip_receipt_residual) {
				if (profile) {
					profile->residual_receipt_checks += 1;
				}
				if (!(receiptdate >= params.start_date && receiptdate < params.end_date)) {
					if (profile) {
						profile->residual_receipt_rejects += 1;
					}
					continue;
				}
			} else if (profile) {
				profile->residual_receipt_skips += 1;
			}
			if (profile) {
				profile->receipt_window_survivors += 1;
			}
			ProbeAndAggregate(orderkeys[orderkey_index], slot);
		}
	}

	void ConsumeLineitemGeneric(duckdb::Vector &orderkey_vector, duckdb::Vector &shipmode_vector,
	                            duckdb::Vector &commit_vector, duckdb::Vector &receipt_vector,
	                            duckdb::Vector &ship_vector, duckdb::idx_t count, Clock::time_point decode_start) {
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat shipmode_format;
		duckdb::UnifiedVectorFormat commit_format;
		duckdb::UnifiedVectorFormat receipt_format;
		duckdb::UnifiedVectorFormat ship_format;
		const auto unified_start = Clock::now();
		orderkey_vector.ToUnifiedFormat(count, orderkey_format);
		shipmode_vector.ToUnifiedFormat(count, shipmode_format);
		commit_vector.ToUnifiedFormat(count, commit_format);
		receipt_vector.ToUnifiedFormat(count, receipt_format);
		ship_vector.ToUnifiedFormat(count, ship_format);
		const auto unified_end = Clock::now();
		if (profile) {
			profile->lineitem_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(unified_start - decode_start).count();
			profile->lineitem_unified_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(unified_end - unified_start).count();
		}

		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto shipmodes = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipmode_format);
		auto commitdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(commit_format);
		auto receiptdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(receipt_format);
		auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(ship_format);

		for (duckdb::idx_t row = 0; row < count; row++) {
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
			if (profile) {
				profile->shipmode_survivors += 1;
			}
			const auto commitdate = commitdates[commit_index];
			const auto receiptdate = receiptdates[receipt_index];
			const auto shipdate = shipdates[ship_index];
			if (!(commitdate < receiptdate)) {
				continue;
			}
			if (profile) {
				profile->commit_lt_receipt_survivors += 1;
			}
			if (!(shipdate < commitdate)) {
				continue;
			}
			if (profile) {
				profile->ship_lt_commit_survivors += 1;
			}
			if (profile) {
				profile->residual_receipt_checks += 1;
			}
			if (!(receiptdate >= params.start_date && receiptdate < params.end_date)) {
				if (profile) {
					profile->residual_receipt_rejects += 1;
				}
				continue;
			}
			if (profile) {
				profile->receipt_window_survivors += 1;
			}
			ProbeAndAggregate(orderkeys[orderkey_index], slot);
		}
	}

	void ProcessAlignedLineitem(const int64_t *orderkeys, const duckdb::SelectionVector *orderkey_sel,
	                            const duckdb::string_t *shipmodes, const duckdb::SelectionVector *shipmode_sel,
	                            const duckdb::date_t *commitdates, const duckdb::SelectionVector *commit_sel,
	                            const duckdb::date_t *receiptdates, const duckdb::SelectionVector *receipt_sel,
	                            const duckdb::date_t *shipdates, const duckdb::SelectionVector *ship_sel,
	                            duckdb::idx_t count, bool require_receipt_residual,
	                            ShipModeSlot *shipmode_cache = nullptr, bool *shipmode_cache_valid = nullptr) {
		const bool identity = orderkey_sel == nullptr;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto shipmode_index = identity ? row : shipmode_sel->get_index(row);
			ShipModeSlot slot = ShipModeSlot::Neither;
			if (shipmode_cache && shipmode_index < STANDARD_VECTOR_SIZE && shipmode_cache_valid[shipmode_index]) {
				slot = shipmode_cache[shipmode_index];
				if (profile) {
					profile->lineitem_dict_cache_entries += 1;
				}
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
			if (profile) {
				profile->shipmode_survivors += 1;
			}

			const auto commit_index = identity ? row : commit_sel->get_index(row);
			const auto receipt_index = identity ? row : receipt_sel->get_index(row);
			const auto ship_index = identity ? row : ship_sel->get_index(row);
			const auto commitdate = commitdates[commit_index];
			const auto receiptdate = receiptdates[receipt_index];
			const auto shipdate = shipdates[ship_index];

			if (!(commitdate < receiptdate)) {
				continue;
			}
			if (profile) {
				profile->commit_lt_receipt_survivors += 1;
			}
			if (!(shipdate < commitdate)) {
				continue;
			}
			if (profile) {
				profile->ship_lt_commit_survivors += 1;
			}
			if (require_receipt_residual) {
				if (profile) {
					profile->residual_receipt_checks += 1;
				}
				if (!(receiptdate >= params.start_date && receiptdate < params.end_date)) {
					if (profile) {
						profile->residual_receipt_rejects += 1;
					}
					continue;
				}
			} else if (profile) {
				profile->residual_receipt_skips += 1;
			}
			if (profile) {
				profile->receipt_window_survivors += 1;
			}

			const auto orderkey_index = identity ? row : orderkey_sel->get_index(row);
			ProbeAndAggregate(orderkeys[orderkey_index], slot);
		}
	}

	void ProbeAndAggregate(int64_t orderkey, ShipModeSlot slot) {
		PriorityClass priority;
		if (order_priority.CanStoreUnchecked(orderkey)) {
			priority = order_priority.ProbeNonNegUnchecked(static_cast<uint64_t>(orderkey));
		} else {
			priority = order_priority.Probe(orderkey);
		}
		if (priority == PriorityClass::Absent) {
			if (profile) {
				profile->probe_misses += 1;
			}
			return;
		}
		if (profile) {
			profile->probe_hits += 1;
		}
		if (priority == PriorityClass::Neither) {
			if (profile) {
				profile->probe_neither_priority += 1;
			}
			return;
		}

		ShipModeAggregate *target = nullptr;
		if (slot == ShipModeSlot::Mode0) {
			target = &fixed_slots[0];
			if (profile) {
				profile->fixed_slot_hits += 1;
			}
		} else if (slot == ShipModeSlot::Mode1) {
			target = &fixed_slots[1];
			if (profile) {
				profile->fixed_slot_hits += 1;
			}
		} else {
			return;
		}

		if (priority == PriorityClass::High) {
			target->high_line_count += 1;
		} else {
			target->low_line_count += 1;
		}
	}

	Q12Parameters params;
	Q12ProfileCounters *profile;
	Q12RuntimeConfig config;
	bool modes_equal = false;
	OrderPriorityStore order_priority;
	ShipModeAggregate fixed_slots[2];
	std::unordered_map<std::string, ShipModeAggregate> generic_aggregates;
	std::vector<Q12OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

bool Q12DenseOrdersEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q12_OPTS
	return ReadBoolEnv("ACCEL_Q12_DENSE_ORDERS", true);
#else
	return true;
#endif
}

bool Q12OrdersDictFastEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q12_OPTS
	return ReadBoolEnv("ACCEL_Q12_ORDERS_DICT_FAST", true);
#else
	return true;
#endif
}

bool Q12LineitemDictFastEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q12_OPTS
	return ReadBoolEnv("ACCEL_Q12_LINEITEM_DICT_FAST", true);
#else
	return true;
#endif
}

bool Q12SkipBaseReceiptResidualEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q12_OPTS
	return ReadBoolEnv("ACCEL_Q12_SKIP_BASE_RECEIPT_RESIDUAL", false);
#else
	return false;
#endif
}

duckdb::unique_ptr<KernelInstance> CreateQ12Kernel(const Q12Parameters &params) {
	return duckdb::make_uniq<Q12Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ12ProfilingKernel(const Q12Parameters &params,
                                                            Q12ProfileCounters &counters) {
	return duckdb::make_uniq<Q12Kernel>(params, &counters);
}

} // namespace accelerator_codegen
