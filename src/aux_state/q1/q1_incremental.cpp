#include "accelerator_codegen/tpch_q1_incremental.hpp"
#include "accelerator_codegen/tpch_q1_maintenance.hpp"
#include "accelerator_codegen/tpch_q1_write_policy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kQ1MaxLightCaptureRows = 1024;

const aux_state::ColumnRef *FindColumn(const aux_state::TableDependency &dependency, const std::string &name) {
	for (const auto &column : dependency.watched_columns) {
		if (column.column_name == name) {
			return &column;
		}
	}
	return nullptr;
}

const aux_state::TableDependency *FindTableDependency(const std::vector<aux_state::TableDependency> &dependencies,
                                                      const std::string &table_name) {
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == table_name) {
			return &dependency;
		}
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
	if (value.IsNull()) {
		return std::nullopt;
	}
	return value.GetValue<duckdb::date_t>();
}

std::vector<duckdb::Value> ValuesForColumn(const duckdb::TableMutationRow &row, duckdb::idx_t physical_index) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == physical_index) {
			values.push_back(projected.value);
		}
	}
	return values;
}

std::optional<int64_t> PickInt64Column(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                       bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	if (values.size() == 1 || !use_new_value) {
		return ReadInt64Value(values.front());
	}
	return ReadInt64Value(values.back());
}

std::optional<duckdb::date_t> PickDateColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                             bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	if (values.size() == 1 || !use_new_value) {
		return ReadDateValue(values.front());
	}
	return ReadDateValue(values.back());
}

std::optional<std::string> PickStringColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                            bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	const auto &picked = (values.size() == 1 || !use_new_value) ? values.front() : values.back();
	if (picked.IsNull()) {
		return std::nullopt;
	}
	return picked.GetValue<std::string>();
}

struct RowSnapshot {
	Q1PartitionKey day = 0;
	Q1GroupKey group_key = 0;
	int64_t quantity = 0;
	int64_t price = 0;
	int64_t discount = 0;
	int64_t tax = 0;
};

struct Q1PendingRowOp {
	int8_t sign = 0;
	Q1PartitionKey day = 0;
	Q1GroupKey group_key = 0;
	int64_t quantity = 0;
	int64_t price = 0;
	int64_t discount = 0;
	int64_t tax = 0;
};

struct Q1PendingRowLightOp {
	RowSnapshot snapshot;
	int8_t sign = 0;
};

std::optional<RowSnapshot> ReadRowSnapshot(const duckdb::TableMutationRow &row,
                                           const aux_state::TableDependency &dependency, bool use_new_value) {
	const auto *shipdate_column = FindColumn(dependency, "l_shipdate");
	const auto *qty_column = FindColumn(dependency, "l_quantity");
	const auto *price_column = FindColumn(dependency, "l_extendedprice");
	const auto *discount_column = FindColumn(dependency, "l_discount");
	const auto *tax_column = FindColumn(dependency, "l_tax");
	const auto *returnflag_column = FindColumn(dependency, "l_returnflag");
	const auto *linestatus_column = FindColumn(dependency, "l_linestatus");
	if (!shipdate_column || !qty_column || !price_column || !discount_column || !tax_column || !returnflag_column ||
	    !linestatus_column) {
		return std::nullopt;
	}
	auto shipdate = PickDateColumn(row, *shipdate_column, use_new_value);
	auto quantity = PickInt64Column(row, *qty_column, use_new_value);
	auto price = PickInt64Column(row, *price_column, use_new_value);
	auto discount = PickInt64Column(row, *discount_column, use_new_value);
	auto tax = PickInt64Column(row, *tax_column, use_new_value);
	auto returnflag = PickStringColumn(row, *returnflag_column, use_new_value);
	auto linestatus = PickStringColumn(row, *linestatus_column, use_new_value);
	if (!shipdate || !quantity || !price || !discount || !tax || !returnflag || !linestatus) {
		return std::nullopt;
	}
	if (returnflag->size() != 1 || linestatus->size() != 1) {
		return std::nullopt;
	}
	RowSnapshot snapshot;
	snapshot.day = PartitionKeyFromShipdate(*shipdate);
	snapshot.group_key = PackQ1GroupKey((*returnflag)[0], (*linestatus)[0]);
	snapshot.quantity = *quantity;
	snapshot.price = *price;
	snapshot.discount = *discount;
	snapshot.tax = *tax;
	return snapshot;
}

