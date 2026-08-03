#include "accelerator_codegen/sec_edgar_q2_incremental.hpp"
#include "accelerator_codegen/sec_edgar_q2_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q2_write_policy.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/sec_edgar_q2_aux.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kSecEdgarQ2MaxLightCaptureRows = 1024;

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

std::vector<duckdb::Value> ValuesForColumn(const duckdb::TableMutationRow &row, duckdb::idx_t physical_index) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == physical_index) {
			values.push_back(projected.value);
		}
	}
	return values;
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

std::optional<double> PickDoubleColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                       bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	const auto &picked = (values.size() == 1 || !use_new_value) ? values.front() : values.back();
	if (picked.IsNull()) {
		return std::nullopt;
	}
	return picked.GetValue<double>();
}

struct SecEdgarQ2NumRowSnapshot {
	std::string adsh;
	std::string tag;
	std::string uom;
	double value = 0.0;
	bool has_value = false;
};

struct SecEdgarQ2PendingNumOp {
	int8_t sign = 0;
	SecEdgarQ2NumRowSnapshot snapshot;
};

struct SecEdgarQ2PendingNumLightOp {
	SecEdgarQ2NumRowSnapshot snapshot;
	int8_t sign = 0;
};

std::optional<SecEdgarQ2NumRowSnapshot> ReadNumRowSnapshot(const duckdb::TableMutationRow &row,
                                                           const aux_state::TableDependency &dependency,
                                                           bool use_new_value) {
	const auto *adsh_column = FindColumn(dependency, "adsh");
	const auto *tag_column = FindColumn(dependency, "tag");
	const auto *uom_column = FindColumn(dependency, "uom");
	const auto *value_column = FindColumn(dependency, "value");
	if (!adsh_column || !tag_column || !uom_column || !value_column) {
		return std::nullopt;
	}
	auto adsh = PickStringColumn(row, *adsh_column, use_new_value);
	auto tag = PickStringColumn(row, *tag_column, use_new_value);
	auto uom = PickStringColumn(row, *uom_column, use_new_value);
	auto value = PickDoubleColumn(row, *value_column, use_new_value);
	if (!adsh || !tag || !uom) {
		return std::nullopt;
	}
	SecEdgarQ2NumRowSnapshot snapshot;
	snapshot.adsh = *adsh;
	snapshot.tag = *tag;
	snapshot.uom = *uom;
	if (value) {
		snapshot.value = *value;
		snapshot.has_value = true;
	}
	return snapshot;
}

void NoteAffectedSlots(aux_state::InvalidationPlan &plan) {
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(kSecEdgarQ2MaxPureMapSlot);
	plan.partitions.insert(kSecEdgarQ2MaxCandidatesSlot);
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	static const std::unordered_set<std::string> kRelevantColumns = {"adsh", "tag", "uom", "value"};
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

bool SnapshotIsRelevantPureInsert(const SecEdgarQ2NumRowSnapshot &snapshot) {
	return snapshot.uom == "pure" && snapshot.has_value;
}

} // namespace

struct SecEdgarQ2IncrementalTxnBuffer::Impl {
	std::vector<SecEdgarQ2PendingNumOp> row_ops;
	std::vector<SecEdgarQ2PendingNumLightOp> pending_row_light_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

SecEdgarQ2IncrementalTxnBuffer::SecEdgarQ2IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {
}
SecEdgarQ2IncrementalTxnBuffer::~SecEdgarQ2IncrementalTxnBuffer() = default;
SecEdgarQ2IncrementalTxnBuffer::SecEdgarQ2IncrementalTxnBuffer(SecEdgarQ2IncrementalTxnBuffer &&) noexcept = default;
SecEdgarQ2IncrementalTxnBuffer &
SecEdgarQ2IncrementalTxnBuffer::operator=(SecEdgarQ2IncrementalTxnBuffer &&) noexcept = default;

void SecEdgarQ2IncrementalTxnBuffer::Clear() {
	impl = std::make_unique<Impl>();
}

bool SecEdgarQ2IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || impl->cheap_rows_touched > 0 || !impl->pending_row_light_ops.empty());
}

