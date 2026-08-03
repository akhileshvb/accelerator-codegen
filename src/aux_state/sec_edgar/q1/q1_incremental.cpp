#include "accelerator_codegen/sec_edgar_q1_incremental.hpp"
#include "accelerator_codegen/sec_edgar_q1_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q1_write_policy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kSecEdgarQ1MaxLightCaptureRows = 1024;

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

std::optional<int32_t> ReadInt32Value(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	return value.GetValue<int32_t>();
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

std::optional<int32_t> PickInt32Column(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                       bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	const auto &picked = (values.size() == 1 || !use_new_value) ? values.front() : values.back();
	return ReadInt32Value(picked);
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
	SecEdgarQ1PartitionKey partition = 0;
	int32_t line = 0;
	std::string adsh;
	std::string stmt;
	std::string rfile;
	bool rfile_is_null = true;
};

struct SecEdgarQ1PendingRowOp {
	int8_t sign = 0;
	RowSnapshot snapshot;
};

struct SecEdgarQ1PendingRowLightOp {
	RowSnapshot snapshot;
	int8_t sign = 0;
};

std::optional<RowSnapshot> ReadRowSnapshot(const duckdb::TableMutationRow &row,
                                           const aux_state::TableDependency &dependency, bool use_new_value) {
	const auto *stmt_column = FindColumn(dependency, "stmt");
	const auto *rfile_column = FindColumn(dependency, "rfile");
	const auto *line_column = FindColumn(dependency, "line");
	const auto *adsh_column = FindColumn(dependency, "adsh");
	if (!stmt_column || !line_column || !adsh_column) {
		return std::nullopt;
	}
	auto stmt = PickStringColumn(row, *stmt_column, use_new_value);
	auto line = PickInt32Column(row, *line_column, use_new_value);
	auto adsh = PickStringColumn(row, *adsh_column, use_new_value);
	if (!stmt || !line || !adsh) {
		return std::nullopt;
	}
	std::optional<std::string> rfile;
	bool rfile_is_null = true;
	if (rfile_column) {
		rfile = PickStringColumn(row, *rfile_column, use_new_value);
		rfile_is_null = !rfile.has_value();
	}
	RowSnapshot snapshot;
	snapshot.partition = PartitionKeyFromStmtRfileStrings(*stmt, rfile.value_or(std::string()), rfile_is_null);
	snapshot.line = *line;
	snapshot.adsh = *adsh;
	snapshot.stmt = *stmt;
	snapshot.rfile = rfile.value_or(std::string());
	snapshot.rfile_is_null = rfile_is_null;
	return snapshot;
}

void NotePartition(aux_state::InvalidationPlan &plan, SecEdgarQ1PartitionKey partition) {
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(partition);
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	static const std::unordered_set<std::string> kRelevantColumns = {"stmt", "rfile", "line", "adsh"};
	for (const auto &updated : event.updated_columns) {
		for (const auto &column : dependency.watched_columns) {
			if (column.physical_index.index != updated.index) {
				continue;
			}
			if (kRelevantColumns.count(column.column_name) > 0) {
				return true;
			}
		}
	}
	return false;
}

void ApplyAdshDelta(SecEdgarQ1GroupPartition &partition, const std::string &adsh, int8_t sign) {
	if (adsh.empty()) {
		return;
	}
	auto &refcount = partition.adsh_refcount[std::hash<std::string>()(adsh) % 1000000];
	(void)refcount;
	auto it = partition.adsh_refcount.find(std::hash<std::string>()(adsh) % 1000000);
	uint32_t intern_id = it == partition.adsh_refcount.end() ? static_cast<uint32_t>(std::hash<std::string>()(adsh) & 0xFFFFFF)
	                                                           : static_cast<uint32_t>(it->first);
	auto &count = partition.adsh_refcount[intern_id];
	count += sign;
	if (count <= 0) {
		partition.adsh_refcount.erase(intern_id);
		partition.distinct_adsh.remove(intern_id);
		return;
	}
	if (count == 1 && sign > 0) {
		partition.distinct_adsh.insert(intern_id);
	}
}

void ApplyDeltaToPartition(SecEdgarQ1GroupPartition &partition, const SecEdgarQ1PartitionDelta &delta) {
	partition.cnt += delta.delta.cnt_delta;
	partition.line_sum += delta.delta.line_sum_delta;
	for (const auto &entry : delta.delta.adsh_refcount_delta) {
		ApplyAdshDelta(partition, entry.first, static_cast<int8_t>(entry.second));
	}
}

std::unordered_map<SecEdgarQ1PartitionKey, SecEdgarQ1PartitionDelta> FoldOpsByPartition(const std::vector<SecEdgarQ1PendingRowOp> &ops) {
	std::unordered_map<SecEdgarQ1PartitionKey, SecEdgarQ1PartitionDelta> folded;
	for (const auto &op : ops) {
		auto &delta = folded[op.snapshot.partition].delta;
		delta.cnt_delta += op.sign;
		delta.line_sum_delta += op.sign * static_cast<int64_t>(op.snapshot.line);
		delta.adsh_refcount_delta[op.snapshot.adsh] += op.sign;
	}
	return folded;
}

} // namespace

