#include <stdexcept>

#include "accelerator_codegen/tpch_q3_incremental.hpp"
#include "accelerator_codegen/tpch_q3_write_policy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kQ3MaxLightCaptureRows = 1024;

const aux_state::ColumnRef *FindColumn(const aux_state::TableDependency &dependency, const std::string &name) {
	for (const auto &column : dependency.watched_columns) {
		if (column.column_name == name) return &column;
	}
	return nullptr;
}

const aux_state::TableDependency *FindTableDependency(const std::vector<aux_state::TableDependency> &dependencies,
                                                      const std::string &table_name) {
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == table_name) return &dependency;
	}
	return nullptr;
}

std::optional<int64_t> ReadInt64Value(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	if (value.type().id() == duckdb::LogicalTypeId::DECIMAL) {
		switch (value.type().InternalType()) {
		case duckdb::PhysicalType::INT16:
			return value.GetValueUnsafe<int16_t>();
		case duckdb::PhysicalType::INT32:
			return value.GetValueUnsafe<int32_t>();
		case duckdb::PhysicalType::INT64:
			return value.GetValueUnsafe<int64_t>();
		default:
			return std::nullopt;
		}
	}
	return value.GetValue<int64_t>();
}

std::optional<duckdb::date_t> ReadDateValue(const duckdb::Value &value) {
	if (value.IsNull()) return std::nullopt;
	return value.GetValue<duckdb::date_t>();
}

std::vector<duckdb::Value> ValuesForColumn(const duckdb::TableMutationRow &row, duckdb::idx_t physical_index) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == physical_index) values.push_back(projected.value);
	}
	return values;
}

std::optional<int64_t> PickInt64Column(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                       bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) return std::nullopt;
	return ReadInt64Value(values.size() == 1 || !use_new_value ? values.front() : values.back());
}

std::optional<duckdb::date_t> PickDateColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                             bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) return std::nullopt;
	return ReadDateValue(values.size() == 1 || !use_new_value ? values.front() : values.back());
}

struct LineitemSnapshot {
	int64_t orderkey = 0;
	int32_t shipdate_day = 0;
	int64_t price = 0;
	int64_t discount = 0;
};

struct Q3PendingLineitemOp {
	int8_t sign = 0;
	Q3PartitionKey partition = 0;
	int64_t orderkey = 0;
	int32_t shipdate_day = 0;
	int64_t price = 0;
	int64_t discount = 0;
};

struct Q3PendingLineitemLightOp {
	LineitemSnapshot snapshot;
	int8_t sign = 0;
};

std::optional<LineitemSnapshot> ReadLineitemSnapshot(const duckdb::TableMutationRow &row,
                                                     const aux_state::TableDependency &dependency, bool use_new_value) {
	const auto *orderkey_column = FindColumn(dependency, "l_orderkey");
	const auto *price_column = FindColumn(dependency, "l_extendedprice");
	const auto *discount_column = FindColumn(dependency, "l_discount");
	const auto *shipdate_column = FindColumn(dependency, "l_shipdate");
	if (!orderkey_column || !price_column || !discount_column || !shipdate_column) return std::nullopt;
	auto orderkey = PickInt64Column(row, *orderkey_column, use_new_value);
	auto price = PickInt64Column(row, *price_column, use_new_value);
	auto discount = PickInt64Column(row, *discount_column, use_new_value);
	auto shipdate = PickDateColumn(row, *shipdate_column, use_new_value);
	if (!orderkey || !price || !discount || !shipdate) {
		return std::nullopt;
	}
	LineitemSnapshot snapshot;
	snapshot.orderkey = *orderkey;
	snapshot.price = *price;
	snapshot.discount = *discount;
	snapshot.shipdate_day = shipdate->days;
	return snapshot;
}

void NotePartition(aux_state::InvalidationPlan &plan, Q3PartitionKey partition) {
	if (partition == 0) return;
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(partition);
}

std::optional<Q3PartitionKey> LookupPartitionForOrder(aux_state::PartitionedStateStore &store, int64_t orderkey) {
	std::optional<Q3PartitionKey> found;
	store.ForEachSlot([&](Q3PartitionKey key, aux_state::PartitionStatus status, const std::shared_ptr<const void> &payload,
	                      uint64_t) {
		if (found || status != aux_state::PartitionStatus::Valid || !payload) return;
		const auto &partition = *std::static_pointer_cast<const Q3SegmentPartition>(payload);
		if (partition.orders.find(orderkey) != partition.orders.end()) found = key;
	});
	return found;
}

