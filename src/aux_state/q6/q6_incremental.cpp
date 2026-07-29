#include "accelerator_codegen/tpch_q6_incremental.hpp"
#include "accelerator_codegen/tpch_q6_write_policy.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kQ6MaxLightCaptureRows = 1024;

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
	duckdb::date_t shipdate {};
	int64_t discount_raw = 0;
	int64_t quantity_raw = 0;
	int64_t extendedprice_raw = 0;
};

struct Q6PendingLineitemOp {
	int8_t sign = 0;
	Q6PartitionKey partition = 0;
	int8_t discount_cent = 0;
	int64_t quantity_scaled = 0;
	Q6AccT contribution = static_cast<Q6AccT>(0);
};

void NotePartition(aux_state::InvalidationPlan &plan, Q6PartitionKey partition) {
	if (partition == 0) return;
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(partition);
}

std::optional<LineitemSnapshot> ReadLineitemSnapshot(const duckdb::TableMutationRow &row,
                                                     const aux_state::TableDependency &dependency, bool use_new_value) {
	const auto *shipdate_column = FindColumn(dependency, "l_shipdate");
	const auto *discount_column = FindColumn(dependency, "l_discount");
	const auto *quantity_column = FindColumn(dependency, "l_quantity");
	const auto *price_column = FindColumn(dependency, "l_extendedprice");
	if (!shipdate_column || !discount_column || !quantity_column || !price_column) return std::nullopt;
	auto shipdate = PickDateColumn(row, *shipdate_column, use_new_value);
	auto discount_raw = PickInt64Column(row, *discount_column, use_new_value);
	auto quantity_raw = PickInt64Column(row, *quantity_column, use_new_value);
	auto extendedprice_raw = PickInt64Column(row, *price_column, use_new_value);
	if (!shipdate || !discount_raw || !quantity_raw || !extendedprice_raw) return std::nullopt;
	LineitemSnapshot snapshot;
	snapshot.shipdate = *shipdate;
	snapshot.discount_raw = *discount_raw;
	snapshot.quantity_raw = *quantity_raw;
	snapshot.extendedprice_raw = *extendedprice_raw;
	return snapshot;
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) return true;
	}
	return false;
}

struct Q6FoldRowKey {
	int8_t discount_cent = 0;
	int64_t quantity_scaled = 0;

	bool operator==(const Q6FoldRowKey &other) const {
		return discount_cent == other.discount_cent && quantity_scaled == other.quantity_scaled;
	}
};

struct Q6FoldRowKeyHash {
	size_t operator()(const Q6FoldRowKey &key) const {
		return std::hash<int64_t>{}((static_cast<uint64_t>(static_cast<uint8_t>(key.discount_cent)) << 56) ^
		                            static_cast<uint64_t>(key.quantity_scaled));
	}
};

void AccumulateSignedNet(Q6AccT &net, Q6AccT contribution, int8_t sign) {
#if defined(__SIZEOF_INT128__)
	net += static_cast<Q6AccT>(sign) * contribution;
#else
	if (sign >= 0) {
		duckdb::Hugeint::AddInPlace(net, contribution);
	} else if (!duckdb::Hugeint::TrySubtractInPlace(net, contribution)) {
		throw duckdb::OutOfRangeException("Overflow while folding Q6 row delta");
	}
#endif
}

bool Q6AccTIsZero(Q6AccT value) {
#if defined(__SIZEOF_INT128__)
	return value == static_cast<Q6AccT>(0);
#else
	return duckdb::Hugeint::Equals(value, duckdb::Hugeint::Convert(0));
#endif
}

std::optional<Q6RowDelta> NetToRowDelta(int8_t discount_cent, int64_t quantity_scaled, Q6AccT net) {
	if (Q6AccTIsZero(net)) {
		return std::nullopt;
	}
#if defined(__SIZEOF_INT128__)
	const int8_t sign = net > static_cast<Q6AccT>(0) ? static_cast<int8_t>(1) : static_cast<int8_t>(-1);
	const Q6AccT contribution = sign > 0 ? net : -net;
#else
	const auto zero = duckdb::Hugeint::Convert(0);
	const int8_t sign =
	    duckdb::Hugeint::Compare(net, zero) >= 0 ? static_cast<int8_t>(1) : static_cast<int8_t>(-1);
	Q6AccT contribution = net;
	if (sign < 0) {
		duckdb::hugeint_t negated;
		if (!duckdb::Hugeint::TryMultiply(net, duckdb::Hugeint::Convert(-1), negated)) {
			throw duckdb::OutOfRangeException("Overflow while folding Q6 row delta");
		}
		contribution = negated;
	}
#endif
	return Q6RowDelta {discount_cent, quantity_scaled, contribution, sign};
}

