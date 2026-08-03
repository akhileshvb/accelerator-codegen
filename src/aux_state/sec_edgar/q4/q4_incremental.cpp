#include "accelerator_codegen/sec_edgar_q4_incremental.hpp"
#include "accelerator_codegen/sec_edgar_q4_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q4_write_policy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/sec_edgar_q4_aux.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kSecEdgarQ4MaxLightCaptureRows = 1024;

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

struct SecEdgarQ4NumRowSnapshot {
	std::string adsh;
	std::string tag;
	std::string version;
	std::string uom;
	double value = 0.0;
	bool has_value = false;
};

struct SecEdgarQ4PendingNumOp {
	int8_t sign = 0;
	SecEdgarQ4NumRowSnapshot snapshot;
};

struct SecEdgarQ4PendingNumLightOp {
	SecEdgarQ4NumRowSnapshot snapshot;
	int8_t sign = 0;
};

std::optional<SecEdgarQ4NumRowSnapshot> ReadNumRowSnapshot(const duckdb::TableMutationRow &row,
                                                           const aux_state::TableDependency &dependency,
                                                           bool use_new_value) {
	const auto *adsh_column = FindColumn(dependency, "adsh");
	const auto *tag_column = FindColumn(dependency, "tag");
	const auto *version_column = FindColumn(dependency, "version");
	const auto *uom_column = FindColumn(dependency, "uom");
	const auto *value_column = FindColumn(dependency, "value");
	if (!adsh_column || !tag_column || !version_column || !uom_column || !value_column) {
		return std::nullopt;
	}
	auto adsh = PickStringColumn(row, *adsh_column, use_new_value);
	auto tag = PickStringColumn(row, *tag_column, use_new_value);
	auto version = PickStringColumn(row, *version_column, use_new_value);
	auto uom = PickStringColumn(row, *uom_column, use_new_value);
	auto value = PickDoubleColumn(row, *value_column, use_new_value);
	if (!adsh || !tag || !version || !uom) {
		return std::nullopt;
	}
	SecEdgarQ4NumRowSnapshot snapshot;
	snapshot.adsh = *adsh;
	snapshot.tag = *tag;
	snapshot.version = *version;
	snapshot.uom = *uom;
	if (value) {
		snapshot.value = *value;
		snapshot.has_value = true;
	}
	return snapshot;
}

void NoteAffectedSlots(aux_state::InvalidationPlan &plan) {
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(kSecEdgarQ4GroupAggregatesSlot);
	plan.partitions.insert(kSecEdgarQ4OutputSlot);
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	static const std::unordered_set<std::string> kRelevantColumns = {"adsh", "tag", "version", "uom", "value"};
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

bool SnapshotIsRelevantUsdRow(const SecEdgarQ4NumRowSnapshot &snapshot) {
	return snapshot.uom == "USD" && snapshot.has_value;
}

} // namespace

struct SecEdgarQ4IncrementalTxnBuffer::Impl {
	std::vector<SecEdgarQ4PendingNumOp> row_ops;
	std::vector<SecEdgarQ4PendingNumLightOp> pending_row_light_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

SecEdgarQ4IncrementalTxnBuffer::SecEdgarQ4IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {
}
SecEdgarQ4IncrementalTxnBuffer::~SecEdgarQ4IncrementalTxnBuffer() = default;
SecEdgarQ4IncrementalTxnBuffer::SecEdgarQ4IncrementalTxnBuffer(SecEdgarQ4IncrementalTxnBuffer &&) noexcept = default;
SecEdgarQ4IncrementalTxnBuffer &
SecEdgarQ4IncrementalTxnBuffer::operator=(SecEdgarQ4IncrementalTxnBuffer &&) noexcept = default;

void SecEdgarQ4IncrementalTxnBuffer::Clear() {
	impl = std::make_unique<Impl>();
}

bool SecEdgarQ4IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || impl->cheap_rows_touched > 0 || !impl->pending_row_light_ops.empty());
}