aux_state::InvalidationPlan SecEdgarQ2IncrementalTxnBuffer::ExtraInvalidationPlan() const {
	return impl ? impl->extra_partitions : aux_state::InvalidationPlan {};
}

bool SecEdgarQ2IncrementalTxnBuffer::IsPoisoned() const {
	return impl && impl->poisoned;
}

uint64_t SecEdgarQ2IncrementalTxnBuffer::RowOpCount() const {
	return impl ? impl->row_ops.size() : 0;
}

uint64_t SecEdgarQ2IncrementalTxnBuffer::PendingLightOpCount() const {
	return impl ? impl->pending_row_light_ops.size() : 0;
}

uint64_t SecEdgarQ2IncrementalTxnBuffer::CheapRowsTouched() const {
	return impl ? impl->cheap_rows_touched : 0;
}

bool SecEdgarQ2IncrementalTxnBuffer::HeavyCaptureDone() const {
	return impl && impl->heavy_capture_done;
}

bool SecEdgarQ2IncrementalTxnBuffer::BulkMutation() const {
	return impl && impl->bulk_mutation;
}

void SecEdgarQ2AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                          const std::vector<aux_state::TableDependency> &dependencies,
                                          SecEdgarQ2IncrementalTxnBuffer &buffer) {
	(void)context;
	if (!buffer.impl) {
		buffer.impl = std::make_unique<SecEdgarQ2IncrementalTxnBuffer::Impl>();
	}
	if (buffer.impl->poisoned) {
		return;
	}
	const auto *num = FindTableDependency(dependencies, "num");
	if (!num || event.table_oid != num->table_oid) {
		return;
	}

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		for (const auto &row : event.rows) {
			auto snapshot = ReadNumRowSnapshot(row, *num, true);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			if (!SnapshotIsRelevantPureInsert(*snapshot)) {
				buffer.impl->cheap_rows_touched++;
				continue;
			}
			buffer.impl->pending_row_light_ops.push_back(SecEdgarQ2PendingNumLightOp {*snapshot, 1});
			NoteAffectedSlots(buffer.impl->extra_partitions);
			buffer.impl->cheap_rows_touched++;
		}
		break;
	case duckdb::TableMutationKind::DELETE:
		buffer.impl->poisoned = true;
		return;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, *num)) {
			return;
		}
		buffer.impl->poisoned = true;
		return;
	default:
		break;
	}
	if (buffer.impl->cheap_rows_touched > kSecEdgarQ2MaxLightCaptureRows) {
		buffer.impl->bulk_mutation = true;
	}
}

bool SecEdgarQ2FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                          const std::vector<aux_state::TableDependency> &dependencies,
                                          SecEdgarQ2IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)dependencies;
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return false;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return !buffer.impl->row_ops.empty();
	}
	for (const auto &pending : buffer.impl->pending_row_light_ops) {
		buffer.impl->row_ops.push_back(SecEdgarQ2PendingNumOp {pending.sign, pending.snapshot});
	}
	buffer.impl->pending_row_light_ops.clear();
	buffer.impl->heavy_capture_done = true;
	return true;
}

void MaybeMergePendingSecEdgarQ2LightOps(duckdb::ClientContext &context,
                                         const std::vector<aux_state::TableDependency> &dependencies,
                                         SecEdgarQ2IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return;
	}
	if (buffer.impl->cheap_rows_touched > kSecEdgarQ2MaxLightCaptureRows) {
		return;
	}
	SecEdgarQ2FinalizeIncrementalCapture(context, dependencies, buffer);
}

void SecEdgarQ2AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                     const std::vector<aux_state::TableDependency> &dependencies,
                                     SecEdgarQ2IncrementalTxnBuffer &buffer) {
	SecEdgarQ2AccumulatePreMutationLight(context, event, dependencies, buffer);
	MaybeMergePendingSecEdgarQ2LightOps(context, dependencies, buffer);
}

