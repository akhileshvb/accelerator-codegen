#include "accelerator_codegen/sec_edgar_q6_maintenance_policy.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unordered_set>

#include "accelerator_codegen/sec_edgar_q6_aux.hpp"
#include "accelerator_codegen/sec_edgar_q6_diagnostics.hpp"
#include "accelerator_codegen/sec_edgar_q6_incremental.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/sec_edgar_q6_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q6_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q6_write_policy.hpp"

namespace accelerator_codegen {
namespace {

using namespace aux_state::maintenance;

MaintenanceAction g_last_action = MaintenanceAction::NoOp;

void ApplyInvalidation(aux_state::PartitionedStateStore &store, const aux_state::InvalidationPlan &plan) {
	if (plan.IsEmpty()) {
		return;
	}
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		store.InvalidateAll();
		RecordSecEdgarQ6InvalidationApplied(true, 0);
		return;
	}
	uint64_t count = 0;
	for (auto partition : plan.partitions) {
		store.InvalidatePartition(partition);
		count++;
	}
	if (count > 0) {
		RecordSecEdgarQ6InvalidationApplied(false, count);
	}
}

double CandidateImmediateCost(const std::vector<SecEdgarQ6CandidateDecisionDetail> &candidates, MaintenanceAction action) {
	for (const auto &candidate : candidates) {
		if (candidate.action == action) {
			return candidate.estimate.immediate_us;
		}
	}
	return 0.0;
}

void FillDecisionRecord(SecEdgarQ6MaintenanceDecisionRecord &record, const SelectionResult &selection,
                        const std::vector<SecEdgarQ6CandidateDecisionDetail> &candidates, bool low_confidence,
                        const SecEdgarQ6MutationBatchFeatures &features, const SecEdgarQ6WorkloadFeatures &workload,
                        bool heavy_capture_ran, const SecEdgarQ6ApplyDeltaEligibility &apply_delta_eligibility) {
	record.selected = selection.action;
	record.low_confidence = low_confidence;
	record.selected_predicted_cost_us = selection.selected_cost_us;
	record.selected_predicted_immediate_us = CandidateImmediateCost(candidates, selection.action);
	record.rows_touched = features.rows_touched;
	record.affected_partition_count = features.affected_partition_count;
	record.expected_queries_before_next_write = workload.expected_queries_before_next_write;
	record.heavy_capture_ran = heavy_capture_ran;
	record.stage1_selected_apply_delta = selection.action == MaintenanceAction::ApplyDelta;
	record.apply_delta_eligibility = apply_delta_eligibility;
	record.candidate_details = candidates;
	record.candidate_costs.clear();
	for (const auto &candidate : candidates) {
		record.candidate_costs.emplace_back(candidate.action, candidate.total_overhead_us);
	}
}

void LogApplyDeltaEligibility(const SecEdgarQ6ApplyDeltaEligibility &eligibility) {
	std::cerr << "  apply_delta_eligible=" << eligibility.eligible
	          << " training_samples_sufficient=" << eligibility.training_samples_sufficient
	          << " low_confidence=" << eligibility.low_confidence << " bulk_mutation=" << eligibility.bulk_mutation
	          << " whole_state_required=" << eligibility.whole_state_required
	          << " buffer_poisoned=" << eligibility.incremental_buffer_poisoned
	          << " buffer_complete=" << eligibility.incremental_buffer_complete
	          << " old_new_available=" << eligibility.old_new_values_available
	          << " heavy_capture_available=" << eligibility.heavy_capture_available
	          << " incremental_supported=" << eligibility.incremental_supported
	          << " rows_exceed_max=" << eligibility.rows_exceed_max
	          << " pending_light_ops=" << eligibility.pending_light_ops << " row_ops=" << eligibility.row_ops
	          << " apply_delta_predicted_immediate_us=" << eligibility.apply_delta_predicted_immediate_us;
	if (eligibility.primary_exclusion_reason) {
		std::cerr << " exclusion=" << eligibility.primary_exclusion_reason;
	}
	std::cerr << '\n';
}

bool ShouldLogPointLineitemDecision(const SecEdgarQ6MaintenanceDecisionRecord &record) {
	const char *point_log = std::getenv("ACCEL_SecEdgarQ6_DECISION_LOG_POINT_LINEITEM");
	if (!point_log || !*point_log) {
		return false;
	}
	return record.mutation_class == SecEdgarQ6MutationClass::PointLineitemUpdate && record.rows_touched <= 1;
}

void MaybeLogDecisionDebug(const SecEdgarQ6MaintenanceDecisionRecord &record) {
	const char *debug = std::getenv("ACCEL_SecEdgarQ6_MAINTENANCE_DEBUG");
	const char *sample = std::getenv("ACCEL_SecEdgarQ6_DECISION_SAMPLE");
	const bool sample_mode = sample && *sample;
	const bool point_lineitem_mode = ShouldLogPointLineitemDecision(record);
	if ((!debug || !*debug) && !sample_mode && !point_lineitem_mode) {
		return;
	}
	static std::unordered_set<uint8_t> logged_classes;
	if (sample_mode && !point_lineitem_mode && logged_classes.count(static_cast<uint8_t>(record.mutation_class)) > 0) {
		return;
	}
	if (sample_mode && !point_lineitem_mode) {
		logged_classes.insert(static_cast<uint8_t>(record.mutation_class));
	}

	std::cerr << "q6_maintenance mutation_class=" << SecEdgarQ6MutationClassName(record.mutation_class)
	          << " selected=" << MaintenanceActionName(record.selected) << " rows=" << record.rows_touched
	          << " partitions=" << record.affected_partition_count
	          << " expected_queries=" << record.expected_queries_before_next_write
	          << " predicted_total_us=" << record.selected_predicted_cost_us
	          << " predicted_immediate_us=" << record.selected_predicted_immediate_us
	          << " actual_immediate_us=" << record.actual_cost_us
	          << " prediction_error_immediate_us=" << record.prediction_error_us << '\n';
	LogApplyDeltaEligibility(record.apply_delta_eligibility);
	for (const auto &candidate : record.candidate_details) {
		std::cerr << "  candidate=" << MaintenanceActionName(candidate.action) << " supported=" << candidate.supported
		          << " immediate_us=" << candidate.estimate.immediate_us
		          << " diff_us=" << candidate.estimate.differential_future_query_us
		          << " total_us=" << candidate.total_overhead_us;
		if (!candidate.exclusion_reason.empty()) {
			std::cerr << " excluded=" << candidate.exclusion_reason;
		}
		std::cerr << '\n';
	}
}

SelectionResult ChooseAction(const SecEdgarQ6MutationBatchFeatures &mutation_features, const SecEdgarQ6WorkloadFeatures &workload) {
	return SelectSecEdgarQ6MaintenanceAction(mutation_features, workload, g_last_action);
}

void RecordFinalHybridAction(MaintenanceAction action, bool low_confidence) {
	RecordSecEdgarQ6HybridActionSelected(action, low_confidence);
}

} // namespace

