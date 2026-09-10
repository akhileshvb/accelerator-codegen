#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q18/duckdb/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/parallel/task.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr duckdb::idx_t kMinPartitions = 16;
constexpr duckdb::idx_t kMaxPartitions = 256;
constexpr duckdb::idx_t kPartitionsPerWorker = 4;

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

struct QuantityUpdate {
	uint32_t orderkey = 0;
	int64_t quantity_raw = 0;
};

struct OrderMetadata {
	int64_t custkey = 0;
	duckdb::date_t orderdate;
	duckdb::hugeint_t totalprice;
};

struct QualifyingOrderRow {
	int64_t orderkey = 0;
	OrderMetadata metadata;
};

struct Q18OutputRow {
	std::string name;
	int64_t custkey = 0;
	int64_t orderkey = 0;
	duckdb::date_t orderdate;
	duckdb::hugeint_t totalprice;
	duckdb::hugeint_t quantity_sum;
};

bool BetterOutputRow(const Q18OutputRow &lhs, const Q18OutputRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.totalprice, rhs.totalprice)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.totalprice, lhs.totalprice)) {
		return false;
	}
	return lhs.orderdate < rhs.orderdate;
}

struct OrderPartitionPlan {
	int64_t capacity = 0;
	duckdb::idx_t num_partitions = 0;
	int64_t keys_per_partition = 0;

	static OrderPartitionPlan Build(int64_t order_capacity, duckdb::idx_t worker_count) {
		OrderPartitionPlan plan;
		plan.capacity = std::max<int64_t>(order_capacity, kDefaultOrderCapacity);
		plan.num_partitions = worker_count * kPartitionsPerWorker;
		if (plan.num_partitions < kMinPartitions) {
			plan.num_partitions = kMinPartitions;
		}
		if (plan.num_partitions > kMaxPartitions) {
			plan.num_partitions = kMaxPartitions;
		}
		plan.keys_per_partition = (plan.capacity + static_cast<int64_t>(plan.num_partitions) - 1) /
		                          static_cast<int64_t>(plan.num_partitions);
		if (plan.keys_per_partition < 1) {
			plan.keys_per_partition = 1;
		}
		return plan;
	}

	int64_t PartitionBegin(duckdb::idx_t partition_id) const {
		return static_cast<int64_t>(partition_id) * keys_per_partition;
	}

	int64_t PartitionEnd(duckdb::idx_t partition_id) const {
		const auto begin = PartitionBegin(partition_id);
		const auto end = begin + keys_per_partition;
		return end > capacity ? capacity : end;
	}

	duckdb::idx_t PartitionForOrderKey(int64_t orderkey) const {
		if (orderkey < 0 || orderkey >= capacity) {
			return std::numeric_limits<duckdb::idx_t>::max();
		}
		const auto partition = static_cast<duckdb::idx_t>(orderkey / keys_per_partition);
		return partition < num_partitions ? partition : num_partitions - 1;
	}
};

struct Q18FrozenState {
	int64_t quantity_threshold_raw = 0;
	std::vector<int64_t> order_quantity_totals;
	std::vector<uint8_t> order_qualifying;
	bool has_qualifying_orders = false;

	explicit Q18FrozenState(int64_t quantity_threshold, int64_t order_capacity)
	    : quantity_threshold_raw(quantity_threshold * 100) {
		const auto capacity = static_cast<size_t>(std::max<int64_t>(order_capacity, kDefaultOrderCapacity));
		order_quantity_totals.assign(capacity, 0);
		order_qualifying.assign(capacity, 0);
	}

	void FinalizeQualifyingOrders() {
		has_qualifying_orders = false;
		for (size_t orderkey = 0; orderkey < order_quantity_totals.size(); orderkey++) {
			if (order_quantity_totals[orderkey] > quantity_threshold_raw) {
				order_qualifying[orderkey] = 1;
				has_qualifying_orders = true;
			}
		}
	}

