#include "accelerator_codegen/sec_edgar_q1_maintenance.hpp"

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

bool SupportsApplyDelta(const SecEdgarQ1MutationBatchFeatures &features) {
	return features.incremental_supported && features.incremental_buffer_complete && !features.incremental_buffer_poisoned &&
	       features.old_new_values_available && !features.whole_state_required && !features.bulk_mutation &&
	       features.heavy_capture_available;
}

const char *ApplyDeltaExclusionReason(const SecEdgarQ1MutationBatchFeatures &features, bool low_confidence) {
	if (low_confidence) {
		return "low_confidence";
	}
	if (features.bulk_mutation) {
		return "bulk_mutation";
	}
	if (features.whole_state_required) {
		return "whole_state_required";
	}
	if (features.incremental_buffer_poisoned) {
		return "incremental_buffer_poisoned";
	}
	if (!features.incremental_buffer_complete) {
		return "incremental_buffer_incomplete";
	}
	if (!features.old_new_values_available) {
		return "old_new_values_unavailable";
	}
	if (!features.heavy_capture_available) {
		return "heavy_capture_deferred";
	}
	if (!features.incremental_supported) {
		return "incremental_unsupported";
	}
	return nullptr;
}

double ExpectedQueryPremium(const SecEdgarQ1WorkloadFeatures &workload) {
	return std::max(0.0, workload.ewma_fallback_latency_us - workload.ewma_aux_hit_latency_us);
}

double ExpectedQueriesForCosting(const SecEdgarQ1WorkloadFeatures &workload) {
	return std::max(1.0, workload.expected_queries_before_next_write);
}

void ApplyInterference(MaintenanceAction action, ActionCostEstimate &estimate, const ActionResourceEstimate &resources,
                       const CostModelParams &params) {
	const auto load = aux_state::GetExecutionLoadRegistry().Snapshot();
	estimate.interference_us = EstimateInterferenceUs(action, resources, load, params);
}

ActionCostEstimate EstimateNoOp() {
	return {};
}

ActionCostEstimate EstimateApplyDelta(const SecEdgarQ1MutationBatchFeatures &features, const SecEdgarQ1WorkloadFeatures &workload,
                                      const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.apply_delta_base_us + SecEdgarQ1ApplyDeltaOverheadUs();
	const auto rows = std::max<uint64_t>(1, features.rows_touched);
	estimate.immediate_us += params.apply_delta_per_row_us * static_cast<double>(rows);
	const auto failure_prob = 1.0 - features.recent_incremental_success_rate;
	estimate.immediate_us +=
	    failure_prob * (params.invalidation_overhead_us +
	                    ExpectedQueriesForCosting(workload) * ExpectedQueryPremium(workload));
	estimate.differential_future_query_us = 0.0;
	ActionResourceEstimate resources;
	resources.estimated_duration_us = estimate.immediate_us;
	ApplyInterference(MaintenanceAction::ApplyDelta, estimate, resources, params);
	return estimate;
}

bool PartitionRebuildEligible(const SecEdgarQ1MutationBatchFeatures &features) {
	return !features.whole_state_required && features.affected_partition_count > 0;
}

bool RebuildAllEligible(const SecEdgarQ1MutationBatchFeatures &features) {
	return features.whole_state_required || features.class_id == SecEdgarQ1MutationClass::BulkLineitem;
}

ActionCostEstimate EstimateLazyRebuildAll(const SecEdgarQ1WorkloadFeatures &workload, const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.invalidation_overhead_us;
	if (workload.expected_queries_before_next_write > 0.0) {
		estimate.differential_future_query_us = SecEdgarQ1FullRebuildUs() + SecEdgarQ1FirstQueryBlockingUs();
	}
	ActionResourceEstimate resources;
	resources.estimated_duration_us = SecEdgarQ1FullRebuildUs();
	resources.partitions_locked = 1;
	ApplyInterference(MaintenanceAction::RebuildAll, estimate, resources, params);
	(void)params;
	return estimate;
}

