#include "accelerator_codegen/tpch_q1_maintenance.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "accelerator_codegen/aux_state/maintenance/action_selector.hpp"

namespace accelerator_codegen {
namespace {

using namespace aux_state::maintenance;

bool SupportsApplyDelta(const Q1MutationBatchFeatures &features) {
	return features.incremental_supported && features.incremental_buffer_complete && !features.incremental_buffer_poisoned &&
	       features.old_new_values_available && !features.whole_state_required && !features.bulk_mutation &&
	       features.heavy_capture_available;
}

const char *ApplyDeltaExclusionReason(const Q1MutationBatchFeatures &features, bool low_confidence) {
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

double ExpectedQueryPremium(const Q1WorkloadFeatures &workload) {
	return std::max(0.0, workload.ewma_fallback_latency_us - workload.ewma_aux_hit_latency_us);
}

double ExpectedQueriesForCosting(const Q1WorkloadFeatures &workload) {
	return std::max(1.0, workload.expected_queries_before_next_write);
}

ActionCostEstimate EstimateNoOp() {
	return {};
}

ActionCostEstimate EstimateApplyDelta(const Q1MutationBatchFeatures &features, const Q1WorkloadFeatures &workload,
                                      const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.apply_delta_base_us + Q1ApplyDeltaOverheadUs();
	const auto rows = std::max<uint64_t>(1, features.rows_touched);
	estimate.immediate_us += params.apply_delta_per_row_us * static_cast<double>(rows);
	const auto failure_prob = 1.0 - features.recent_incremental_success_rate;
	estimate.immediate_us +=
	    failure_prob * (params.invalidation_overhead_us +
	                    ExpectedQueriesForCosting(workload) * ExpectedQueryPremium(workload));
	estimate.differential_future_query_us = 0.0;
	(void)features;
	return estimate;
}

bool PartitionRebuildEligible(const Q1MutationBatchFeatures &features) {
	return !features.whole_state_required && features.affected_partition_count > 0;
}

bool RebuildAllEligible(const Q1MutationBatchFeatures &features) {
	return features.whole_state_required || features.class_id == Q1MutationClass::BulkLineitem;
}

ActionCostEstimate EstimateLazyRebuildAll(const Q1WorkloadFeatures &workload, const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.invalidation_overhead_us;
	if (workload.expected_queries_before_next_write > 0.0) {
		estimate.differential_future_query_us = Q1FullRebuildUs() + Q1FirstQueryBlockingUs();
	}
	(void)params;
	return estimate;
}

ActionCostEstimate EstimateLazyRebuildPartition(const Q1MutationBatchFeatures &features,
                                                const Q1WorkloadFeatures &workload, const CostModelParams &params) {
	ActionCostEstimate estimate;
	const auto partition_count = std::max<uint64_t>(1, features.affected_partition_count);
	estimate.immediate_us = params.invalidation_overhead_us * static_cast<double>(partition_count);
	if (workload.expected_queries_before_next_write > 0.0) {
		const auto rebuild_scan = Q1PartitionRebuildUs() * static_cast<double>(partition_count);
		estimate.differential_future_query_us = rebuild_scan + Q1FirstQueryBlockingUs();
	}
	(void)features;
	return estimate;
}

ActionCostEstimate EstimateInvalidate(const Q1WorkloadFeatures &workload, const CostModelParams &params) {
	ActionCostEstimate estimate;
	estimate.immediate_us = params.invalidation_overhead_us;
	estimate.differential_future_query_us =
	    ExpectedQueriesForCosting(workload) * ExpectedQueryPremium(workload);
	(void)params;
	return estimate;
}

std::vector<Q1CandidateDecisionDetail> BuildCandidateDetails(const Q1MutationBatchFeatures &features,
                                                              const Q1WorkloadFeatures &workload,
                                                              const CostModelParams &params, bool low_confidence) {
	std::vector<Q1CandidateDecisionDetail> details;
	auto add = [&](MaintenanceAction action, bool supported, const char *unsupported_reason,
	               const ActionCostEstimate &estimate) {
		Q1CandidateDecisionDetail detail;
		detail.action = action;
		detail.supported = supported;
		detail.estimate = estimate;
		detail.total_overhead_us = estimate.TotalOverheadUs();
		if (!supported && unsupported_reason) {
			detail.exclusion_reason = unsupported_reason;
		}
		details.push_back(detail);
	};

	if (features.class_id == Q1MutationClass::Irrelevant) {
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

std::vector<Q1CandidateDecisionDetail>
EnumerateQ1ActionCandidatesDetailed(const Q1MutationBatchFeatures &features, const Q1WorkloadFeatures &workload,
                                     const CostModelParams &params, bool low_confidence) {
	return BuildCandidateDetails(features, workload, params, low_confidence);
}

std::vector<ActionCandidate> EnumerateQ1ActionCandidates(const Q1MutationBatchFeatures &features,
                                                          const Q1WorkloadFeatures &workload,
                                                          const CostModelParams &params, bool low_confidence) {
	std::vector<ActionCandidate> candidates;
	for (const auto &detail : BuildCandidateDetails(features, workload, params, low_confidence)) {
		candidates.push_back(ActionCandidate {detail.action, detail.estimate, detail.supported});
	}
	return candidates;
}

bool Q1ApplyDeltaPlausible(const Q1MutationBatchFeatures &features, const Q1WorkloadFeatures &workload,
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

SelectionResult SelectQ1MaintenanceAction(const Q1MutationBatchFeatures &features, const Q1WorkloadFeatures &workload,
                                         MaintenanceAction last_action) {
	const auto &params = GetQ1CostModelParams();
	const bool low_confidence = !Q1HasSufficientTrainingSamples();
	auto candidates = EnumerateQ1ActionCandidates(features, workload, params, low_confidence);

	SelectionInput input;
	input.candidates = std::move(candidates);
	input.confidence = low_confidence ? ConfidenceTier::Low : ConfidenceTier::Trained;
	input.last_action = last_action;
	input.decision_margin_us = params.decision_margin_us;
	return SelectMinCostAction(input);
}

Q1ApplyDeltaEligibility BuildQ1ApplyDeltaEligibility(const Q1MutationBatchFeatures &features,
                                                     const CostModelParams &params, bool low_confidence,
                                                     uint64_t pending_light_ops, uint64_t row_ops) {
	Q1ApplyDeltaEligibility eligibility;
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
	    EstimateApplyDelta(features, Q1WorkloadFeatures {}, params).immediate_us;
	return eligibility;
}

} // namespace accelerator_codegen