	void FinalizeQualifyingTouched(const std::vector<int64_t> &touched_orderkeys) {
		has_qualifying_orders = false;
		for (const auto orderkey : touched_orderkeys) {
			if (orderkey < 0 || static_cast<size_t>(orderkey) >= order_quantity_totals.size()) {
				continue;
			}
			const auto sum = order_quantity_totals[static_cast<size_t>(orderkey)];
			if (sum <= quantity_threshold_raw) {
				continue;
			}
			order_qualifying[static_cast<size_t>(orderkey)] = 1;
			has_qualifying_orders = true;
		}
	}

	bool IsQualifyingOrder(int64_t orderkey) const {
		if (orderkey < 0 || static_cast<size_t>(orderkey) >= order_qualifying.size()) {
			return false;
		}
		return order_qualifying[static_cast<size_t>(orderkey)] != 0;
	}

	int64_t OrderQuantity(int64_t orderkey) const {
		if (orderkey < 0 || static_cast<size_t>(orderkey) >= order_quantity_totals.size()) {
			return 0;
		}
		return order_quantity_totals[static_cast<size_t>(orderkey)];
	}
};

struct LineitemPassProfile {
	size_t rows_in = 0;
	size_t updates = 0;
	size_t flat_rows = 0;
	size_t dict_rows = 0;
	size_t fallback_rows = 0;
	int64_t process_ns = 0;
};

struct LineitemWorkerState {
	std::vector<std::vector<QuantityUpdate>> partition_fragments;
	LineitemPassProfile stats;

	explicit LineitemWorkerState(duckdb::idx_t num_partitions) : partition_fragments(num_partitions) {
	}
};

struct Q18OrdersWorkerState {
	std::vector<QualifyingOrderRow> orders;
	std::vector<uint8_t> needed_custkeys;

	void MarkCustkey(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(custkey) + 1;
		if (required > needed_custkeys.size()) {
			size_t target = required;
			if (!needed_custkeys.empty()) {
				const size_t doubled = needed_custkeys.size() * 2;
				if (doubled > target) {
					target = doubled;
				}
			} else {
				target = std::max(target, static_cast<size_t>(kDefaultCustomerCapacity));
			}
			needed_custkeys.resize(target, 0);
		}
		needed_custkeys[static_cast<size_t>(custkey)] = 1;
	}
};

struct PartitionAggregateProfile {
	size_t updates_merged = 0;
	size_t qualifying_orders = 0;
	int64_t process_ns = 0;
};

struct Q18ParallelProfile {
	int64_t lineitem_scan_ns = 0;
	int64_t partition_aggregate_ns = 0;
	int64_t orders_scan_ns = 0;
	int64_t orders_merge_ns = 0;
	int64_t customer_ns = 0;
	int64_t finalize_ns = 0;
	size_t worker_count = 0;
	size_t partition_count = 0;
	size_t order_capacity = 0;
	size_t lineitem_rows = 0;
	size_t quantity_updates = 0;
	size_t atomic_ops = 0;
	size_t update_bytes = 0;
	size_t partition_state_bytes = 0;
	size_t qualifying_orders = 0;
	size_t lineitem_flat_rows = 0;
	size_t lineitem_dict_rows = 0;
	size_t lineitem_fallback_rows = 0;
	int64_t min_partition_ns = 0;
	int64_t max_partition_ns = 0;
	int64_t min_worker_lineitem_ns = 0;
	int64_t max_worker_lineitem_ns = 0;
};

void AppendQuantityUpdate(LineitemWorkerState &worker, const OrderPartitionPlan &plan, int64_t orderkey,
                          int64_t quantity_raw) {
	const auto partition_id = plan.PartitionForOrderKey(orderkey);
	if (partition_id == std::numeric_limits<duckdb::idx_t>::max()) {
		return;
	}
	worker.partition_fragments[partition_id].push_back(
	    QuantityUpdate {static_cast<uint32_t>(orderkey), quantity_raw});
	worker.stats.updates += 1;
}

