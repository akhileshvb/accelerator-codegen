#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"

#include <algorithm>

namespace accelerator_codegen::aux_state::core {

MaintenanceDecision SelectAuxMaintenanceAction(
    const MaintenanceRequest &request,
    const maintenance::CostModelParams &params) {
  using maintenance::ActionCandidate;
  using maintenance::ActionCostEstimate;
  using maintenance::MaintenanceAction;

  if (request.rows_touched == 0 && !request.whole_state_required) {
    return {MaintenanceAction::NoOp, 0.0};
  }

  const double expected_queries =
      std::max(1.0, request.expected_queries_before_next_write);
  const double future_query_premium =
      std::max(0.0, params.fallback_us - params.aux_hit_us);
  const auto rows = std::max<uint64_t>(1, request.rows_touched);
  const auto partitions =
      std::max<uint64_t>(1, request.affected_partition_count);

  std::vector<ActionCandidate> candidates;
  ActionCostEstimate apply_delta;
  apply_delta.immediate_us = params.apply_delta_overhead_us +
                             params.apply_delta_base_us +
                             params.apply_delta_per_row_us * rows;
  candidates.push_back({
      MaintenanceAction::ApplyDelta, apply_delta,
      request.incremental_supported && !request.whole_state_required &&
          request.rows_touched <= params.apply_delta_max_rows});

  ActionCostEstimate rebuild_partition;
  rebuild_partition.immediate_us =
      params.invalidation_overhead_us * partitions;
  rebuild_partition.differential_future_query_us =
      params.partition_rebuild_us * partitions;
  candidates.push_back({
      MaintenanceAction::RebuildPartition, rebuild_partition,
      request.partition_rebuild_supported &&
          !request.whole_state_required && request.affected_partition_count > 0});

  ActionCostEstimate rebuild_all;
  rebuild_all.immediate_us = params.invalidation_overhead_us;
  rebuild_all.differential_future_query_us = params.full_rebuild_us;
  candidates.push_back({MaintenanceAction::RebuildAll, rebuild_all,
                        request.full_rebuild_supported});

  ActionCostEstimate invalidate;
  invalidate.immediate_us = params.invalidation_overhead_us;
  invalidate.differential_future_query_us =
      expected_queries * future_query_premium;
  candidates.push_back({MaintenanceAction::InvalidateAndFallback, invalidate,
                        true});

  maintenance::SelectionInput input;
  input.candidates = std::move(candidates);
  input.confidence = request.low_confidence
                         ? maintenance::ConfidenceTier::Low
                         : maintenance::ConfidenceTier::Trained;
  input.last_action = request.last_action;
  input.decision_margin_us = params.decision_margin_us;
  const auto selection = maintenance::SelectMinCostAction(input);
  return {selection.action, selection.selected_cost_us};
}

} // namespace accelerator_codegen::aux_state::core