aux_state::InvalidationPlan SecEdgarQ4IncrementalTxnBuffer::ExtraInvalidationPlan() const {
	return impl ? impl->extra_partitions : aux_state::InvalidationPlan {};
}

bool SecEdgarQ4IncrementalTxnBuffer::IsPoisoned() const {
	return impl && impl->poisoned;
}

uint64_t SecEdgarQ4IncrementalTxnBuffer::RowOpCount() const {
	return impl ? impl->row_ops.size() : 0;
}

uint64_t SecEdgarQ4IncrementalTxnBuffer::PendingLightOpCount() const {
	return impl ? impl->pending_row_light_ops.size() : 0;
}

uint64_t SecEdgarQ4IncrementalTxnBuffer::CheapRowsTouched() const {
	return impl ? impl->cheap_rows_touched : 0;
}

bool SecEdgarQ4IncrementalTxnBuffer::HeavyCaptureDone() const {
	return impl && impl->heavy_capture_done;
}

bool SecEdgarQ4IncrementalTxnBuffer::BulkMutation() const {
	return impl && impl->bulk_mutation;
}

void SecEdgarQ4AccumulatePreMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                          const std::vector<aux_state::TableDependency> &dependencies,
                                          SecEdgarQ4IncrementalTxnBuffer &buffer) {
	(void)context;
	if (!buffer.impl) {
		buffer.impl = std::make_unique<SecEdgarQ4IncrementalTxnBuffer::Impl>();
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
			if (!SnapshotIsRelevantUsdRow(*snapshot)) {
				buffer.impl->cheap_rows_touched++;
				continue;
			}
			buffer.impl->pending_row_light_ops.push_back(SecEdgarQ4PendingNumLightOp {*snapshot, 1});
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
	if (buffer.impl->cheap_rows_touched > kSecEdgarQ4MaxLightCaptureRows) {
		buffer.impl->bulk_mutation = true;
	}
}

bool SecEdgarQ4FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                          const std::vector<aux_state::TableDependency> &dependencies,
                                          SecEdgarQ4IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)dependencies;
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return false;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return !buffer.impl->row_ops.empty();
	}
	for (const auto &pending : buffer.impl->pending_row_light_ops) {
		buffer.impl->row_ops.push_back(SecEdgarQ4PendingNumOp {pending.sign, pending.snapshot});
	}
	buffer.impl->pending_row_light_ops.clear();
	buffer.impl->heavy_capture_done = true;
	return true;
}

void MaybeMergePendingSecEdgarQ4LightOps(duckdb::ClientContext &context,
                                         const std::vector<aux_state::TableDependency> &dependencies,
                                         SecEdgarQ4IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return;
	}
	if (buffer.impl->pending_row_light_ops.empty()) {
		return;
	}
	if (buffer.impl->cheap_rows_touched > kSecEdgarQ4MaxLightCaptureRows) {
		return;
	}
	SecEdgarQ4FinalizeIncrementalCapture(context, dependencies, buffer);
}

void SecEdgarQ4AccumulatePreMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                     const std::vector<aux_state::TableDependency> &dependencies,
                                     SecEdgarQ4IncrementalTxnBuffer &buffer) {
	SecEdgarQ4AccumulatePreMutationLight(context, event, dependencies, buffer);
	MaybeMergePendingSecEdgarQ4LightOps(context, dependencies, buffer);
}