void AccumulateLineitemChunkDirect(duckdb::DataChunk &chunk, std::vector<int64_t> &order_quantity_totals,
                                   std::vector<uint8_t> &touched_orders, std::vector<int64_t> &touched_orderkeys) {
	const auto capacity = static_cast<int64_t>(order_quantity_totals.size());
	auto &orderkey_vector = chunk.data[Q18LineitemChunkLayout::ORDERKEY_INDEX];
	auto &quantity_vector = chunk.data[Q18LineitemChunkLayout::QUANTITY_INDEX];

	auto accumulate = [&](int64_t orderkey, int64_t quantity_raw) {
		if (orderkey < 0 || orderkey >= capacity) {
			return;
		}
		const auto index = static_cast<size_t>(orderkey);
		order_quantity_totals[index] += quantity_raw;
		if (touched_orders[index] == 0) {
			touched_orders[index] = 1;
			touched_orderkeys.push_back(orderkey);
		}
	};
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(quantity_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			accumulate(orderkeys[row], quantities[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(quantity_vector)) {
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto quantities = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(quantity_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &quantity_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(quantity_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto quantity_index = quantity_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !quantity_validity.RowIsValid(quantity_index)) {
				continue;
			}
			accumulate(orderkeys[orderkey_index], quantities[quantity_index]);
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	quantity_vector.ToUnifiedFormat(chunk.size(), quantity_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto quantity_index = quantity_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
			continue;
		}
		accumulate(orderkeys[orderkey_index], quantities[quantity_index]);
	}
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const OrderPartitionPlan &plan, LineitemWorkerState &worker) {
	worker.stats.rows_in += chunk.size();
	auto &orderkey_vector = chunk.data[Q18LineitemChunkLayout::ORDERKEY_INDEX];
	auto &quantity_vector = chunk.data[Q18LineitemChunkLayout::QUANTITY_INDEX];

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(quantity_vector)) {
		worker.stats.flat_rows += chunk.size();
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			AppendQuantityUpdate(worker, plan, orderkeys[row], quantities[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(quantity_vector)) {
		worker.stats.dict_rows += chunk.size();
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto quantities = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(quantity_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &quantity_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(quantity_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto quantity_index = quantity_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !quantity_validity.RowIsValid(quantity_index)) {
				continue;
			}
			AppendQuantityUpdate(worker, plan, orderkeys[orderkey_index], quantities[quantity_index]);
		}
		return;
	}

	worker.stats.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	quantity_vector.ToUnifiedFormat(chunk.size(), quantity_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto quantity_index = quantity_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
			continue;
		}
		AppendQuantityUpdate(worker, plan, orderkeys[orderkey_index], quantities[quantity_index]);
	}
}

void AccumulateOrdersChunk(duckdb::DataChunk &chunk, const Q18FrozenState &state, Q18OrdersWorkerState &worker) {
	if (!state.has_qualifying_orders) {
		return;
	}

	auto &orderkey_vector = chunk.data[Q18OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q18OrdersChunkLayout::CUSTKEY_INDEX];
	auto &orderdate_vector = chunk.data[Q18OrdersChunkLayout::ORDERDATE_INDEX];
	auto &totalprice_vector = chunk.data[Q18OrdersChunkLayout::TOTALPRICE_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) && IsFlatAllValid(orderdate_vector) &&
	    IsFlatAllValid(totalprice_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto orderdates = duckdb::FlatVector::GetData<int32_t>(orderdate_vector);
		auto totalprices = duckdb::FlatVector::GetData<int64_t>(totalprice_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey = orderkeys[row];
			if (!state.IsQualifyingOrder(orderkey)) {
				continue;
			}
			const auto custkey = custkeys[row];
			worker.MarkCustkey(custkey);
			worker.orders.push_back(
			    QualifyingOrderRow {orderkey,
			                        OrderMetadata {custkey, duckdb::date_t(orderdates[row]), duckdb::hugeint_t(totalprices[row])}});
		}
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(custkey_vector) &&
	    IsDictionaryOverFlat(orderdate_vector) && IsDictionaryOverFlat(totalprice_vector)) {
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
		auto &orderdate_sel = duckdb::DictionaryVector::SelVector(orderdate_vector);
		auto &totalprice_sel = duckdb::DictionaryVector::SelVector(totalprice_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
		auto orderdates = duckdb::FlatVector::GetData<int32_t>(duckdb::DictionaryVector::Child(orderdate_vector));
		auto totalprices = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(totalprice_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
		auto &orderdate_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderdate_vector));
		auto &totalprice_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(totalprice_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto custkey_index = custkey_sel.get_index(row);
			const auto orderdate_index = orderdate_sel.get_index(row);
			const auto totalprice_index = totalprice_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !custkey_validity.RowIsValid(custkey_index) ||
			    !orderdate_validity.RowIsValid(orderdate_index) || !totalprice_validity.RowIsValid(totalprice_index)) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			if (!state.IsQualifyingOrder(orderkey)) {
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			worker.MarkCustkey(custkey);
			worker.orders.push_back(QualifyingOrderRow {
			    orderkey,
			    OrderMetadata {custkey, duckdb::date_t(orderdates[orderdate_index]),
			                   duckdb::hugeint_t(totalprices[totalprice_index])}});
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat orderdate_format;
	duckdb::UnifiedVectorFormat totalprice_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	orderdate_vector.ToUnifiedFormat(chunk.size(), orderdate_format);
	totalprice_vector.ToUnifiedFormat(chunk.size(), totalprice_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto orderdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(orderdate_format);
	auto totalprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(totalprice_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto orderdate_index = orderdate_format.sel->get_index(row);
		const auto totalprice_index = totalprice_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) ||
		    (!totalprice_format.validity.AllValid() &&
		     !totalprice_format.validity.RowIsValidUnsafe(totalprice_index))) {
			continue;
		}
		const auto orderkey = orderkeys[orderkey_index];
		if (!state.IsQualifyingOrder(orderkey)) {
			continue;
		}
		const auto custkey = custkeys[custkey_index];
		worker.MarkCustkey(custkey);
		worker.orders.push_back(QualifyingOrderRow {
		    orderkey,
		    OrderMetadata {custkey, duckdb::date_t(orderdates[orderdate_index]),
		                   duckdb::hugeint_t(totalprices[totalprice_index])}});
	}
}

void MergeCustkeyBitmap(std::vector<uint8_t> &global, const std::vector<uint8_t> &local) {
	if (local.size() > global.size()) {
		global.resize(local.size(), 0);
	}
	for (size_t index = 0; index < local.size(); index++) {
		if (local[index] != 0) {
			global[index] = 1;
		}
	}
}

void BuildCustomerState(duckdb::ClientContext &context, const duckdb::FunctionData &customer_bind,
                        const std::vector<uint8_t> &needed_custkeys, std::vector<std::string> &customer_names) {
	if (needed_custkeys.empty()) {
		return;
	}
	customer_names.resize(needed_custkeys.size());

	auto source = CreateNativeChunkSource(context, customer_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		auto &custkey_vector = chunk.data[Q18CustomerChunkLayout::CUSTKEY_INDEX];
		auto &name_vector = chunk.data[Q18CustomerChunkLayout::NAME_INDEX];

		if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(name_vector)) {
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto custkey = custkeys[row];
				if (custkey < 0 || static_cast<size_t>(custkey) >= needed_custkeys.size() ||
				    needed_custkeys[static_cast<size_t>(custkey)] == 0) {
					continue;
				}
				const auto &name_value = names[row];
				customer_names[static_cast<size_t>(custkey)] =
				    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
			}
			continue;
		}

		if (IsDictionaryOverFlat(custkey_vector) && IsDictionaryOverFlat(name_vector)) {
			auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
			auto &name_sel = duckdb::DictionaryVector::SelVector(name_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(name_vector));
			auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
			auto &name_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(name_vector));
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto custkey_index = custkey_sel.get_index(row);
				const auto name_index = name_sel.get_index(row);
				if (!custkey_validity.RowIsValid(custkey_index) || !name_validity.RowIsValid(name_index)) {
					continue;
				}
				const auto custkey = custkeys[custkey_index];
				if (custkey < 0 || static_cast<size_t>(custkey) >= needed_custkeys.size() ||
				    needed_custkeys[static_cast<size_t>(custkey)] == 0) {
					continue;
				}
				const auto &name_value = names[name_index];
				customer_names[static_cast<size_t>(custkey)] =
				    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
			}
			continue;
		}

		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat name_format;
		chunk.data[Q18CustomerChunkLayout::CUSTKEY_INDEX].ToUnifiedFormat(chunk.size(), custkey_format);
		chunk.data[Q18CustomerChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (custkey < 0 || static_cast<size_t>(custkey) >= needed_custkeys.size() ||
			    needed_custkeys[static_cast<size_t>(custkey)] == 0) {
				continue;
			}
			const auto &name_value = names[name_index];
			customer_names[static_cast<size_t>(custkey)] =
			    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
		}
	}
}