bool ComputeSecEdgarQ2IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                       const aux_state::InvalidationPlan &plan, SecEdgarQ2IncrementalTxnBuffer &buffer,
                                       SecEdgarQ2ComputedIncrementalDelta &out) {
	(void)context;
	const auto start = std::chrono::steady_clock::now();
	out = SecEdgarQ2ComputedIncrementalDelta {};
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->row_ops.empty()) {
		return false;
	}
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		return false;
	}

	for (const auto slot : {kSecEdgarQ2MaxPureMapSlot, kSecEdgarQ2Fy2022CohortSlot, kSecEdgarQ2MaxCandidatesSlot}) {
		if (store.GetStatus(slot) != aux_state::PartitionStatus::Valid) {
			return false;
		}
	}

	auto max_map_payload = store.GetReadablePayload(kSecEdgarQ2MaxPureMapSlot);
	auto cohort_payload = store.GetReadablePayload(kSecEdgarQ2Fy2022CohortSlot);
	auto candidates_payload = store.GetReadablePayload(kSecEdgarQ2MaxCandidatesSlot);
	if (!max_map_payload || !cohort_payload || !candidates_payload) {
		return false;
	}

	auto max_map = *std::static_pointer_cast<const SecEdgarQ2MaxPureMapPartition>(max_map_payload);
	auto cohort = *std::static_pointer_cast<const SecEdgarQ2Fy2022CohortPartition>(cohort_payload);
	auto candidates = *std::static_pointer_cast<const SecEdgarQ2CandidatePartition>(candidates_payload);
	if (candidates.rows_at_max.empty() && !candidates.output_rows.empty()) {
		return false;
	}

	for (const auto &op : buffer.impl->row_ops) {
		if (op.sign <= 0 || !SnapshotIsRelevantPureInsert(op.snapshot)) {
			return false;
		}
		const auto adsh_key = SecEdgarQ2AdshKey::FromStored(op.snapshot.adsh);
		if (!ApplySecEdgarQ2PureNumInsert(max_map, cohort, candidates, adsh_key, op.snapshot.tag, op.snapshot.value)) {
			return false;
		}
	}

	auto stage = [&](SecEdgarQ2PartitionKey key, auto payload) {
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = key;
		update.payload = std::make_shared<std::decay_t<decltype(payload)>>(std::move(payload));
		update.expected_partition_version = store.GetPartitionVersion(key);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(key);
	};

	stage(kSecEdgarQ2MaxPureMapSlot, max_map);
	stage(kSecEdgarQ2MaxCandidatesSlot, candidates);

	out.success = true;
	const auto end = std::chrono::steady_clock::now();
	const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
	RecordSecEdgarQ2IncrementalApplyAttempt(true, ns);
	return true;
}

void PublishSecEdgarQ2IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ2ComputedIncrementalDelta &delta,
                                       uint64_t valid_through_mutation_epoch) {
	if (!delta.success) {
		return;
	}
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

SecEdgarQ2IncrementalTxnBuffer &GetSecEdgarQ2IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kSecEdgarQ2AuxAcceleratorId);
	auto *q2 = dynamic_cast<SecEdgarQ2IncrementalTxnBuffer *>(&base);
	if (!q2) {
		throw std::runtime_error("incremental buffer for sec_edgar_q2_aux has unexpected type");
	}
	return *q2;
}

aux_state::InvalidationPlan AccumulateSecEdgarQ2Mutation(duckdb::ClientContext &context,
                                                         const duckdb::TableMutationEvent &event,
                                                         const std::vector<aux_state::TableDependency> &dependencies,
                                                         aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetSecEdgarQ2IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsSecEdgarQ2DependentTableMutation(event, dependencies);
	const auto *num = FindTableDependency(dependencies, "num");
	if (num && event.table_oid == num->table_oid) {
		SecEdgarQ2AccumulatePreMutationLight(context, event, dependencies, incremental);
	}
	if (dependent) {
		RecordSecEdgarQ2DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordSecEdgarQ2WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
