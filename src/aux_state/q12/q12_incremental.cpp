#include <stdexcept>

#include "accelerator_codegen/tpch_q12_incremental.hpp"
#include "accelerator_codegen/tpch_q12_write_policy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kQ12MaxLightCaptureRows = 1024;

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
	if (value.IsNull()) return std::nullopt;
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

std::optional<std::string> PickStringColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                            bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) return std::nullopt;
	const auto &picked = values.size() == 1 || !use_new_value ? values.front() : values.back();
	if (picked.IsNull()) return std::nullopt;
	return picked.GetValue<std::string>();
}

struct LineitemSnapshot {
	int64_t orderkey = 0;
	std::string shipmode;
	duckdb::date_t commitdate {};
	duckdb::date_t receiptdate {};
	duckdb::date_t shipdate {};
};

struct Q12PendingLineitemOp {
	int8_t sign = 0;
	Q12PartitionKey partition = 0;
	std::string shipmode;
	Q12PriorityClass priority = Q12PriorityClass::Absent;
};

struct Q12PendingLineitemLightOp {
	LineitemSnapshot snapshot;
	int8_t sign = 0;
};

void NotePartition(aux_state::InvalidationPlan &plan, Q12PartitionKey partition) {
	if (partition == 0) return;
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(partition);
}

std::optional<Q12PriorityClass> LookupOrderPriority(duckdb::ClientContext &context, int64_t orderkey) {
	NativeScanSpec order_spec;
	order_spec.table_name = "orders";
	order_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderpriority", "orderpriority", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	order_spec.filters = {{"o_orderkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(orderkey)}};
	auto order_bind = BindNativeScan(context, order_spec);
	auto order_source = CreateNativeChunkSource(context, *order_bind);
	while (order_source->Next()) {
		auto &chunk = order_source->Chunk();
		duckdb::UnifiedVectorFormat priorities;
		chunk.data[1].ToUnifiedFormat(chunk.size(), priorities);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto idx = priorities.sel->get_index(row);
			if (!priorities.validity.RowIsValid(idx)) {
				return Q12PriorityClass::Neither;
			}
			const auto &value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(priorities)[idx];
			return ClassifyQ12Priority(value, false);
		}
	}
	return std::nullopt;
}

std::optional<LineitemSnapshot> ReadLineitemSnapshot(const duckdb::TableMutationRow &row,
                                                     const aux_state::TableDependency &dependency, bool use_new_value) {
	const auto *orderkey_column = FindColumn(dependency, "l_orderkey");
	const auto *shipmode_column = FindColumn(dependency, "l_shipmode");
	const auto *commit_column = FindColumn(dependency, "l_commitdate");
	const auto *receipt_column = FindColumn(dependency, "l_receiptdate");
	const auto *ship_column = FindColumn(dependency, "l_shipdate");
	if (!orderkey_column || !shipmode_column || !commit_column || !receipt_column || !ship_column) return std::nullopt;
	auto orderkey = PickInt64Column(row, *orderkey_column, use_new_value);
	auto shipmode = PickStringColumn(row, *shipmode_column, use_new_value);
	auto commitdate = PickDateColumn(row, *commit_column, use_new_value);
	auto receiptdate = PickDateColumn(row, *receipt_column, use_new_value);
	auto shipdate = PickDateColumn(row, *ship_column, use_new_value);
	if (!orderkey || !shipmode || !commitdate || !receiptdate || !shipdate) return std::nullopt;
	LineitemSnapshot snapshot;
	snapshot.orderkey = *orderkey;
	snapshot.shipmode = *shipmode;
	snapshot.commitdate = *commitdate;
	snapshot.receiptdate = *receiptdate;
	snapshot.shipdate = *shipdate;
	return snapshot;
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) return true;
	}
	return false;
}