std::optional<Q3PartitionKey> ResolvePartitionForOrder(duckdb::ClientContext &context, int64_t orderkey) {
	NativeScanSpec order_spec;
	order_spec.table_name = "orders";
	order_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	};
	order_spec.filters = {{"o_orderkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(orderkey)}};
	auto order_bind = BindNativeScan(context, order_spec);
	auto order_source = CreateNativeChunkSource(context, *order_bind);
	std::optional<int64_t> custkey;
	while (order_source->Next()) {
		auto &chunk = order_source->Chunk();
		duckdb::UnifiedVectorFormat custkeys;
		chunk.data[1].ToUnifiedFormat(chunk.size(), custkeys);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto idx = custkeys.sel->get_index(row);
			if (!custkeys.validity.RowIsValid(idx)) continue;
			custkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkeys)[idx];
		}
	}
	if (!custkey) return std::nullopt;

	NativeScanSpec customer_spec;
	customer_spec.table_name = "customer";
	customer_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_mktsegment", "mktsegment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	customer_spec.filters = {{"c_custkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(*custkey)}};
	auto customer_bind = BindNativeScan(context, customer_spec);
	auto customer_source = CreateNativeChunkSource(context, *customer_bind);
	while (customer_source->Next()) {
		auto &chunk = customer_source->Chunk();
		duckdb::UnifiedVectorFormat segments;
		chunk.data[1].ToUnifiedFormat(chunk.size(), segments);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto idx = segments.sel->get_index(row);
			if (!segments.validity.RowIsValid(idx)) continue;
			const auto &segment = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(segments)[idx];
			return PartitionKeyFromMktsegment(segment.GetString());
		}
	}
	return std::nullopt;
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) return true;
	}
	return false;
}

std::unordered_map<Q3PartitionKey, Q3PartitionDelta> FoldLineitemOps(const std::vector<Q3PendingLineitemOp> &ops) {
	std::unordered_map<Q3PartitionKey, Q3PartitionDelta> folded;
	for (const auto &op : ops) {
		auto &delta = folded[op.partition];
		delta.bucket_deltas.emplace_back(op.orderkey, op.shipdate_day,
		                                 DiscountedRevenueRaw(op.price, op.discount) * static_cast<Q3AccT>(op.sign));
	}
	return folded;
}

void ApplyDeltaToPartition(Q3SegmentPartition &partition, const Q3PartitionDelta &delta) {
	for (const auto &orderkey : delta.removals) {
		partition.orders.erase(orderkey);
	}
	for (const auto &entry : delta.upserts) {
		partition.orders[entry.first] = entry.second;
	}
	for (const auto &bucket_delta : delta.bucket_deltas) {
		const auto orderkey = std::get<0>(bucket_delta);
		const auto shipdate_day = std::get<1>(bucket_delta);
		const auto revenue_delta = std::get<2>(bucket_delta);
		auto it = partition.orders.find(orderkey);
		if (it == partition.orders.end()) continue;
		AddAccumulatedRevenue(it->second.shipdate_buckets[shipdate_day], revenue_delta, 1);
	}
}

} // namespace

struct Q3IncrementalTxnBuffer::Impl {
	std::vector<Q3PendingLineitemOp> row_ops;
	std::vector<Q3PendingLineitemLightOp> pending_row_light_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t cheap_order_fanout = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

Q3IncrementalTxnBuffer::Q3IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {}
Q3IncrementalTxnBuffer::~Q3IncrementalTxnBuffer() = default;
Q3IncrementalTxnBuffer::Q3IncrementalTxnBuffer(Q3IncrementalTxnBuffer &&) noexcept = default;
Q3IncrementalTxnBuffer &Q3IncrementalTxnBuffer::operator=(Q3IncrementalTxnBuffer &&) noexcept = default;

void Q3IncrementalTxnBuffer::Clear() {
	if (!impl) impl = std::make_unique<Impl>();
	impl->row_ops.clear();
	impl->pending_row_light_ops.clear();
	impl->extra_partitions = {};
	impl->cheap_rows_touched = 0;
	impl->estimated_lineitem_scan_rows = 0;
	impl->cheap_order_fanout = 0;
	impl->poisoned = false;
	impl->heavy_capture_done = false;
	impl->bulk_mutation = false;
}

bool Q3IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || impl->cheap_rows_touched > 0 || !impl->pending_row_light_ops.empty());
}