std::unordered_map<Q6PartitionKey, Q6PartitionDelta> FoldLineitemOps(const std::vector<Q6PendingLineitemOp> &ops) {
	std::unordered_map<Q6PartitionKey, std::unordered_map<Q6FoldRowKey, Q6AccT, Q6FoldRowKeyHash>> nets;
	for (const auto &op : ops) {
		const Q6FoldRowKey key {op.discount_cent, op.quantity_scaled};
		AccumulateSignedNet(nets[op.partition][key], op.contribution, op.sign);
	}
	std::unordered_map<Q6PartitionKey, Q6PartitionDelta> folded;
	for (auto &partition_entry : nets) {
		auto &delta = folded[partition_entry.first];
		for (auto &row_entry : partition_entry.second) {
			if (auto row = NetToRowDelta(row_entry.first.discount_cent, row_entry.first.quantity_scaled,
			                             row_entry.second)) {
				delta.row_deltas.push_back(*row);
			}
		}
	}
	return folded;
}

void ApplyDeltaToPartition(Q6DayPartition &partition, const Q6PartitionDelta &delta) {
	for (const auto &row : delta.row_deltas) {
		ApplyQ6RowContribution(partition, row.discount_cent, row.quantity_scaled, row.contribution, row.sign);
	}
}

std::optional<Q6PendingLineitemOp> MakeOpFromSnapshot(const LineitemSnapshot &snapshot, int8_t sign) {
	Q6PendingLineitemOp op;
	op.sign = sign;
	op.partition = Q6PartitionKeyFromShipdate(snapshot.shipdate);
	op.discount_cent = static_cast<int8_t>(snapshot.discount_raw);
	op.quantity_scaled = snapshot.quantity_raw;
	op.contribution = Q6RowContributionRaw(snapshot.extendedprice_raw, snapshot.discount_raw);
	return op;
}

} // namespace

struct Q6IncrementalTxnBuffer::Impl {
	std::vector<Q6PendingLineitemOp> row_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t cross_partition_moves = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

Q6IncrementalTxnBuffer::Q6IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {}
Q6IncrementalTxnBuffer::~Q6IncrementalTxnBuffer() = default;
Q6IncrementalTxnBuffer::Q6IncrementalTxnBuffer(Q6IncrementalTxnBuffer &&) noexcept = default;
Q6IncrementalTxnBuffer &Q6IncrementalTxnBuffer::operator=(Q6IncrementalTxnBuffer &&) noexcept = default;

void Q6IncrementalTxnBuffer::Clear() {
	if (!impl) impl = std::make_unique<Impl>();
	impl->row_ops.clear();
	impl->extra_partitions = {};
	impl->cheap_rows_touched = 0;
	impl->estimated_lineitem_scan_rows = 0;
	impl->cross_partition_moves = 0;
	impl->poisoned = false;
	impl->heavy_capture_done = false;
	impl->bulk_mutation = false;
}

bool Q6IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || impl->cheap_rows_touched > 0);
}

uint64_t Q6IncrementalTxnBuffer::CheapRowsTouched() const { return impl ? impl->cheap_rows_touched : 0; }
uint64_t Q6IncrementalTxnBuffer::EstimatedLineitemScanRows() const { return impl ? impl->estimated_lineitem_scan_rows : 0; }
bool Q6IncrementalTxnBuffer::HeavyCaptureDone() const { return impl && impl->heavy_capture_done; }
bool Q6IncrementalTxnBuffer::BulkMutation() const { return impl && impl->bulk_mutation; }
aux_state::InvalidationPlan Q6IncrementalTxnBuffer::ExtraInvalidationPlan() const { return impl ? impl->extra_partitions : aux_state::InvalidationPlan {}; }
bool Q6IncrementalTxnBuffer::IsPoisoned() const { return impl && impl->poisoned; }
uint64_t Q6IncrementalTxnBuffer::RowOpCount() const { return impl ? impl->row_ops.size() : 0; }
uint64_t Q6IncrementalTxnBuffer::CrossPartitionOpCount() const { return impl ? impl->cross_partition_moves : 0; }
uint64_t Q6IncrementalTxnBuffer::PendingLightOpCount() const { return 0; }