class PartitionAggregateTask final : public duckdb::BaseExecutorTask {
public:
	PartitionAggregateTask(duckdb::TaskExecutor &executor, duckdb::idx_t partition_id_p, const OrderPartitionPlan &plan_p,
	                       const std::vector<LineitemWorkerState> &workers_p, Q18FrozenState &frozen_p,
	                       PartitionAggregateProfile &profile_p)
	    : BaseExecutorTask(executor),
	      partition_id(partition_id_p),
	      plan(plan_p),
	      workers(workers_p),
	      frozen(frozen_p),
	      profile(profile_p) {
	}

	void ExecuteTask() override {
		using clock = std::chrono::steady_clock;
		const auto t0 = clock::now();
		const auto begin = plan.PartitionBegin(partition_id);
		const auto end = plan.PartitionEnd(partition_id);
		const auto range = end - begin;
		if (range <= 0) {
			profile.process_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			return;
		}

		std::vector<int64_t> local_totals(static_cast<size_t>(range), 0);
		std::vector<uint8_t> touched(static_cast<size_t>(range), 0);
		std::vector<int64_t> touched_offsets;
		touched_offsets.reserve(1024);
		for (const auto &worker : workers) {
			const auto &fragment = worker.partition_fragments[partition_id];
			profile.updates_merged += fragment.size();
			for (const auto &update : fragment) {
				const auto offset = static_cast<int64_t>(update.orderkey) - begin;
				if (offset < 0 || offset >= range) {
					continue;
				}
				const auto index = static_cast<size_t>(offset);
				if (touched[index] == 0) {
					touched[index] = 1;
					touched_offsets.push_back(offset);
				}
				local_totals[index] += update.quantity_raw;
			}
		}

		for (const auto offset : touched_offsets) {
			const auto sum = local_totals[static_cast<size_t>(offset)];
			if (sum <= frozen.quantity_threshold_raw) {
				continue;
			}
			const auto orderkey = begin + offset;
			frozen.order_quantity_totals[static_cast<size_t>(orderkey)] = sum;
			frozen.order_qualifying[static_cast<size_t>(orderkey)] = 1;
			profile.qualifying_orders += 1;
		}
		profile.process_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
	}

private:
	duckdb::idx_t partition_id;
	const OrderPartitionPlan &plan;
	const std::vector<LineitemWorkerState> &workers;
	Q18FrozenState &frozen;
	PartitionAggregateProfile &profile;
};

