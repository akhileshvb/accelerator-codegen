#include "accelerator_codegen/tpch_q3_maintenance_policy.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unordered_set>

#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q3_diagnostics.hpp"
#include "accelerator_codegen/tpch_q3_incremental.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/tpch_q3_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q3_write_policy.hpp"

namespace accelerator_codegen {
namespace {

using namespace aux_state::maintenance;

MaintenanceAction g_last_action = MaintenanceAction::NoOp;

void ApplyInvalidation(aux_state::PartitionedStateStore &store, const aux_state::InvalidationPlan &plan) {
	if (plan.IsEmpty()) return;
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		store.InvalidateAll();
		RecordQ3InvalidationApplied(true, 0);
		return;
	}
	uint64_t count = 0;
	for (auto partition : plan.partitions) {
		store.InvalidatePartition(partition);
		count++;
	}
	if (count > 0) RecordQ3InvalidationApplied(false, count);
}

double CandidateImmediateCost(const std::vector<Q3CandidateDecisionDetail> &candidates, MaintenanceAction action) {
	for (const auto &candidate : candidates) {
		if (candidate.action == action) return candidate.estimate.immediate_us;
	}
	return 0.0;
}

void FillDecisionRecord(Q3MaintenanceDecisionRecord &record, const SelectionResult &selection,
                        const std::vector<Q3CandidateDecisionDetail> &candidates, bool low_confidence,
                        const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                        bool heavy_capture_ran, const Q3ApplyDeltaEligibility &eligibility) {
	record.selected = selection.action;
	record.low_confidence = low_confidence;
	record.selected_predicted_cost_us = selection.selected_cost_us;
	record.selected_predicted_immediate_us = CandidateImmediateCost(candidates, selection.action);
	record.rows_touched = features.rows_touched;
	record.affected_partition_count = features.affected_partition_count;
	record.expected_queries_before_next_write = workload.expected_queries_before_next_write;
	record.heavy_capture_ran = heavy_capture_ran;
	record.stage1_selected_apply_delta = selection.action == MaintenanceAction::ApplyDelta;
	record.apply_delta_eligibility = eligibility;
	record.candidate_details = candidates;
	record.candidate_costs.clear();
	for (const auto &candidate : candidates) {
		record.candidate_costs.emplace_back(candidate.action, candidate.total_overhead_us);
	}
}

} // namespace

