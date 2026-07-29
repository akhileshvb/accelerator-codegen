#include "accelerator_codegen/tpch_q3_maintenance.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/maintenance/interference.hpp"
#include "accelerator_codegen/aux_state/maintenance/action_selector.hpp"

namespace accelerator_codegen {
namespace {

using namespace aux_state::maintenance;

const char *ApplyDeltaExclusionReason(const Q3MutationBatchFeatures &features, bool low_confidence) {
	if (low_confidence) return "low_confidence";
	if (features.bulk_mutation) return "bulk_mutation";
	if (features.whole_state_required) return "whole_state_required";
	if (features.incremental_buffer_poisoned) return "incremental_buffer_poisoned";
	if (!features.incremental_buffer_complete) return "incremental_buffer_incomplete";
	if (!features.old_new_values_available) return "old_new_values_unavailable";
	if (!features.heavy_capture_available) return "heavy_capture_deferred";
	if (!features.incremental_supported) return "incremental_unsupported";
	return nullptr;
}

double ExpectedQueryPremium(const Q3WorkloadFeatures &workload) {
	return std::max(0.0, workload.ewma_fallback_latency_us - workload.ewma_aux_hit_latency_us);
}

double ExpectedQueriesForCosting(const Q3WorkloadFeatures &workload) {
	return std::max(1.0, workload.expected_queries_before_next_write);
}

void ApplyInterference(MaintenanceAction action, ActionCostEstimate &estimate, const ActionResourceEstimate &resources,
                       const CostModelParams &params) {
	const auto load = aux_state::GetExecutionLoadRegistry().Snapshot();
	estimate.interference_us = EstimateInterferenceUs(action, resources, load, params);
}

ActionCostEstimate EstimateApplyDelta(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                                      const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.apply_delta_base_us + Q3ApplyDeltaOverheadUs();
	const auto rows = std::max<uint64_t>(1, features.rows_touched);
	estimate.immediate_us += params.apply_delta_per_row_us * static_cast<double>(rows);
	if (!features.heavy_capture_available) {
		estimate.immediate_us += params.apply_delta_part_lineitem_scan_us *
		                         static_cast<double>(features.estimated_lineitem_scan_rows);
	}
	const auto failure_prob = 1.0 - features.recent_incremental_success_rate;
	estimate.immediate_us += failure_prob * (params.invalidation_overhead_us +
	                                      ExpectedQueriesForCosting(workload) * ExpectedQueryPremium(workload));
	ActionResourceEstimate resources;
	resources.estimated_duration_us = estimate.immediate_us;
	ApplyInterference(MaintenanceAction::ApplyDelta, estimate, resources, params);
	(void)features.cross_partition_moves;
	return estimate;
}

ActionCostEstimate EstimateInvalidate(const Q3WorkloadFeatures &workload, const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.invalidation_overhead_us;
	estimate.differential_future_query_us = ExpectedQueriesForCosting(workload) * ExpectedQueryPremium(workload);
	ActionResourceEstimate resources;
	ApplyInterference(MaintenanceAction::InvalidateAndFallback, estimate, resources, params);
	(void)params;
	return estimate;
}

std::vector<Q3CandidateDecisionDetail> BuildCandidateDetails(const Q3MutationBatchFeatures &features,
                                                             const Q3WorkloadFeatures &workload,
                                                             const CostModelParams &params, bool low_confidence) {
	std::vector<Q3CandidateDecisionDetail> details;
	auto add = [&](MaintenanceAction action, bool supported, const char *reason, const ActionCostEstimate &estimate) {
		Q3CandidateDecisionDetail detail;
		detail.action = action;
		detail.supported = supported;
		detail.estimate = estimate;
		detail.total_overhead_us = estimate.TotalOverheadUs();
		if (!supported && reason) detail.exclusion_reason = reason;
		details.push_back(detail);
	};
	if (features.class_id == Q3MutationClass::Irrelevant) {
		add(MaintenanceAction::NoOp, true, nullptr, {});
		add(MaintenanceAction::InvalidateAndFallback, true, nullptr, EstimateInvalidate(workload, params));
		return details;
	}
	const char *apply_reason = ApplyDeltaExclusionReason(features, low_confidence);
	if (features.rows_touched > params.apply_delta_max_rows) {
		add(MaintenanceAction::ApplyDelta, false, "rows_exceed_apply_delta_max", EstimateApplyDelta(features, workload, params));
	} else if (features.estimated_order_fanout > params.apply_delta_max_part_fanout) {
		add(MaintenanceAction::ApplyDelta, false, "order_fanout_exceeds_max", EstimateApplyDelta(features, workload, params));
	} else {
		add(MaintenanceAction::ApplyDelta, apply_reason == nullptr && !low_confidence,
		    low_confidence ? "low_confidence" : apply_reason, EstimateApplyDelta(features, workload, params));
	}
	const bool partition_ok = !features.whole_state_required && features.affected_partition_count > 0;
	ActionCostEstimate rebuild_part;
	rebuild_part.immediate_us = params.invalidation_overhead_us * std::max<uint64_t>(1, features.affected_partition_count);
	if (workload.expected_queries_before_next_write > 0.0) {
		rebuild_part.differential_future_query_us =
		    Q3PartitionRebuildUs() * std::max<uint64_t>(1, features.affected_partition_count) + Q3FirstQueryBlockingUs();
	}
	{
		ActionResourceEstimate resources;
		resources.estimated_duration_us =
		    Q3PartitionRebuildUs() * static_cast<double>(std::max<uint64_t>(1, features.affected_partition_count));
		resources.partitions_locked = std::max<uint64_t>(1, features.affected_partition_count);
		ApplyInterference(MaintenanceAction::RebuildPartition, rebuild_part, resources, params);
	}
	add(MaintenanceAction::RebuildPartition, partition_ok, partition_ok ? nullptr : "no_known_affected_partitions", rebuild_part);
	ActionCostEstimate rebuild_all;
	rebuild_all.immediate_us = params.invalidation_overhead_us;
	if (workload.expected_queries_before_next_write > 0.0) {
		rebuild_all.differential_future_query_us = Q3FullRebuildUs() + Q3FirstQueryBlockingUs();
	}
	{
		ActionResourceEstimate resources;
		resources.estimated_duration_us = Q3FullRebuildUs();
		resources.partitions_locked = 1;
		ApplyInterference(MaintenanceAction::RebuildAll, rebuild_all, resources, params);
	}
	add(MaintenanceAction::RebuildAll, features.whole_state_required, features.whole_state_required ? nullptr : "whole_state_not_required", rebuild_all);
	add(MaintenanceAction::InvalidateAndFallback, true, nullptr, EstimateInvalidate(workload, params));
	return details;
}

} // namespace

std::vector<Q3CandidateDecisionDetail>
EnumerateQ3ActionCandidatesDetailed(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                                    const CostModelParams &params, bool low_confidence) {
	return BuildCandidateDetails(features, workload, params, low_confidence);
}

std::vector<ActionCandidate> EnumerateQ3ActionCandidates(const Q3MutationBatchFeatures &features,
                                                         const Q3WorkloadFeatures &workload,
                                                         const CostModelParams &params, bool low_confidence) {
	std::vector<ActionCandidate> candidates;
	for (const auto &detail : BuildCandidateDetails(features, workload, params, low_confidence)) {
		candidates.push_back(ActionCandidate {detail.action, detail.estimate, detail.supported});
	}
	return candidates;
}

bool Q3ApplyDeltaPlausible(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                           const CostModelParams &params, bool low_confidence) {
	const char *reason = ApplyDeltaExclusionReason(features, low_confidence);
	if (reason != nullptr && std::strcmp(reason, "heavy_capture_deferred") != 0) return false;
	if (features.rows_touched > params.apply_delta_max_rows) return false;
	if (features.estimated_order_fanout > params.apply_delta_max_part_fanout) return false;
	return EstimateApplyDelta(features, workload, params).TotalOverheadUs() + params.decision_margin_us <
	       EstimateInvalidate(workload, params).TotalOverheadUs();
}

bool Q3ShouldRunHeavyCapture(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                             const CostModelParams &params, bool low_confidence) {
	(void)workload;
	(void)params;
	return !low_confidence && !features.bulk_mutation && !features.incremental_buffer_poisoned;
}

SelectionResult SelectQ3MaintenanceAction(const Q3MutationBatchFeatures &features, const Q3WorkloadFeatures &workload,
                                        MaintenanceAction last_action) {
	const auto &params = GetQ3CostModelParams();
	const bool low_confidence = !Q3HasSufficientTrainingSamples();
	SelectionInput input;
	input.candidates = EnumerateQ3ActionCandidates(features, workload, params, low_confidence);
	input.confidence = low_confidence ? ConfidenceTier::Low : ConfidenceTier::Trained;
	input.last_action = last_action;
	input.decision_margin_us = params.decision_margin_us;
	return SelectMinCostAction(input);
}

Q3ApplyDeltaEligibility BuildQ3ApplyDeltaEligibility(const Q3MutationBatchFeatures &features,
                                                     const CostModelParams &params, bool low_confidence,
                                                     uint64_t pending_light_ops, uint64_t row_ops,
                                                     uint64_t cross_partition_ops) {
	Q3ApplyDeltaEligibility eligibility;
	eligibility.low_confidence = low_confidence;
	eligibility.training_samples_sufficient = !low_confidence;
	eligibility.bulk_mutation = features.bulk_mutation;
	eligibility.whole_state_required = features.whole_state_required;
	eligibility.incremental_buffer_poisoned = features.incremental_buffer_poisoned;
	eligibility.incremental_buffer_complete = features.incremental_buffer_complete;
	eligibility.old_new_values_available = features.old_new_values_available;
	eligibility.heavy_capture_available = features.heavy_capture_available;
	eligibility.incremental_supported = features.incremental_supported;
	eligibility.rows_exceed_max = features.rows_touched > params.apply_delta_max_rows;
	eligibility.bounded_order_fanout = features.estimated_order_fanout;
	eligibility.estimated_lineitem_scan_rows = features.estimated_lineitem_scan_rows;
	eligibility.order_fanout_exceeds_max = features.estimated_order_fanout > params.apply_delta_max_part_fanout;
	eligibility.pending_light_ops = pending_light_ops;
	eligibility.row_ops = row_ops;
	eligibility.cross_partition_ops = cross_partition_ops;
	eligibility.primary_exclusion_reason = ApplyDeltaExclusionReason(features, low_confidence);
	if (eligibility.rows_exceed_max) eligibility.primary_exclusion_reason = "rows_exceed_apply_delta_max";
	else if (eligibility.order_fanout_exceeds_max) eligibility.primary_exclusion_reason = "order_fanout_exceeds_max";
	eligibility.eligible = eligibility.primary_exclusion_reason == nullptr && !low_confidence;
	eligibility.apply_delta_predicted_immediate_us = EstimateApplyDelta(features, Q3WorkloadFeatures {}, params).immediate_us;
	return eligibility;
}

} // namespace accelerator_codegen