ActionCostEstimate EstimateLazyRebuildPartition(const SecEdgarQ1MutationBatchFeatures &features,
                                                const SecEdgarQ1WorkloadFeatures &workload, const CostModelParams &params) {
	ActionCostEstimate estimate;
	const auto partition_count = std::max<uint64_t>(1, features.affected_partition_count);
	estimate.immediate_us = params.invalidation_overhead_us * static_cast<double>(partition_count);
	if (workload.expected_queries_before_next_write > 0.0) {
		const auto rebuild_scan = SecEdgarQ1PartitionRebuildUs() * static_cast<double>(partition_count);
		estimate.differential_future_query_us = rebuild_scan + SecEdgarQ1FirstQueryBlockingUs();
	}
	ActionResourceEstimate resources;
	resources.estimated_duration_us = SecEdgarQ1PartitionRebuildUs() * static_cast<double>(partition_count);
	resources.partitions_locked = partition_count;
	ApplyInterference(MaintenanceAction::RebuildPartition, estimate, resources, params);
	(void)features;
	return estimate;
}

ActionCostEstimate EstimateInvalidate(const SecEdgarQ1WorkloadFeatures &workload, const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.invalidation_overhead_us;
	estimate.differential_future_query_us =
	    ExpectedQueriesForCosting(workload) * ExpectedQueryPremium(workload);
	ActionResourceEstimate resources;
	ApplyInterference(MaintenanceAction::InvalidateAndFallback, estimate, resources, params);
	(void)params;
	return estimate;
}

std::vector<SecEdgarQ1CandidateDecisionDetail> BuildCandidateDetails(const SecEdgarQ1MutationBatchFeatures &features,
                                                              const SecEdgarQ1WorkloadFeatures &workload,
                                                              const CostModelParams &params, bool low_confidence) {
	std::vector<SecEdgarQ1CandidateDecisionDetail> details;
	auto add = [&](MaintenanceAction action, bool supported, const char *unsupported_reason,
	               const ActionCostEstimate &estimate) {
		SecEdgarQ1CandidateDecisionDetail detail;
		detail.action = action;
		detail.supported = supported;
		detail.estimate = estimate;
		detail.total_overhead_us = estimate.TotalOverheadUs();
		if (!supported && unsupported_reason) {
			detail.exclusion_reason = unsupported_reason;
		}
		details.push_back(detail);
	};

	if (features.class_id == SecEdgarQ1MutationClass::Irrelevant) {
		add(MaintenanceAction::NoOp, true, nullptr, EstimateNoOp());
		add(MaintenanceAction::InvalidateAndFallback, true, nullptr, EstimateInvalidate(workload, params));
		return details;
	}

	const char *apply_delta_reason = ApplyDeltaExclusionReason(features, low_confidence);
	const bool apply_delta_ok = apply_delta_reason == nullptr;
	if (features.rows_touched > params.apply_delta_max_rows) {
		add(MaintenanceAction::ApplyDelta, false, "rows_exceed_apply_delta_max", EstimateApplyDelta(features, workload, params));
	} else {
		add(MaintenanceAction::ApplyDelta, apply_delta_ok && !low_confidence,
		    low_confidence ? "low_confidence" : apply_delta_reason, EstimateApplyDelta(features, workload, params));
	}

	const bool partition_rebuild_ok = PartitionRebuildEligible(features);
	add(MaintenanceAction::RebuildPartition, partition_rebuild_ok,
	    partition_rebuild_ok ? nullptr : "no_known_affected_partitions",
	    EstimateLazyRebuildPartition(features, workload, params));

	const bool rebuild_all_ok = RebuildAllEligible(features);
	add(MaintenanceAction::RebuildAll, rebuild_all_ok, rebuild_all_ok ? nullptr : "whole_state_not_required",
	    EstimateLazyRebuildAll(workload, params));

	add(MaintenanceAction::InvalidateAndFallback, true, nullptr, EstimateInvalidate(workload, params));
	return details;
}

} // namespace