void NotePartition(aux_state::InvalidationPlan &plan, Q1PartitionKey day) {
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(day);
}

bool TryNotePartitionFromRow(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &shipdate_column,
                             aux_state::InvalidationPlan &plan) {
	auto shipdate = PickDateColumn(row, shipdate_column, false);
	if (!shipdate) {
		shipdate = PickDateColumn(row, shipdate_column, true);
	}
	if (!shipdate) {
		return false;
	}
	NotePartition(plan, PartitionKeyFromShipdate(*shipdate));
	return true;
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) {
			return true;
		}
	}
	return false;
}

void ApplyDeltaToPartition(Q1DayPartition &partition, const Q1PartitionDelta &delta) {
	for (const auto &entry : delta.groups) {
		auto &stats = partition.groups[entry.first];
		const auto &group_delta = entry.second.delta;
		stats.count += group_delta.count;
		stats.sum_qty += group_delta.sum_qty;
		stats.sum_base_price += group_delta.sum_base_price;
		stats.sum_disc_price += group_delta.sum_disc_price;
		stats.sum_charge += group_delta.sum_charge;
		stats.sum_discount += group_delta.sum_discount;
	}
}

std::unordered_map<Q1PartitionKey, Q1PartitionDelta> FoldOpsByPartition(const std::vector<Q1PendingRowOp> &ops) {
	std::unordered_map<Q1PartitionKey, Q1PartitionDelta> folded;
	for (const auto &op : ops) {
		Q1DayGroupStats contribution;
		ApplyQ1RowContribution(contribution, op.quantity, op.price, op.discount, op.tax, op.sign);
		auto &group_delta = folded[op.day].groups[op.group_key].delta;
		group_delta.count += contribution.count;
		group_delta.sum_qty += contribution.sum_qty;
		group_delta.sum_base_price += contribution.sum_base_price;
		group_delta.sum_disc_price += contribution.sum_disc_price;
		group_delta.sum_charge += contribution.sum_charge;
		group_delta.sum_discount += contribution.sum_discount;
	}
	return folded;
}

} // namespace

struct Q1IncrementalTxnBuffer::Impl {
	std::vector<Q1PendingRowOp> row_ops;
	std::vector<Q1PendingRowLightOp> pending_row_light_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

Q1IncrementalTxnBuffer::Q1IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {
}
Q1IncrementalTxnBuffer::~Q1IncrementalTxnBuffer() = default;
Q1IncrementalTxnBuffer::Q1IncrementalTxnBuffer(Q1IncrementalTxnBuffer &&) noexcept = default;
Q1IncrementalTxnBuffer &Q1IncrementalTxnBuffer::operator=(Q1IncrementalTxnBuffer &&) noexcept = default;

void Q1IncrementalTxnBuffer::Clear() {
	if (!impl) {
		impl = std::make_unique<Impl>();
	}
	impl->row_ops.clear();
	impl->pending_row_light_ops.clear();
	impl->extra_partitions = aux_state::InvalidationPlan {};
	impl->cheap_rows_touched = 0;
	impl->poisoned = false;
	impl->heavy_capture_done = false;
	impl->bulk_mutation = false;
}

bool Q1IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || impl->cheap_rows_touched > 0 || !impl->pending_row_light_ops.empty());
}

bool Q1IncrementalTxnBuffer::IsPoisoned() const {
	return impl && impl->poisoned;
}