std::unordered_map<Q12PartitionKey, Q12PartitionDelta> FoldLineitemOps(const std::vector<Q12PendingLineitemOp> &ops) {
	std::unordered_map<Q12PartitionKey, Q12PartitionDelta> folded;
	for (const auto &op : ops) {
		auto &delta = folded[op.partition];
		delta.counter_deltas.push_back(Q12CounterDelta {op.shipmode, op.priority, op.sign});
	}
	return folded;
}

void ApplyDeltaToPartition(Q12DayPartition &partition, const Q12PartitionDelta &delta) {
	for (const auto &counter : delta.counter_deltas) {
		auto &stats = partition.shipmodes[counter.shipmode];
		ApplyQ12LineitemContribution(stats, counter.priority, counter.sign);
	}
}

std::optional<Q12PendingLineitemOp> MakeOpFromSnapshot(duckdb::ClientContext &context, const LineitemSnapshot &snapshot,
                                                       int8_t sign) {
	if (!Q12LineitemQualifies(snapshot.commitdate, snapshot.receiptdate, snapshot.shipdate)) {
		return std::nullopt;
	}
	auto priority = LookupOrderPriority(context, snapshot.orderkey);
	if (!priority || *priority == Q12PriorityClass::Absent) {
		return std::nullopt;
	}
	Q12PendingLineitemOp op;
	op.sign = sign;
	op.partition = PartitionKeyFromReceiptdate(snapshot.receiptdate);
	op.shipmode = snapshot.shipmode;
	op.priority = *priority;
	return op;
}

} // namespace

struct Q12IncrementalTxnBuffer::Impl {
	std::vector<Q12PendingLineitemOp> row_ops;
	std::vector<Q12PendingLineitemLightOp> pending_row_light_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t cheap_order_fanout = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

Q12IncrementalTxnBuffer::Q12IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {}
Q12IncrementalTxnBuffer::~Q12IncrementalTxnBuffer() = default;
Q12IncrementalTxnBuffer::Q12IncrementalTxnBuffer(Q12IncrementalTxnBuffer &&) noexcept = default;
Q12IncrementalTxnBuffer &Q12IncrementalTxnBuffer::operator=(Q12IncrementalTxnBuffer &&) noexcept = default;

void Q12IncrementalTxnBuffer::Clear() {
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

bool Q12IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || impl->cheap_rows_touched > 0 || !impl->pending_row_light_ops.empty());
}

uint64_t Q12IncrementalTxnBuffer::CheapRowsTouched() const { return impl ? impl->cheap_rows_touched : 0; }
uint64_t Q12IncrementalTxnBuffer::CheapOrderFanout() const { return impl ? impl->cheap_order_fanout : 0; }
uint64_t Q12IncrementalTxnBuffer::EstimatedLineitemScanRows() const { return impl ? impl->estimated_lineitem_scan_rows : 0; }
bool Q12IncrementalTxnBuffer::HeavyCaptureDone() const { return impl && impl->heavy_capture_done; }
bool Q12IncrementalTxnBuffer::BulkMutation() const { return impl && impl->bulk_mutation; }
aux_state::InvalidationPlan Q12IncrementalTxnBuffer::ExtraInvalidationPlan() const { return impl ? impl->extra_partitions : aux_state::InvalidationPlan {}; }
bool Q12IncrementalTxnBuffer::IsPoisoned() const { return impl && impl->poisoned; }
uint64_t Q12IncrementalTxnBuffer::RowOpCount() const { return impl ? impl->row_ops.size() : 0; }
uint64_t Q12IncrementalTxnBuffer::CrossPartitionOpCount() const { return 0; }
uint64_t Q12IncrementalTxnBuffer::PendingLightOpCount() const { return impl ? impl->pending_row_light_ops.size() : 0; }

void MaybeMergePendingQ12LightOps(duckdb::ClientContext &context, const std::vector<aux_state::TableDependency> &dependencies,
                                  Q12IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation || buffer.impl->pending_row_light_ops.empty()) return;
	if (buffer.impl->cheap_rows_touched > kQ12MaxLightCaptureRows) return;
	Q12FinalizeIncrementalCapture(context, dependencies, buffer);
}