bool ComputeSecEdgarQ4IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                       const aux_state::InvalidationPlan &plan, SecEdgarQ4IncrementalTxnBuffer &buffer,
                                       SecEdgarQ4ComputedIncrementalDelta &out) {
	(void)context;
	const auto start = std::chrono::steady_clock::now();
	out = SecEdgarQ4ComputedIncrementalDelta {};
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->row_ops.empty()) {
		return false;
	}
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		return false;
	}

	for (const auto slot : SecEdgarQ4FixedPartitionKeys()) {
		if (store.GetStatus(slot) != aux_state::PartitionStatus::Valid) {
			return false;
		}
	}

	auto cohort_payload = store.GetReadablePayload(kSecEdgarQ4SicCohortSlot);
	auto pre_payload = store.GetReadablePayload(kSecEdgarQ4PreEqBridgeSlot);
	auto tag_payload = store.GetReadablePayload(kSecEdgarQ4TagTlabelSlot);
	auto aggregates_payload = store.GetReadablePayload(kSecEdgarQ4GroupAggregatesSlot);
	auto output_payload = store.GetReadablePayload(kSecEdgarQ4OutputSlot);
	if (!cohort_payload || !pre_payload || !tag_payload || !aggregates_payload || !output_payload) {
		return false;
	}

	auto cohort = *std::static_pointer_cast<const SecEdgarQ4SicCohortPartition>(cohort_payload);
	auto pre_bridge = *std::static_pointer_cast<const SecEdgarQ4PreEqBridgePartition>(pre_payload);
	auto tag_tlabel = *std::static_pointer_cast<const SecEdgarQ4TagTlabelPartition>(tag_payload);
	auto aggregates = *std::static_pointer_cast<const SecEdgarQ4GroupAggregatesPartition>(aggregates_payload);
	auto output = *std::static_pointer_cast<const SecEdgarQ4OutputPartition>(output_payload);

	for (const auto &op : buffer.impl->row_ops) {
		if (!SnapshotIsRelevantUsdRow(op.snapshot)) {
			continue;
		}
		const auto adsh_key = SecEdgarQ4AdshKey::FromStored(op.snapshot.adsh);
		const auto tag_version_key = SecEdgarQ4TagVersionKey::FromStored(op.snapshot.tag, op.snapshot.version);
		if (!ApplySecEdgarQ4UsdNumDelta(cohort, pre_bridge, tag_tlabel, aggregates, output, adsh_key, tag_version_key,
		                                op.snapshot.value, op.sign)) {
			return false;
		}
	}

	auto stage = [&](SecEdgarQ4PartitionKey key, auto payload) {
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = key;
		update.payload = std::make_shared<std::decay_t<decltype(payload)>>(std::move(payload));
		update.expected_partition_version = store.GetPartitionVersion(key);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(key);
	};

	stage(kSecEdgarQ4GroupAggregatesSlot, aggregates);
	stage(kSecEdgarQ4OutputSlot, output);

	out.success = true;
	const auto end = std::chrono::steady_clock::now();
	const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
	RecordSecEdgarQ4IncrementalApplyAttempt(true, ns);
	return true;
}

void PublishSecEdgarQ4IncrementalDelta(aux_state::PartitionedStateStore &store, const SecEdgarQ4ComputedIncrementalDelta &delta,
                                       uint64_t valid_through_mutation_epoch) {
	if (!delta.success) {
		return;
	}
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

SecEdgarQ4IncrementalTxnBuffer &GetSecEdgarQ4IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kSecEdgarQ4AuxAcceleratorId);
	auto *q4 = dynamic_cast<SecEdgarQ4IncrementalTxnBuffer *>(&base);
	if (!q4) {
		throw std::runtime_error("incremental buffer for sec_edgar_q4_aux has unexpected type");
	}
	return *q4;
}

aux_state::InvalidationPlan AccumulateSecEdgarQ4Mutation(duckdb::ClientContext &context,
                                                         const duckdb::TableMutationEvent &event,
                                                         const std::vector<aux_state::TableDependency> &dependencies,
                                                         aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetSecEdgarQ4IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsSecEdgarQ4DependentTableMutation(event, dependencies);
	const auto *num = FindTableDependency(dependencies, "num");
	if (num && event.table_oid == num->table_oid) {
		SecEdgarQ4AccumulatePreMutationLight(context, event, dependencies, incremental);
	}
	if (dependent) {
		RecordSecEdgarQ4DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordSecEdgarQ4WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