uint64_t Q1IncrementalTxnBuffer::RowOpCount() const {
	return impl ? impl->row_ops.size() : 0;
}

uint64_t Q1IncrementalTxnBuffer::PendingLightOpCount() const {
	return impl ? impl->pending_row_light_ops.size() : 0;
}

uint64_t Q1IncrementalTxnBuffer::CheapRowsTouched() const {
	return impl ? impl->cheap_rows_touched : 0;
}

bool Q1IncrementalTxnBuffer::HeavyCaptureDone() const {
	return impl && impl->heavy_capture_done;
}

bool Q1IncrementalTxnBuffer::BulkMutation() const {
	return impl && impl->bulk_mutation;
}

aux_state::InvalidationPlan Q1IncrementalTxnBuffer::ExtraInvalidationPlan() const {
	if (!impl) {
		return {};
	}
	return impl->extra_partitions;
}

void Q1AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q1IncrementalTxnBuffer &buffer) {
	(void)context;
	if (!buffer.impl) {
		buffer.impl = std::make_unique<Q1IncrementalTxnBuffer::Impl>();
	}
	if (buffer.impl->poisoned) {
		return;
	}
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (!lineitem || event.table_oid != lineitem->table_oid) {
		return;
	}
	const auto *shipdate_column = FindColumn(*lineitem, "l_shipdate");

	auto touch_rows = [&](size_t row_count) {
		buffer.impl->cheap_rows_touched += row_count;
		if (buffer.impl->cheap_rows_touched > kQ1MaxLightCaptureRows) {
			buffer.impl->bulk_mutation = true;
		}
	};

	auto note_snapshot = [&](const RowSnapshot &snapshot) { NotePartition(buffer.impl->extra_partitions, snapshot.day); };

	auto append_light_row = [&](const RowSnapshot &snapshot, int8_t sign) {
		note_snapshot(snapshot);
		if (buffer.impl->bulk_mutation) {
			return;
		}
		buffer.impl->pending_row_light_ops.push_back(Q1PendingRowLightOp {snapshot, sign});
	};

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			if (shipdate_column) {
				TryNotePartitionFromRow(row, *shipdate_column, buffer.impl->extra_partitions);
			}
			if (buffer.impl->bulk_mutation) {
				continue;
			}
			auto snapshot = ReadRowSnapshot(row, *lineitem, true);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				buffer.impl->pending_row_light_ops.clear();
				return;
			}
			append_light_row(*snapshot, 1);
		}
		break;
	case duckdb::TableMutationKind::DELETE:
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			if (shipdate_column) {
				TryNotePartitionFromRow(row, *shipdate_column, buffer.impl->extra_partitions);
			}
			if (buffer.impl->bulk_mutation) {
				continue;
			}
			auto snapshot = ReadRowSnapshot(row, *lineitem, false);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				buffer.impl->pending_row_light_ops.clear();
				return;
			}
			append_light_row(*snapshot, -1);
		}
		break;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, *lineitem)) {
			return;
		}
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			if (shipdate_column) {
				TryNotePartitionFromRow(row, *shipdate_column, buffer.impl->extra_partitions);
			}
			if (buffer.impl->bulk_mutation) {
				continue;
			}
			auto old_snapshot = ReadRowSnapshot(row, *lineitem, false);
			auto new_snapshot = ReadRowSnapshot(row, *lineitem, true);
			if (!old_snapshot || !new_snapshot) {
				buffer.impl->poisoned = true;
				buffer.impl->pending_row_light_ops.clear();
				return;
			}
			append_light_row(*old_snapshot, -1);
			append_light_row(*new_snapshot, 1);
		}
		break;
	default:
		return;
	}
}

