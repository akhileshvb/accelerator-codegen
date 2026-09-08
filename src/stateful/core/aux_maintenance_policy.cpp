// SCAFFOLDING
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"

#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace accelerator_codegen::aux_state::core {

namespace {

bool TryForcedMaintenanceAction(MaintenanceDecision &out) {
  const char *force = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  if (!force || !*force) {
    return false;
  }
  using maintenance::MaintenanceAction;
  if (std::strcmp(force, "ApplyDelta") == 0) {
    out = {MaintenanceAction::ApplyDelta, 0.0};
    return true;
  }
  if (std::strcmp(force, "RebuildAll") == 0) {
    out = {MaintenanceAction::RebuildAll, 0.0};
    return true;
  }
  if (std::strcmp(force, "RebuildPartition") == 0) {
    out = {MaintenanceAction::RebuildPartition, 0.0};
    return true;
  }
  if (std::strcmp(force, "InvalidateAndFallback") == 0) {
    out = {MaintenanceAction::InvalidateAndFallback, 0.0};
    return true;
  }
  if (std::strcmp(force, "NoOp") == 0) {
    out = {MaintenanceAction::NoOp, 0.0};
    return true;
  }
  return false;
}

} // namespace

MaintenanceDecision SelectAuxMaintenanceAction(
    const MaintenanceRequest &request,
    const maintenance::CostModelParams &params) {
  using maintenance::ActionCandidate;
  using maintenance::ActionCostEstimate;
  using maintenance::MaintenanceAction;

  MaintenanceDecision forced;
  if (TryForcedMaintenanceAction(forced)) {
    return forced;
  }

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
  const bool apply_delta_eligible =
      request.incremental_supported && !request.whole_state_required &&
      request.rows_touched <= params.apply_delta_max_rows;

  ActionCostEstimate apply_delta;
  apply_delta.immediate_us = params.apply_delta_overhead_us +
                             params.apply_delta_base_us +
                             params.apply_delta_per_row_us * rows;
  candidates.push_back(
      {MaintenanceAction::ApplyDelta, apply_delta, apply_delta_eligible});

  // Rebuild actions now run Build+Publish on Commit (sync). Charge rebuild cost
  // as immediate so ApplyDelta still wins when cheaper, and so InvalidateAndFallback
  // remains preferred only when few future queries are expected.
  ActionCostEstimate rebuild_partition;
  rebuild_partition.immediate_us =
      params.invalidation_overhead_us * partitions +
      params.partition_rebuild_us * partitions;
  candidates.push_back({
      MaintenanceAction::RebuildPartition, rebuild_partition,
      request.partition_rebuild_supported &&
          !request.whole_state_required && request.affected_partition_count > 0});

  ActionCostEstimate rebuild_all;
  rebuild_all.immediate_us =
      params.invalidation_overhead_us + params.full_rebuild_us;
  candidates.push_back({MaintenanceAction::RebuildAll, rebuild_all,
                        request.full_rebuild_supported});

  ActionCostEstimate invalidate;
  invalidate.immediate_us = params.invalidation_overhead_us;
  invalidate.differential_future_query_us =
      expected_queries * future_query_premium;
  // When ApplyDelta is eligible, do not offer lazy invalidation: large RF
  // batches with low expected-query counts otherwise prefer Invalidate even
  // though measured incremental maintenance is cheaper than rebuild.
  candidates.push_back({MaintenanceAction::InvalidateAndFallback, invalidate,
                        !apply_delta_eligible});

  maintenance::SelectionInput input;
  input.candidates = std::move(candidates);
  input.confidence = request.low_confidence
                         ? maintenance::ConfidenceTier::Low
                         : maintenance::ConfidenceTier::Trained;
  input.last_action = request.last_action;
  input.decision_margin_us = params.decision_margin_us;
  const auto selection = maintenance::SelectMinCostAction(input);

  MaintenanceDecisionLogEntry log_entry;
  log_entry.selected = selection.action;
  log_entry.staged_action = selection.action;
  log_entry.selected_predicted_cost_us = selection.selected_cost_us;
  log_entry.rows_touched = request.rows_touched;
  log_entry.affected_partition_count = request.affected_partition_count;
  log_entry.expected_queries_before_next_write =
      request.expected_queries_before_next_write;
  for (const auto &candidate : input.candidates) {
    const double total = candidate.estimate.TotalOverheadUs();
    switch (candidate.action) {
    case MaintenanceAction::ApplyDelta:
      log_entry.predicted_apply_delta_us = total;
      break;
    case MaintenanceAction::RebuildPartition:
      log_entry.predicted_rebuild_partition_us = total;
      break;
    case MaintenanceAction::RebuildAll:
      log_entry.predicted_rebuild_all_us = total;
      break;
    case MaintenanceAction::InvalidateAndFallback:
      log_entry.predicted_invalidate_us = total;
      break;
    case MaintenanceAction::NoOp:
      break;
    }
  }
  RecordMaintenanceSelectorDecision(log_entry);

  return {selection.action, selection.selected_cost_us};
}

} // namespace accelerator_codegen::aux_state::core