void MaybeMergePendingQ6LightOps(duckdb::ClientContext &, const std::vector<aux_state::TableDependency> &,
                                 Q6IncrementalTxnBuffer &) {
}

void Q6AccumulateLineitemMutationLight(duckdb::ClientContext &, const duckdb::TableMutationEvent &event,
                                       const std::vector<aux_state::TableDependency> &dependencies,
                                       Q6IncrementalTxnBuffer &buffer) {
	if (!buffer.impl) buffer.impl = std::make_unique<Q6IncrementalTxnBuffer::Impl>();
	if (buffer.impl->poisoned) return;
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (!lineitem || event.table_oid != lineitem->table_oid) return;

	auto touch = [&](size_t count) {
		buffer.impl->cheap_rows_touched += count;
		if (buffer.impl->cheap_rows_touched > kQ6MaxLightCaptureRows) buffer.impl->bulk_mutation = true;
	};

	auto append = [&](const LineitemSnapshot &snapshot, int8_t sign) {
		if (buffer.impl->bulk_mutation) return;
		if (auto op = MakeOpFromSnapshot(snapshot, sign)) {
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
			if (Q6PartitionKeyFromShipdate(old_snapshot->shipdate) != Q6PartitionKeyFromShipdate(new_snapshot->shipdate)) {
				buffer.impl->cross_partition_moves++;
			}
			append(*old_snapshot, -1);
			append(*new_snapshot, 1);
		}
		break;
	default:
		return;
	}
}

bool Q6FinalizeIncrementalCapture(duckdb::ClientContext &, const std::vector<aux_state::TableDependency> &,
                                  Q6IncrementalTxnBuffer &buffer) {
	return buffer.impl && !buffer.impl->poisoned && !buffer.impl->row_ops.empty();
}

bool ComputeQ6IncrementalDelta(duckdb::ClientContext &, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, Q6IncrementalTxnBuffer &buffer,
                               Q6ComputedIncrementalDelta &out) {
	out = {};
	if (!buffer.impl || buffer.impl->poisoned) return false;
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) return false;
	auto folded = FoldLineitemOps(buffer.impl->row_ops);
	std::unordered_set<Q6PartitionKey> partitions;
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
		Q6DayPartition partition_state;
		if (store.GetStatus(partition) == aux_state::PartitionStatus::Valid) {
			auto payload = store.GetReadablePayload(partition);
			if (!payload) return false;
			partition_state = *std::static_pointer_cast<const Q6DayPartition>(payload);
		}
		ApplyDeltaToPartition(partition_state, folded_it->second);
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = partition;
		update.payload = std::make_shared<Q6DayPartition>(std::move(partition_state));
		update.expected_partition_version = store.GetPartitionVersion(partition);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(partition);
	}
	out.success = true;
	RecordQ6IncrementalApplyAttempt(true, 0);
	return true;
}

void PublishQ6IncrementalDelta(aux_state::PartitionedStateStore &store, const Q6ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch) {
	if (!delta.success) return;
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

Q6IncrementalTxnBuffer &GetQ6IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kQ6AuxAcceleratorId);
	auto *q6 = dynamic_cast<Q6IncrementalTxnBuffer *>(&base);
	if (!q6) throw std::runtime_error("incremental buffer for tpch_q6_aux has unexpected type");
	return *q6;
}

aux_state::InvalidationPlan AccumulateQ6Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                  const std::vector<aux_state::TableDependency> &dependencies,
                                                  aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetQ6IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsQ6DependentTableMutation(event, dependencies);
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (lineitem && event.table_oid == lineitem->table_oid) {
		Q6AccumulateLineitemMutationLight(context, event, dependencies, incremental);
	}
	if (dependent) {
		RecordQ6DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordQ6WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