bool Q1FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q1IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)dependencies;
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return false;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return !buffer.impl->row_ops.empty();
	}
	for (const auto &pending : buffer.impl->pending_row_light_ops) {
		buffer.impl->row_ops.push_back(
		    Q1PendingRowOp {pending.sign, pending.snapshot.day, pending.snapshot.group_key, pending.snapshot.quantity,
		                    pending.snapshot.price, pending.snapshot.discount, pending.snapshot.tax});
	}
	buffer.impl->pending_row_light_ops.clear();
	buffer.impl->heavy_capture_done = true;
	return true;
}

void MaybeMergePendingQ1LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 Q1IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return;
	}
	if (buffer.impl->cheap_rows_touched > kQ1MaxLightCaptureRows) {
		return;
	}
	Q1FinalizeIncrementalCapture(context, dependencies, buffer);
}

void Q1AccumulateLineitemMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q1IncrementalTxnBuffer &buffer) {
	Q1AccumulateLineitemMutationLight(context, event, dependencies, buffer);
	MaybeMergePendingQ1LightOps(context, dependencies, buffer);
}

bool ComputeQ1IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q1IncrementalTxnBuffer &buffer,
                               Q1ComputedIncrementalDelta &out) {
	(void)context;
	const auto start = std::chrono::steady_clock::now();
	out = Q1ComputedIncrementalDelta {};
	if (!buffer.impl || buffer.impl->poisoned) {
		return false;
	}
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		return false;
	}
	if (plan.scope == aux_state::InvalidationScope::None && !buffer.HasPendingChanges()) {
		out.success = true;
		return true;
	}

	auto folded = FoldOpsByPartition(buffer.impl->row_ops);
	std::unordered_set<Q1PartitionKey> partitions;
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) {
			partitions.insert(partition);
		}
	}
	for (const auto &entry : folded) {
		partitions.insert(entry.first);
	}
	if (partitions.empty()) {
		out.success = true;
		return true;
	}

	for (auto partition : partitions) {
		const auto folded_it = folded.find(partition);
		if (folded_it == folded.end()) {
			continue;
		}
		const auto status = store.GetStatus(partition);
		if (status == aux_state::PartitionStatus::Invalid) {
			return false;
		}
		Q1DayPartition day_partition;
		if (status == aux_state::PartitionStatus::Valid) {
			auto payload = store.GetReadablePayload(partition);
			if (!payload) {
				return false;
			}
			day_partition = *std::static_pointer_cast<const Q1DayPartition>(payload);
		}
		for (const auto &group_entry : folded_it->second.groups) {
			const auto &delta = group_entry.second.delta;
			auto &stats = day_partition.groups[group_entry.first];
			stats.count += delta.count;
			stats.sum_qty += delta.sum_qty;
			stats.sum_base_price += delta.sum_base_price;
			stats.sum_disc_price += delta.sum_disc_price;
			stats.sum_charge += delta.sum_charge;
			stats.sum_discount += delta.sum_discount;
		}
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = partition;
		update.payload = std::make_shared<Q1DayPartition>(day_partition);
		update.expected_partition_version = store.GetPartitionVersion(partition);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(partition);
	}
	out.success = true;
	const auto end = std::chrono::steady_clock::now();
	const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
	RecordQ1IncrementalApplyAttempt(true, ns);
	return true;
}

void PublishQ1IncrementalDelta(aux_state::PartitionedStateStore &store, const Q1ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch) {
	if (!delta.success) {
		return;
	}
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

Q1IncrementalTxnBuffer &GetQ1IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kQ1AuxAcceleratorId);
	auto *q1 = dynamic_cast<Q1IncrementalTxnBuffer *>(&base);
	if (!q1) {
		throw std::runtime_error("incremental buffer for tpch_q1_aux has unexpected type");
	}
	return *q1;
}

aux_state::InvalidationPlan AccumulateQ1Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetQ1IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsQ1DependentTableMutation(event, dependencies);
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (lineitem && event.table_oid == lineitem->table_oid) {
		Q1AccumulateLineitemMutationLight(context, event, dependencies, incremental);
	}
	if (dependent) {
		RecordQ1DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordQ1WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