void ExecuteSecEdgarQ6MaintenancePreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                    const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer,
                                    StagedMaintenanceOutcome &outcome) {
	const auto start = std::chrono::steady_clock::now();
	outcome.Clear();
	outcome.active = true;

	auto &incremental = GetSecEdgarQ6IncrementalTxnBuffer(txn_buffer);
	const auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto &q6_dependencies = registry->GetAcceleratorDependencies("sec_edgar_q6_aux");

	const bool low_confidence = !SecEdgarQ6HasSufficientTrainingSamples();
	const auto workload = BuildSecEdgarQ6WorkloadFeatures(store);
	const auto &params = GetSecEdgarQ6CostModelParams();

	const auto pending_before_merge = incremental.PendingLightOpCount();
	if (!incremental.BulkMutation() && pending_before_merge > 0) {
		MaybeMergePendingSecEdgarQ6LightOps(context, q6_dependencies, incremental);
	}
	const bool heavy_capture_ran =
	    pending_before_merge > 0 && incremental.PendingLightOpCount() == 0 && incremental.RowOpCount() > 0;

	const auto mutation_features =
	    BuildSecEdgarQ6MutationBatchFeatures(plan, incremental, incremental.CheapRowsTouched());
	const auto apply_delta_eligibility = BuildSecEdgarQ6ApplyDeltaEligibility(
	    mutation_features, params, low_confidence, incremental.PendingLightOpCount(), incremental.RowOpCount());
	auto candidates =
	    EnumerateSecEdgarQ6ActionCandidatesDetailed(mutation_features, workload, params, low_confidence);
	auto selection = ChooseAction(mutation_features, workload);

	outcome.selected = selection.action;
	outcome.trace.selected = selection.action;
	outcome.trace.low_confidence = low_confidence;
	outcome.trace.selected_predicted_cost_us = selection.selected_cost_us;

	SecEdgarQ6MaintenanceDecisionRecord record;
	record.mutation_class = mutation_features.class_id;
	FillDecisionRecord(record, selection, candidates, low_confidence, mutation_features, workload, heavy_capture_ran,
	                   apply_delta_eligibility);

	switch (selection.action) {
	case MaintenanceAction::NoOp:
		RecordFinalHybridAction(selection.action, low_confidence);
		break;
	case MaintenanceAction::ApplyDelta: {
		SecEdgarQ6ComputedIncrementalDelta delta;
		const bool ok = ComputeSecEdgarQ6IncrementalDelta(context, store, plan, incremental, delta);
		if (!ok) {
			outcome.selected = MaintenanceAction::RebuildAll;
			outcome.invalidations.scope = aux_state::InvalidationScope::AllPartitions;
			outcome.pending_lazy_rebuild = outcome.invalidations;
			RecordSecEdgarQ6IncrementalApplyAttempt(false, 0);
			RecordSecEdgarQ6IncrementalSuccessObservation(false);
			RecordFinalHybridAction(outcome.selected, low_confidence);
			break;
		}
		outcome.partition_updates = std::move(delta.partition_updates);
		RecordSecEdgarQ6IncrementalApplyAttempt(true, 0);
		RecordSecEdgarQ6IncrementalSuccessObservation(true);
		RecordFinalHybridAction(selection.action, low_confidence);
		break;
	}
	case MaintenanceAction::RebuildPartition:
		outcome.invalidations.scope = aux_state::InvalidationScope::PartitionSet;
		outcome.invalidations.partitions = mutation_features.affected_partitions.empty()
		                                       ? plan.partitions
		                                       : std::unordered_set<aux_state::PartitionKey>(
		                                             mutation_features.affected_partitions.begin(),
		                                             mutation_features.affected_partitions.end());
		outcome.pending_lazy_rebuild = outcome.invalidations;
		RecordFinalHybridAction(selection.action, low_confidence);
		break;
	case MaintenanceAction::RebuildAll:
		outcome.invalidations.scope = aux_state::InvalidationScope::AllPartitions;
		outcome.pending_lazy_rebuild = outcome.invalidations;
		RecordFinalHybridAction(selection.action, low_confidence);
		break;
	case MaintenanceAction::InvalidateAndFallback:
		outcome.invalidations = plan.scope == aux_state::InvalidationScope::None ? incremental.ExtraInvalidationPlan()
		                                                                         : plan;
		if (outcome.invalidations.IsEmpty() && !plan.IsEmpty()) {
			outcome.invalidations = plan;
		}
		if (outcome.invalidations.IsEmpty() && incremental.HasPendingChanges()) {
			outcome.invalidations.scope = aux_state::InvalidationScope::AllPartitions;
		}
		if (!outcome.invalidations.IsEmpty()) {
			outcome.pending_lazy_rebuild = outcome.invalidations;
		}
		RecordFinalHybridAction(selection.action, low_confidence);
		break;
	}

	const auto end = std::chrono::steady_clock::now();
	outcome.trace.actual_cost_us =
	    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
	record.selected_predicted_immediate_us = CandidateImmediateCost(candidates, outcome.selected);
	outcome.trace.prediction_error_us = outcome.trace.actual_cost_us - record.selected_predicted_immediate_us;
	record.actual_cost_us = outcome.trace.actual_cost_us;
	record.prediction_error_us = outcome.trace.prediction_error_us;
	record.selected = outcome.selected;
	AppendSecEdgarQ6MaintenanceDecisionRecord(record);
	MaybeAppendSecEdgarQ6DecisionCsv(record);
	MaybeLogDecisionDebug(record);
	RecordSecEdgarQ6HybridPredictionError(outcome.trace.prediction_error_us);
	ResetSecEdgarQ6QueriesSinceLastWrite();
	g_last_action = outcome.selected;
}