void PrintQ18ParallelProfile(const Q18ParallelProfile &profile) {
	std::cerr << "q18_parallel_profile"
	          << " lineitem_scan_ns=" << profile.lineitem_scan_ns
	          << " partition_aggregate_ns=" << profile.partition_aggregate_ns << " orders_scan_ns=" << profile.orders_scan_ns
	          << " orders_merge_ns=" << profile.orders_merge_ns << " customer_ns=" << profile.customer_ns
	          << " finalize_ns=" << profile.finalize_ns << " worker_count=" << profile.worker_count
	          << " partition_count=" << profile.partition_count << " order_capacity=" << profile.order_capacity
	          << " lineitem_rows=" << profile.lineitem_rows << " quantity_updates=" << profile.quantity_updates
	          << " atomic_ops=" << profile.atomic_ops << " update_bytes=" << profile.update_bytes
	          << " partition_state_bytes=" << profile.partition_state_bytes
	          << " qualifying_orders=" << profile.qualifying_orders
	          << " lineitem_flat_rows=" << profile.lineitem_flat_rows
	          << " lineitem_dict_rows=" << profile.lineitem_dict_rows
	          << " lineitem_fallback_rows=" << profile.lineitem_fallback_rows
	          << " min_partition_ns=" << profile.min_partition_ns << " max_partition_ns=" << profile.max_partition_ns
	          << " min_worker_lineitem_ns=" << profile.min_worker_lineitem_ns
	          << " max_worker_lineitem_ns=" << profile.max_worker_lineitem_ns << "\n";
}