struct SecEdgarQ1IncrementalTxnBuffer::Impl {
	std::vector<SecEdgarQ1PendingRowOp> row_ops;
	std::vector<SecEdgarQ1PendingRowLightOp> pending_row_light_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

SecEdgarQ1IncrementalTxnBuffer::SecEdgarQ1IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {
}
SecEdgarQ1IncrementalTxnBuffer::~SecEdgarQ1IncrementalTxnBuffer() = default;
SecEdgarQ1IncrementalTxnBuffer::SecEdgarQ1IncrementalTxnBuffer(SecEdgarQ1IncrementalTxnBuffer &&) noexcept = default;
SecEdgarQ1IncrementalTxnBuffer &SecEdgarQ1IncrementalTxnBuffer::operator=(SecEdgarQ1IncrementalTxnBuffer &&) noexcept = default;

void SecEdgarQ1IncrementalTxnBuffer::Clear() {
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

bool SecEdgarQ1IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || impl->cheap_rows_touched > 0 || !impl->pending_row_light_ops.empty());
}

bool SecEdgarQ1IncrementalTxnBuffer::IsPoisoned() const {
	return impl && impl->poisoned;
}

uint64_t SecEdgarQ1IncrementalTxnBuffer::RowOpCount() const {
	return impl ? impl->row_ops.size() : 0;
}

uint64_t SecEdgarQ1IncrementalTxnBuffer::PendingLightOpCount() const {
	return impl ? impl->pending_row_light_ops.size() : 0;
}

uint64_t SecEdgarQ1IncrementalTxnBuffer::CheapRowsTouched() const {
	return impl ? impl->cheap_rows_touched : 0;
}

bool SecEdgarQ1IncrementalTxnBuffer::HeavyCaptureDone() const {
	return impl && impl->heavy_capture_done;
}

bool SecEdgarQ1IncrementalTxnBuffer::BulkMutation() const {
	return impl && impl->bulk_mutation;
}

aux_state::InvalidationPlan SecEdgarQ1IncrementalTxnBuffer::ExtraInvalidationPlan() const {
	if (!impl) {
		return {};
	}
	return impl->extra_partitions;
}

void SecEdgarQ1AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ1IncrementalTxnBuffer &buffer) {
	(void)context;
	if (!buffer.impl) {
		buffer.impl = std::make_unique<SecEdgarQ1IncrementalTxnBuffer::Impl>();
	}
	if (buffer.impl->poisoned) {
		return;
	}
	const auto *pre = FindTableDependency(dependencies, "pre");
	if (!pre || event.table_oid != pre->table_oid) {
		return;
	}

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		for (const auto &row : event.rows) {
			auto snapshot = ReadRowSnapshot(row, *pre, true);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			buffer.impl->pending_row_light_ops.push_back(SecEdgarQ1PendingRowLightOp {*snapshot, 1});
			NotePartition(buffer.impl->extra_partitions, snapshot->partition);
			buffer.impl->cheap_rows_touched++;
		}
		break;
	case duckdb::TableMutationKind::DELETE:
		for (const auto &row : event.rows) {
			auto snapshot = ReadRowSnapshot(row, *pre, false);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			buffer.impl->pending_row_light_ops.push_back(SecEdgarQ1PendingRowLightOp {*snapshot, -1});
			NotePartition(buffer.impl->extra_partitions, snapshot->partition);
			buffer.impl->cheap_rows_touched++;
		}
		break;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, *pre)) {
			return;
		}
		for (const auto &row : event.rows) {
			auto old_snapshot = ReadRowSnapshot(row, *pre, false);
			auto new_snapshot = ReadRowSnapshot(row, *pre, true);
			if (!old_snapshot || !new_snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			if (old_snapshot->partition != new_snapshot->partition) {
				buffer.impl->pending_row_light_ops.push_back(SecEdgarQ1PendingRowLightOp {*old_snapshot, -1});
				buffer.impl->pending_row_light_ops.push_back(SecEdgarQ1PendingRowLightOp {*new_snapshot, 1});
				NotePartition(buffer.impl->extra_partitions, old_snapshot->partition);
				NotePartition(buffer.impl->extra_partitions, new_snapshot->partition);
			} else if (old_snapshot->line != new_snapshot->line || old_snapshot->adsh != new_snapshot->adsh) {
				buffer.impl->pending_row_light_ops.push_back(SecEdgarQ1PendingRowLightOp {*old_snapshot, -1});
				buffer.impl->pending_row_light_ops.push_back(SecEdgarQ1PendingRowLightOp {*new_snapshot, 1});
				NotePartition(buffer.impl->extra_partitions, old_snapshot->partition);
			}
			buffer.impl->cheap_rows_touched++;
		}
		break;
	default:
		break;
	}
	if (buffer.impl->cheap_rows_touched > kSecEdgarQ1MaxLightCaptureRows) {
		buffer.impl->bulk_mutation = true;
	}
}