void ExecuteQ3MaintenancePreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                   const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer,
                                   StagedMaintenanceOutcome &outcome) {
	const auto start = std::chrono::steady_clock::now();
	outcome.Clear();
	outcome.active = true;

	auto &incremental = GetQ3IncrementalTxnBuffer(txn_buffer);
	const auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto &dependencies = registry->GetAcceleratorDependencies(kQ3AuxAcceleratorId);
	const bool low_confidence = !Q3HasSufficientTrainingSamples();
	const auto workload = BuildQ3WorkloadFeatures(store);
	const auto &params = GetQ3CostModelParams();

	const auto pending_before = incremental.PendingLightOpCount();
	if (!incremental.BulkMutation() && pending_before > 0) {
		MaybeMergePendingQ3LightOps(context, dependencies, incremental);
	}
	const bool heavy_capture_ran = pending_before > 0 && incremental.PendingLightOpCount() == 0 && incremental.RowOpCount() > 0;

	const auto mutation_features = BuildQ3MutationBatchFeatures(plan, incremental, incremental.CheapRowsTouched());
	const auto eligibility = BuildQ3ApplyDeltaEligibility(mutation_features, params, low_confidence, pending_before,
	                                                      incremental.RowOpCount(), incremental.CrossPartitionOpCount());
	auto candidates = EnumerateQ3ActionCandidatesDetailed(mutation_features, workload, params, low_confidence);
	auto selection = SelectQ3MaintenanceAction(mutation_features, workload, g_last_action);

	outcome.selected = selection.action;
	outcome.trace.selected = selection.action;
	outcome.trace.low_confidence = low_confidence;
	outcome.trace.selected_predicted_cost_us = selection.selected_cost_us;

	Q3MaintenanceDecisionRecord record;
	record.mutation_class = mutation_features.class_id;
	FillDecisionRecord(record, selection, candidates, low_confidence, mutation_features, workload, heavy_capture_ran, eligibility);

	switch (selection.action) {
	case MaintenanceAction::NoOp:
		RecordQ3HybridActionSelected(selection.action, low_confidence);
		break;
	case MaintenanceAction::ApplyDelta: {
		Q3ComputedIncrementalDelta delta;
		if (!ComputeQ3IncrementalDelta(context, store, plan, incremental, delta)) {
			outcome.selected = MaintenanceAction::InvalidateAndFallback;
			outcome.invalidations = plan.scope == aux_state::InvalidationScope::None ? incremental.ExtraInvalidationPlan() : plan;
			if (outcome.invalidations.IsEmpty() && incremental.HasPendingChanges()) {
				outcome.invalidations.scope = aux_state::InvalidationScope::AllPartitions;
			}
			RecordQ3IncrementalApplyAttempt(false, 0);
			RecordQ3IncrementalSuccessObservation(false);
			RecordQ3HybridActionSelected(outcome.selected, low_confidence);
			break;
		}
		outcome.partition_updates = std::move(delta.partition_updates);
		RecordQ3IncrementalApplyAttempt(true, 0);
		RecordQ3IncrementalSuccessObservation(true);
		RecordQ3HybridActionSelected(selection.action, low_confidence);
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
		RecordQ3HybridActionSelected(selection.action, low_confidence);
		break;
	case MaintenanceAction::RebuildAll:
		outcome.invalidations.scope = aux_state::InvalidationScope::AllPartitions;
		outcome.pending_lazy_rebuild = outcome.invalidations;
		RecordQ3HybridActionSelected(selection.action, low_confidence);
		break;
	case MaintenanceAction::InvalidateAndFallback:
		outcome.invalidations = plan.scope == aux_state::InvalidationScope::None ? incremental.ExtraInvalidationPlan() : plan;
		if (outcome.invalidations.IsEmpty() && incremental.HasPendingChanges()) {
			outcome.invalidations.scope = aux_state::InvalidationScope::AllPartitions;
		}
		RecordQ3HybridActionSelected(selection.action, low_confidence);
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
	AppendQ3MaintenanceDecisionRecord(record);
	MaybeAppendQ3DecisionCsv(record);
	RecordQ3HybridPredictionError(outcome.trace.prediction_error_us);
	ResetQ3QueriesSinceLastWrite();
	g_last_action = outcome.selected;
}

void PublishQ3MaintenanceOnCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                  aux_state::TxnInvalidationBuffer &txn_buffer, StagedMaintenanceOutcome &outcome) {
	if (!outcome.active) return;
	if (!MaintenanceOutcomeHasPublishEffect(outcome)) {
		outcome.Clear();
		return;
	}
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &maintenance = registry->GetMaintenanceState(kQ3AuxAcceleratorId);
	const auto publish_epoch = maintenance.BumpCommittedMutationEpoch();
	ApplyInvalidation(store, outcome.invalidations);
	if (!outcome.partition_updates.empty()) {
		aux_state::maintenance::PublishValidatedPartitionUpdates(store, outcome.partition_updates, publish_epoch);
	}
	if (!outcome.pending_lazy_rebuild.IsEmpty()) {
		maintenance.MergeGlobalPendingLazyRebuild(outcome.pending_lazy_rebuild);
		RecordQ3LazyRebuildScheduled();
	}
	(void)context;
	(void)txn_buffer;
	outcome.Clear();
}

void DiscardQ3Maintenance(StagedMaintenanceOutcome &outcome) {
	outcome.Clear();
}

class Q3MaintenanceHook final : public aux_state::maintenance::MaintenanceHook {
public:
	void PlanMaintenance(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                     const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer,
	                     StagedMaintenanceOutcome &outcome) override {
		ExecuteQ3MaintenancePreCommit(context, store, plan, txn_buffer, outcome);
	}
	void PublishMaintenance(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
	                        aux_state::TxnInvalidationBuffer &txn_buffer, StagedMaintenanceOutcome &outcome) override {
		PublishQ3MaintenanceOnCommit(context, store, txn_buffer, outcome);
	}
	void DiscardMaintenance(StagedMaintenanceOutcome &outcome) override {
		DiscardQ3Maintenance(outcome);
	}
};

std::shared_ptr<aux_state::maintenance::MaintenanceHook> CreateQ3MaintenanceHook() {
	return std::make_shared<Q3MaintenanceHook>();
}

} // namespace accelerator_codegen