void Q12AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies,
                                        Q12IncrementalTxnBuffer &buffer) {
	if (!buffer.impl) buffer.impl = std::make_unique<Q12IncrementalTxnBuffer::Impl>();
	if (buffer.impl->poisoned) return;
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (!lineitem || event.table_oid != lineitem->table_oid) return;

	auto touch = [&](size_t count) {
		buffer.impl->cheap_rows_touched += count;
		if (buffer.impl->cheap_rows_touched > kQ12MaxLightCaptureRows) buffer.impl->bulk_mutation = true;
	};

	auto append = [&](const LineitemSnapshot &snapshot, int8_t sign) {
		if (buffer.impl->bulk_mutation) return;
		if (auto op = MakeOpFromSnapshot(context, snapshot, sign)) {
			NotePartition(buffer.impl->extra_partitions, op->partition);
			buffer.impl->row_ops.push_back(*op);
		}
	};

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		touch(event.rows.size());
		for (const auto &row : event.rows) {
			auto snapshot = ReadLineitemSnapshot(row, *lineitem, true);
			if (!snapshot) { buffer.impl->poisoned = true; return; }
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
	MaybeMergePendingQ12LightOps(context, dependencies, buffer);
}

void Q12AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                      const std::vector<aux_state::TableDependency> &dependencies,
                                      Q12IncrementalTxnBuffer &buffer) {
	if (!buffer.impl) buffer.impl = std::make_unique<Q12IncrementalTxnBuffer::Impl>();
	const auto *orders = FindTableDependency(dependencies, "orders");
	if (!orders || event.table_oid != orders->table_oid) return;
	if (event.kind != duckdb::TableMutationKind::UPDATE || !UpdateTouchesRelevantColumns(event, *orders)) return;
	const auto *priority_column = FindColumn(*orders, "o_orderpriority");
	const auto *orderkey_column = FindColumn(*orders, "o_orderkey");
	if (!priority_column || !orderkey_column) {
		buffer.impl->poisoned = true;
		return;
	}
	for (const auto &row : event.rows) {
		buffer.impl->cheap_order_fanout++;
		auto orderkey = PickInt64Column(row, *orderkey_column, true);
		if (!orderkey) continue;
		Q12PriorityClass old_priority = Q12PriorityClass::Neither;
		Q12PriorityClass new_priority = Q12PriorityClass::Neither;
		auto old_values = ValuesForColumn(row, priority_column->physical_index.index);
		if (!old_values.empty()) {
			if (!old_values.front().IsNull()) {
				old_priority = ClassifyQ12Priority(old_values.front().GetValue<std::string>());
			}
			if (old_values.size() > 1 && !old_values.back().IsNull()) {
				new_priority = ClassifyQ12Priority(old_values.back().GetValue<std::string>());
			}
		}
		if (old_priority == new_priority) {
			continue;
		}
		NativeScanSpec lineitem_spec;
		lineitem_spec.table_name = "lineitem";
		lineitem_spec.projected_columns = {
		    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
		    {"l_shipmode", "shipmode", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
		    {"l_commitdate", "commitdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
		    {"l_receiptdate", "receiptdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
		    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
		};
		lineitem_spec.filters = {{"l_orderkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(*orderkey)}};
		auto bind = BindNativeScan(context, lineitem_spec);
		auto source = CreateNativeChunkSource(context, *bind);
		while (source->Next()) {
			auto &chunk = source->Chunk();
			duckdb::UnifiedVectorFormat orderkeys;
			duckdb::UnifiedVectorFormat shipmodes;
			duckdb::UnifiedVectorFormat commitdates;
			duckdb::UnifiedVectorFormat receiptdates;
			duckdb::UnifiedVectorFormat shipdates;
			chunk.data[0].ToUnifiedFormat(chunk.size(), orderkeys);
			chunk.data[1].ToUnifiedFormat(chunk.size(), shipmodes);
			chunk.data[2].ToUnifiedFormat(chunk.size(), commitdates);
			chunk.data[3].ToUnifiedFormat(chunk.size(), receiptdates);
			chunk.data[4].ToUnifiedFormat(chunk.size(), shipdates);
			for (duckdb::idx_t r = 0; r < chunk.size(); r++) {
				LineitemSnapshot snapshot;
				snapshot.orderkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkeys)[orderkeys.sel->get_index(r)];
				snapshot.shipmode =
				    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipmodes)[shipmodes.sel->get_index(r)].GetString();
				snapshot.commitdate =
				    duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(commitdates)[commitdates.sel->get_index(r)];
				snapshot.receiptdate =
				    duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(receiptdates)[receiptdates.sel->get_index(r)];
				snapshot.shipdate =
				    duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdates)[shipdates.sel->get_index(r)];
				if (!Q12LineitemQualifies(snapshot.commitdate, snapshot.receiptdate, snapshot.shipdate)) {
					continue;
				}
				Q12PendingLineitemOp remove_op;
				remove_op.sign = -1;
				remove_op.partition = PartitionKeyFromReceiptdate(snapshot.receiptdate);
				remove_op.shipmode = snapshot.shipmode;
				remove_op.priority = old_priority;
				Q12PendingLineitemOp add_op = remove_op;
				add_op.sign = 1;
				add_op.priority = new_priority;
				NotePartition(buffer.impl->extra_partitions, remove_op.partition);
				buffer.impl->row_ops.push_back(remove_op);
				buffer.impl->row_ops.push_back(add_op);
			}
		}
	}
}

bool Q12FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                   const std::vector<aux_state::TableDependency> &dependencies,
                                   Q12IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)dependencies;
	return buffer.impl && !buffer.impl->poisoned && !buffer.impl->row_ops.empty();
}