void PublishSecEdgarQ6MaintenanceOnCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                   aux_state::TxnInvalidationBuffer &txn_buffer, StagedMaintenanceOutcome &outcome) {
	if (!outcome.active) {
		return;
	}
	if (!MaintenanceOutcomeHasPublishEffect(outcome)) {
		outcome.Clear();
		return;
	}
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &maintenance = registry->GetMaintenanceState(kSecEdgarQ6AuxAcceleratorId);
	const auto publish_epoch = maintenance.BumpCommittedMutationEpoch();

	ApplyInvalidation(store, outcome.invalidations);

	if (!outcome.partition_updates.empty()) {
		aux_state::maintenance::PublishValidatedPartitionUpdates(store, outcome.partition_updates, publish_epoch);
	}

	if (!outcome.pending_lazy_rebuild.IsEmpty()) {
		maintenance.MergeGlobalPendingLazyRebuild(outcome.pending_lazy_rebuild);
		RecordSecEdgarQ6LazyRebuildScheduled();
	}

	(void)context;
	(void)txn_buffer;
	outcome.Clear();
}

void DiscardSecEdgarQ6Maintenance(StagedMaintenanceOutcome &outcome) {
	outcome.Clear();
}

class SecEdgarQ6MaintenanceHook final : public aux_state::maintenance::MaintenanceHook {
public:
	void PlanMaintenance(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                     const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer,
	                     StagedMaintenanceOutcome &outcome) override {
		ExecuteSecEdgarQ6MaintenancePreCommit(context, store, plan, txn_buffer, outcome);
	}

	void PublishMaintenance(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                        aux_state::TxnInvalidationBuffer &txn_buffer,
	                        StagedMaintenanceOutcome &outcome) override {
		PublishSecEdgarQ6MaintenanceOnCommit(context, store, txn_buffer, outcome);
	}

	void DiscardMaintenance(StagedMaintenanceOutcome &outcome) override {
		DiscardSecEdgarQ6Maintenance(outcome);
	}
};

std::shared_ptr<aux_state::maintenance::MaintenanceHook> CreateSecEdgarQ6MaintenanceHook() {
	return std::make_shared<SecEdgarQ6MaintenanceHook>();
}

} // namespace accelerator_codegen