uint64_t Q3IncrementalTxnBuffer::CheapRowsTouched() const { return impl ? impl->cheap_rows_touched : 0; }
uint64_t Q3IncrementalTxnBuffer::CheapOrderFanout() const { return impl ? impl->cheap_order_fanout : 0; }
uint64_t Q3IncrementalTxnBuffer::EstimatedLineitemScanRows() const { return impl ? impl->estimated_lineitem_scan_rows : 0; }
bool Q3IncrementalTxnBuffer::HeavyCaptureDone() const { return impl && impl->heavy_capture_done; }
bool Q3IncrementalTxnBuffer::BulkMutation() const { return impl && impl->bulk_mutation; }
aux_state::InvalidationPlan Q3IncrementalTxnBuffer::ExtraInvalidationPlan() const { return impl ? impl->extra_partitions : aux_state::InvalidationPlan {}; }
bool Q3IncrementalTxnBuffer::IsPoisoned() const { return impl && impl->poisoned; }
uint64_t Q3IncrementalTxnBuffer::RowOpCount() const { return impl ? impl->row_ops.size() : 0; }
uint64_t Q3IncrementalTxnBuffer::CrossPartitionOpCount() const { return 0; }
uint64_t Q3IncrementalTxnBuffer::PendingLightOpCount() const { return impl ? impl->pending_row_light_ops.size() : 0; }

void MaybeMergePendingQ3LightOps(duckdb::ClientContext &context, const std::vector<aux_state::TableDependency> &dependencies,
                               Q3IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation || buffer.impl->pending_row_light_ops.empty()) return;
	if (buffer.impl->cheap_rows_touched > kQ3MaxLightCaptureRows) return;
	Q3FinalizeIncrementalCapture(context, dependencies, buffer);
}

void Q3AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q3IncrementalTxnBuffer &buffer) {
	if (!buffer.impl) buffer.impl = std::make_unique<Q3IncrementalTxnBuffer::Impl>();
	if (buffer.impl->poisoned) return;
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (!lineitem || event.table_oid != lineitem->table_oid) return;

	auto touch = [&](size_t count) {
		buffer.impl->cheap_rows_touched += count;
		if (buffer.impl->cheap_rows_touched > kQ3MaxLightCaptureRows) buffer.impl->bulk_mutation = true;
	};

	auto append = [&](const LineitemSnapshot &snapshot, int8_t sign) {
		if (buffer.impl->bulk_mutation) {
			return;
		}
		auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
		auto &store = registry->GetStore(kQ3AuxAcceleratorId);
		auto partition = LookupPartitionForOrder(store, snapshot.orderkey);
		if (!partition) {
			partition = ResolvePartitionForOrder(context, snapshot.orderkey);
		}
		if (!partition) {
			buffer.impl->pending_row_light_ops.push_back(Q3PendingLineitemLightOp {snapshot, sign});
			return;
		}
		NotePartition(buffer.impl->extra_partitions, *partition);
		buffer.impl->row_ops.push_back(
		    Q3PendingLineitemOp {sign, *partition, snapshot.orderkey, snapshot.shipdate_day, snapshot.price, snapshot.discount});
	};

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		touch(event.rows.size());
		for (const auto &row : event.rows) {
			auto snapshot = ReadLineitemSnapshot(row, *lineitem, true);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			append(*snapshot, 1);
		}
		break;
	case duckdb::TableMutationKind::DELETE:
		touch(event.rows.size());
		for (const auto &row : event.rows) {
			auto snapshot = ReadLineitemSnapshot(row, *lineitem, false);
			if (!snapshot) { buffer.impl->poisoned = true; return; }
			append(*snapshot, -1);
		}
		break;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, *lineitem)) return;
		touch(event.rows.size());
		for (const auto &row : event.rows) {
			auto old_snapshot = ReadLineitemSnapshot(row, *lineitem, false);
			auto new_snapshot = ReadLineitemSnapshot(row, *lineitem, true);
			if (!old_snapshot || !new_snapshot) { buffer.impl->poisoned = true; return; }
			append(*old_snapshot, -1);
			append(*new_snapshot, 1);
		}
		break;
	default:
		return;
	}
	MaybeMergePendingQ3LightOps(context, dependencies, buffer);
}

void Q3AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                     const std::vector<aux_state::TableDependency> &dependencies,
                                     Q3IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)event;
	(void)dependencies;
	(void)buffer;
}