bool ComputeQ12IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                const aux_state::InvalidationPlan &plan, Q12IncrementalTxnBuffer &buffer,
                                Q12ComputedIncrementalDelta &out) {
	(void)context;
	out = {};
	if (!buffer.impl || buffer.impl->poisoned) return false;
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) return false;
	auto folded = FoldLineitemOps(buffer.impl->row_ops);
	std::unordered_set<Q12PartitionKey> partitions;
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
		Q12DayPartition partition_state;
		if (store.GetStatus(partition) == aux_state::PartitionStatus::Valid) {
			auto payload = store.GetReadablePayload(partition);
			if (!payload) return false;
			partition_state = *std::static_pointer_cast<const Q12DayPartition>(payload);
		}
		ApplyDeltaToPartition(partition_state, folded_it->second);
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = partition;
		update.payload = std::make_shared<Q12DayPartition>(std::move(partition_state));
		update.expected_partition_version = store.GetPartitionVersion(partition);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(partition);
	}
	out.success = true;
	RecordQ12IncrementalApplyAttempt(true, 0);
	return true;
}

void PublishQ12IncrementalDelta(aux_state::PartitionedStateStore &store, const Q12ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch) {
	if (!delta.success) return;
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

Q12IncrementalTxnBuffer &GetQ12IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kQ12AuxAcceleratorId);
	auto *q12 = dynamic_cast<Q12IncrementalTxnBuffer *>(&base);
	if (!q12) throw std::runtime_error("incremental buffer for tpch_q12_aux has unexpected type");
	return *q12;
}

aux_state::InvalidationPlan AccumulateQ12Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                  const std::vector<aux_state::TableDependency> &dependencies,
                                                  aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetQ12IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsQ12DependentTableMutation(event, dependencies);
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	const auto *orders = FindTableDependency(dependencies, "orders");
	if (lineitem && event.table_oid == lineitem->table_oid) {
		Q12AccumulateLineitemMutationLight(context, event, dependencies, incremental);
	} else if (orders && event.table_oid == orders->table_oid) {
		Q12AccumulateOrdersMutationLight(context, event, dependencies, incremental);
	}
	if (dependent) {
		RecordQ12DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordQ12WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