std::vector<SecEdgarQ1CandidateDecisionDetail>
EnumerateSecEdgarQ1ActionCandidatesDetailed(const SecEdgarQ1MutationBatchFeatures &features, const SecEdgarQ1WorkloadFeatures &workload,
                                     const CostModelParams &params, bool low_confidence) {
	return BuildCandidateDetails(features, workload, params, low_confidence);
}

std::vector<ActionCandidate> EnumerateSecEdgarQ1ActionCandidates(const SecEdgarQ1MutationBatchFeatures &features,
                                                          const SecEdgarQ1WorkloadFeatures &workload,
                                                          const CostModelParams &params, bool low_confidence) {
	std::vector<ActionCandidate> candidates;
	for (const auto &detail : BuildCandidateDetails(features, workload, params, low_confidence)) {
		candidates.push_back(ActionCandidate {detail.action, detail.estimate, detail.supported});
	}
	return candidates;
}

bool SecEdgarQ1ApplyDeltaPlausible(const SecEdgarQ1MutationBatchFeatures &features, const SecEdgarQ1WorkloadFeatures &workload,
                            const CostModelParams &params, bool low_confidence) {
	const char *reason = ApplyDeltaExclusionReason(features, low_confidence);
	if (reason != nullptr && std::strcmp(reason, "heavy_capture_deferred") != 0) {
		return false;
	}
	if (features.rows_touched > params.apply_delta_max_rows) {
		return false;
	}
	const auto apply_cost = EstimateApplyDelta(features, workload, params).TotalOverheadUs();
	const auto invalidate_cost = EstimateInvalidate(workload, params).TotalOverheadUs();
	const auto rebuild_cost =
	    PartitionRebuildEligible(features)
	        ? EstimateLazyRebuildPartition(features, workload, params).TotalOverheadUs()
	        : std::numeric_limits<double>::infinity();
	return apply_cost + params.decision_margin_us < invalidate_cost &&
	       apply_cost + params.decision_margin_us < rebuild_cost;
}

SelectionResult SelectSecEdgarQ1MaintenanceAction(const SecEdgarQ1MutationBatchFeatures &features, const SecEdgarQ1WorkloadFeatures &workload,
                                         MaintenanceAction last_action) {
	const auto &params = GetSecEdgarQ1CostModelParams();
	const bool low_confidence = !SecEdgarQ1HasSufficientTrainingSamples();
	auto candidates = EnumerateSecEdgarQ1ActionCandidates(features, workload, params, low_confidence);

	SelectionInput input;
	input.candidates = std::move(candidates);
	input.confidence = low_confidence ? ConfidenceTier::Low : ConfidenceTier::Trained;
	input.last_action = last_action;
	input.decision_margin_us = params.decision_margin_us;
	return SelectMinCostAction(input);
}

SecEdgarQ1ApplyDeltaEligibility BuildSecEdgarQ1ApplyDeltaEligibility(const SecEdgarQ1MutationBatchFeatures &features,
                                                     const CostModelParams &params, bool low_confidence,
                                                     uint64_t pending_light_ops, uint64_t row_ops) {
	SecEdgarQ1ApplyDeltaEligibility eligibility;
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
	eligibility.pending_light_ops = pending_light_ops;
	eligibility.row_ops = row_ops;
	eligibility.primary_exclusion_reason = ApplyDeltaExclusionReason(features, low_confidence);
	if (eligibility.rows_exceed_max) {
		eligibility.primary_exclusion_reason = "rows_exceed_apply_delta_max";
	}
	eligibility.eligible = eligibility.primary_exclusion_reason == nullptr && !low_confidence;
	eligibility.apply_delta_predicted_immediate_us =
	    EstimateApplyDelta(features, SecEdgarQ1WorkloadFeatures {}, params).immediate_us;
	return eligibility;
}

} // namespace accelerator_codegen