class Q18ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q18ParallelKernel(Q18Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 3 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2]) {
			throw duckdb::InternalException("Q18 parallel expects bound lineitem, orders, customer scans");
		}

		using clock = std::chrono::steady_clock;
		Q18ParallelProfile profile;
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);
		const auto order_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[1]);
		const auto partition_plan = OrderPartitionPlan::Build(order_capacity, worker_count);
		profile.partition_count = partition_plan.num_partitions;
		profile.order_capacity = static_cast<size_t>(partition_plan.capacity);

		Q18FrozenState frozen(params.quantity_threshold, order_capacity);

		if (worker_count == 1) {
			std::vector<uint8_t> touched_orders(frozen.order_quantity_totals.size(), 0);
			std::vector<int64_t> touched_orderkeys;
			touched_orderkeys.reserve(1500000);
			const auto lineitem_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[0], worker_count,
			                     [&](duckdb::idx_t, duckdb::DataChunk &chunk) {
				                     profile.lineitem_rows += chunk.size();
				                     profile.quantity_updates += chunk.size();
				                     AccumulateLineitemChunkDirect(chunk, frozen.order_quantity_totals, touched_orders,
				                                                   touched_orderkeys);
			                     });
			profile.lineitem_scan_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

			const auto partition_t0 = clock::now();
			frozen.FinalizeQualifyingTouched(touched_orderkeys);
			profile.qualifying_orders = 0;
			for (const auto orderkey : touched_orderkeys) {
				if (frozen.IsQualifyingOrder(orderkey)) {
					profile.qualifying_orders += 1;
				}
			}
			profile.partition_aggregate_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - partition_t0).count();
		} else {
			profile.partition_state_bytes =
			    static_cast<size_t>(partition_plan.keys_per_partition) * sizeof(int64_t);

			std::vector<LineitemWorkerState> lineitem_workers(worker_count, LineitemWorkerState(partition_plan.num_partitions));
			const auto lineitem_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[0], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     const auto chunk_t0 = clock::now();
				                     auto &worker = lineitem_workers[static_cast<size_t>(worker_id)];
				                     AccumulateLineitemChunk(chunk, partition_plan, worker);
				                     worker.stats.process_ns +=
				                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0)
				                             .count();
			                     });
			profile.lineitem_scan_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

			std::vector<int64_t> worker_lineitem_ns(lineitem_workers.size(), 0);
			for (size_t worker_id = 0; worker_id < lineitem_workers.size(); worker_id++) {
				const auto &worker = lineitem_workers[worker_id];
				profile.lineitem_rows += worker.stats.rows_in;
				profile.quantity_updates += worker.stats.updates;
				profile.update_bytes += worker.stats.updates * sizeof(QuantityUpdate);
				profile.lineitem_flat_rows += worker.stats.flat_rows;
				profile.lineitem_dict_rows += worker.stats.dict_rows;
				profile.lineitem_fallback_rows += worker.stats.fallback_rows;
				worker_lineitem_ns[worker_id] = worker.stats.process_ns;
			}
			if (!worker_lineitem_ns.empty()) {
				profile.min_worker_lineitem_ns = *std::min_element(worker_lineitem_ns.begin(), worker_lineitem_ns.end());
				profile.max_worker_lineitem_ns = *std::max_element(worker_lineitem_ns.begin(), worker_lineitem_ns.end());
			}

			std::vector<PartitionAggregateProfile> partition_profiles(partition_plan.num_partitions);
			const auto partition_t0 = clock::now();
			{
				duckdb::TaskExecutor executor(context);
				for (duckdb::idx_t partition_id = 0; partition_id < partition_plan.num_partitions; partition_id++) {
					executor.ScheduleTask(duckdb::make_uniq<PartitionAggregateTask>(
					    executor, partition_id, partition_plan, lineitem_workers, frozen, partition_profiles[partition_id]));
				}
				executor.WorkOnTasks();
			}
			profile.partition_aggregate_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - partition_t0).count();
			lineitem_workers.clear();
			lineitem_workers.shrink_to_fit();

			std::vector<int64_t> partition_ns;
			partition_ns.reserve(partition_profiles.size());
			for (const auto &partition_profile : partition_profiles) {
				profile.qualifying_orders += partition_profile.qualifying_orders;
				partition_ns.push_back(partition_profile.process_ns);
			}
			frozen.has_qualifying_orders = profile.qualifying_orders > 0;
			if (!partition_ns.empty()) {
				profile.min_partition_ns = *std::min_element(partition_ns.begin(), partition_ns.end());
				profile.max_partition_ns = *std::max_element(partition_ns.begin(), partition_ns.end());
			}
		}

		std::vector<CacheLinePadded<Q18OrdersWorkerState>> order_workers(worker_count);
		const auto orders_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     AccumulateOrdersChunk(chunk, frozen, order_workers[static_cast<size_t>(worker_id)].value);
		                     });
		profile.orders_scan_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();

		const auto orders_merge_t0 = clock::now();
		std::vector<QualifyingOrderRow> orders;
		std::vector<uint8_t> needed_custkeys;
		for (const auto &worker : order_workers) {
			orders.insert(orders.end(), worker.value.orders.begin(), worker.value.orders.end());
			MergeCustkeyBitmap(needed_custkeys, worker.value.needed_custkeys);
		}
		order_workers.clear();
		order_workers.shrink_to_fit();
		profile.orders_merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_merge_t0).count();

		const auto customer_t0 = clock::now();
		std::vector<std::string> customer_names;
		BuildCustomerState(context, *bound_inputs[2], needed_custkeys, customer_names);
		profile.customer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - customer_t0).count();

		const auto finalize_t0 = clock::now();
		output_rows.clear();
		output_rows.reserve(orders.size());
		for (const auto &entry : orders) {
			const auto &metadata = entry.metadata;
			if (metadata.custkey < 0 || static_cast<size_t>(metadata.custkey) >= customer_names.size()) {
				continue;
			}
			const auto &name = customer_names[static_cast<size_t>(metadata.custkey)];
			if (name.empty()) {
				continue;
			}
			Q18OutputRow row;
			row.name = name;
			row.custkey = metadata.custkey;
			row.orderkey = entry.orderkey;
			row.orderdate = metadata.orderdate;
			row.totalprice = metadata.totalprice;
			row.quantity_sum = duckdb::hugeint_t(frozen.OrderQuantity(entry.orderkey));
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		profile.finalize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - finalize_t0).count();

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			PrintQ18ParallelProfile(profile);
		}
	}

	void Finalize() override {
		output_offset = 0;
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= output_rows.size()) {
			return false;
		}
		const duckdb::idx_t remaining = output_rows.size() - output_offset;
		const duckdb::idx_t count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &entry = output_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value(entry.name));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.custkey));
			output.SetValue(2, row, duckdb::Value::BIGINT(entry.orderkey));
			output.SetValue(3, row, duckdb::Value::DATE(entry.orderdate));
			output.SetValue(4, row, duckdb::Value::DECIMAL(entry.totalprice, 15, 2));
			output.SetValue(5, row, duckdb::Value::DECIMAL(entry.quantity_sum, 38, 2));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q18Parameters params;
	std::vector<Q18OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ18ParallelKernel(const Q18Parameters &params) {
	return duckdb::make_uniq<Q18ParallelKernel>(params);
}

} // namespace accelerator_codegen