void Q3AccumulateCustomerMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q3IncrementalTxnBuffer &buffer) {
	(void)context;
	if (!buffer.impl) buffer.impl = std::make_unique<Q3IncrementalTxnBuffer::Impl>();
	const auto *customer = FindTableDependency(dependencies, "customer");
	if (!customer || event.table_oid != customer->table_oid) return;
	if (event.kind == duckdb::TableMutationKind::UPDATE && UpdateTouchesRelevantColumns(event, *customer)) {
		buffer.impl->cheap_order_fanout++;
	}
}

bool Q3FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q3IncrementalTxnBuffer &buffer) {
	(void)dependencies;
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation || buffer.impl->pending_row_light_ops.empty()) {
		return !buffer.impl || (!buffer.impl->poisoned && !buffer.impl->row_ops.empty());
	}
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ3AuxAcceleratorId);
	for (const auto &pending : buffer.impl->pending_row_light_ops) {
		auto partition = LookupPartitionForOrder(store, pending.snapshot.orderkey);
		if (!partition) partition = ResolvePartitionForOrder(context, pending.snapshot.orderkey);
		if (!partition) {
			buffer.impl->poisoned = true;
			buffer.impl->row_ops.clear();
			return false;
		}
		NotePartition(buffer.impl->extra_partitions, *partition);
		buffer.impl->row_ops.push_back(
		    Q3PendingLineitemOp {pending.sign, *partition, pending.snapshot.orderkey, pending.snapshot.shipdate_day,
		                         pending.snapshot.price, pending.snapshot.discount});
	}
	buffer.impl->pending_row_light_ops.clear();
	buffer.impl->heavy_capture_done = true;
	return !buffer.impl->poisoned;
}

bool ComputeQ3IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q3IncrementalTxnBuffer &buffer,
                               Q3ComputedIncrementalDelta &out) {
	(void)context;
	out = {};
	if (!buffer.impl || buffer.impl->poisoned) return false;
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) return false;
	auto folded = FoldLineitemOps(buffer.impl->row_ops);
	std::unordered_set<Q3PartitionKey> partitions;
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) partitions.insert(partition);
	}
	for (const auto &entry : folded) partitions.insert(entry.first);
	for (auto partition : buffer.impl->extra_partitions.partitions) partitions.insert(partition);
	if (partitions.empty() && buffer.impl->row_ops.empty()) {
		out.success = true;
		return true;
	}
	for (auto partition : partitions) {
		const auto folded_it = folded.find(partition);
		if (folded_it == folded.end()) continue;
		if (store.GetStatus(partition) == aux_state::PartitionStatus::Invalid) return false;
		Q3SegmentPartition partition_state;
		if (store.GetStatus(partition) == aux_state::PartitionStatus::Valid) {
			auto payload = store.GetReadablePayload(partition);
			if (!payload) return false;
			partition_state = *std::static_pointer_cast<const Q3SegmentPartition>(payload);
		}
		ApplyDeltaToPartition(partition_state, folded_it->second);
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = partition;
		update.payload = std::make_shared<Q3SegmentPartition>(std::move(partition_state));
		update.expected_partition_version = store.GetPartitionVersion(partition);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(partition);
	}
	out.success = true;
	RecordQ3IncrementalApplyAttempt(true, 0);
	return true;
}

void PublishQ3IncrementalDelta(aux_state::PartitionedStateStore &store, const Q3ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch) {
	if (!delta.success) return;
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

Q3IncrementalTxnBuffer &GetQ3IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kQ3AuxAcceleratorId);
	auto *q3 = dynamic_cast<Q3IncrementalTxnBuffer *>(&base);
	if (!q3) throw std::runtime_error("incremental buffer for tpch_q3_aux has unexpected type");
	return *q3;
}

aux_state::InvalidationPlan AccumulateQ3Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetQ3IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsQ3DependentTableMutation(event, dependencies);
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	const auto *orders = FindTableDependency(dependencies, "orders");
	const auto *customer = FindTableDependency(dependencies, "customer");
	if (lineitem && event.table_oid == lineitem->table_oid) {
		Q3AccumulateLineitemMutationLight(context, event, dependencies, incremental);
	} else if (orders && event.table_oid == orders->table_oid) {
		Q3AccumulateOrdersMutationLight(context, event, dependencies, incremental);
	} else if (customer && event.table_oid == customer->table_oid) {
		Q3AccumulateCustomerMutationLight(context, event, dependencies, incremental);
	}
	if (dependent) {
		RecordQ3DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordQ3WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