bool SecEdgarQ1FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  SecEdgarQ1IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)dependencies;
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return false;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return !buffer.impl->row_ops.empty();
	}
	for (const auto &pending : buffer.impl->pending_row_light_ops) {
		buffer.impl->row_ops.push_back(SecEdgarQ1PendingRowOp {pending.sign, pending.snapshot});
	}
	buffer.impl->pending_row_light_ops.clear();
	buffer.impl->heavy_capture_done = true;
	return true;
}

void MaybeMergePendingSecEdgarQ1LightOps(duckdb::ClientContext &context,
                                 const std::vector<aux_state::TableDependency> &dependencies,
                                 SecEdgarQ1IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return;
	}
	if (buffer.impl->cheap_rows_touched > kSecEdgarQ1MaxLightCaptureRows) {
		return;
	}
	SecEdgarQ1FinalizeIncrementalCapture(context, dependencies, buffer);
}

void SecEdgarQ1AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                             const std::vector<aux_state::TableDependency> &dependencies,
                             SecEdgarQ1IncrementalTxnBuffer &buffer) {
	SecEdgarQ1AccumulatePreMutationLight(context, event, dependencies, buffer);
	MaybeMergePendingSecEdgarQ1LightOps(context, dependencies, buffer);
}

bool ComputeSecEdgarQ1IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, SecEdgarQ1IncrementalTxnBuffer &buffer,
                               SecEdgarQ1ComputedIncrementalDelta &out) {
	(void)context;
	const auto start = std::chrono::steady_clock::now();
	out = SecEdgarQ1ComputedIncrementalDelta {};
	if (!buffer.impl || buffer.impl->poisoned) {
		return false;
	}
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		return false;
	}

	auto folded = FoldOpsByPartition(buffer.impl->row_ops);
	std::unordered_set<SecEdgarQ1PartitionKey> partitions;
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
		SecEdgarQ1GroupPartition group_partition;
		if (status == aux_state::PartitionStatus::Valid) {
			auto payload = store.GetReadablePayload(partition);
			if (!payload) {
				return false;
			}
			group_partition = *std::static_pointer_cast<const SecEdgarQ1GroupPartition>(payload);
		}
		ApplyDeltaToPartition(group_partition, folded_it->second);
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = partition;
		update.payload = std::make_shared<SecEdgarQ1GroupPartition>(group_partition);
		update.expected_partition_version = store.GetPartitionVersion(partition);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(partition);
	}
	out.success = true;
	const auto end = std::chrono::steady_clock::now();
	const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
	RecordSecEdgarQ1IncrementalApplyAttempt(true, ns);
	return true;
}

void PublishSecEdgarQ1IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ1ComputedIncrementalDelta &delta,
                               uint64_t valid_through_mutation_epoch) {
	if (!delta.success) {
		return;
	}
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

SecEdgarQ1IncrementalTxnBuffer &GetSecEdgarQ1IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kSecEdgarQ1AuxAcceleratorId);
	auto *q1 = dynamic_cast<SecEdgarQ1IncrementalTxnBuffer *>(&base);
	if (!q1) {
		throw std::runtime_error("incremental buffer for sec_edgar_q1_aux has unexpected type");
	}
	return *q1;
}

aux_state::InvalidationPlan AccumulateSecEdgarQ1Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                 const std::vector<aux_state::TableDependency> &dependencies,
                                                 aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetSecEdgarQ1IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsSecEdgarQ1DependentTableMutation(event, dependencies);
	const auto *pre = FindTableDependency(dependencies, "pre");
	if (pre && event.table_oid == pre->table_oid) {
		SecEdgarQ1AccumulatePreMutationLight(context, event, dependencies, incremental);
	}
	if (dependent) {
		RecordSecEdgarQ1DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordSecEdgarQ1WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
