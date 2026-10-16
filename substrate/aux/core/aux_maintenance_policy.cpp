// SCAFFOLDING
#include "substrate/aux/core/aux_maintenance_policy.hpp"

#include "substrate/aux/core/aux_maintenance_decision_log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace accelerator_codegen::aux_state::core {

namespace {

std::mutex &ForceMutex() {
  static std::mutex mutex;
  return mutex;
}

std::optional<maintenance::MaintenanceAction> &ForceSlot() {
  static std::optional<maintenance::MaintenanceAction> slot;
  return slot;
}

std::mutex &ExpectedQueriesMutex() {
  static std::mutex mutex;
  return mutex;
}

double &ExpectedQueriesSlot() {
  static double slot = -1.0;
  return slot;
}

std::optional<maintenance::MaintenanceAction>
ParseMaintenanceActionName(std::string_view name) {
  using maintenance::MaintenanceAction;
  if (name.empty()) {
    return std::nullopt;
  }
  if (name == "ApplyDelta") {
    return MaintenanceAction::ApplyDelta;
  }
  if (name == "RebuildAll") {
    return MaintenanceAction::RebuildAll;
  }
  if (name == "RebuildPartition") {
    return MaintenanceAction::RebuildPartition;
  }
  if (name == "InvalidateAndRebuildOnRead" ||
      name == "InvalidateAndFallback") { // the old name

    return MaintenanceAction::InvalidateAndRebuildOnRead;
  }
  if (name == "NoOp") {
    return MaintenanceAction::NoOp;
  }
  return std::nullopt;
}

bool TryForcedMaintenanceAction(MaintenanceDecision &out) {
  if (auto forced = ForcedMaintenanceAction()) {
    out = {*forced, 0.0};
    return true;
  }
  return false;
}

} // namespace

void SetForcedMaintenanceActionName(std::string_view action_name) {
  std::lock_guard<std::mutex> guard(ForceMutex());
  ForceSlot() = ParseMaintenanceActionName(action_name);
}

std::optional<maintenance::MaintenanceAction> ForcedMaintenanceAction() {
  {
    std::lock_guard<std::mutex> guard(ForceMutex());
    if (ForceSlot().has_value()) {
      return ForceSlot();
    }
  }
  const char *force = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  if (!force || !*force) {
    return std::nullopt;
  }
  return ParseMaintenanceActionName(force);
}

void SetExpectedQueriesOverride(double expected_queries) {
  std::lock_guard<std::mutex> guard(ExpectedQueriesMutex());
  ExpectedQueriesSlot() = expected_queries;
}

double ExpectedQueriesOverride() {
  {
    std::lock_guard<std::mutex> guard(ExpectedQueriesMutex());
    if (ExpectedQueriesSlot() >= 0.0) {
      return ExpectedQueriesSlot();
    }
  }
  const char *env = std::getenv("ACCEL_EXPECTED_QUERIES_BEFORE_WRITE");
  if (env && *env) {
    return std::max(0.0, std::atof(env));
  }
  return -1.0;
}

MaintenanceDecision SelectAuxMaintenanceAction(
    const MaintenanceRequest &request,
    const maintenance::CostModelParams &params) {
  using maintenance::ActionCandidate;
  using maintenance::ActionCostEstimate;
  using maintenance::MaintenanceAction;

  MaintenanceDecision forced;
  if (TryForcedMaintenanceAction(forced)) {
    MaintenanceDecisionLogEntry log_entry;
    log_entry.selected = forced.action;
    log_entry.staged_action = forced.action;
    log_entry.rows_touched = request.rows_touched;
    log_entry.affected_partition_count = request.affected_partition_count;
    log_entry.data_rows_hint = request.data_rows_hint;
    log_entry.expected_queries_before_next_write = request.expected_queries_before_next_write;
    RecordMaintenanceSelectorDecision(log_entry);
    return forced;
  }

  if (request.rows_touched == 0 && !request.whole_state_required) {
    return {MaintenanceAction::NoOp, 0.0};
  }

  // Reads expected before the next write (may be below 1: write-heavy).
  const double expected_queries =
      std::max(0.0, request.expected_queries_before_next_write);
  const auto rows = std::max<uint64_t>(1, request.rows_touched);
  const auto partitions =
      std::max<uint64_t>(1, request.affected_partition_count);

  // Every accelerator offers the same actions. Each is priced by what it
  // costs this accelerator; a module without finer partitions rebuilds its
  // whole state for RebuildPartition.
  // Rebuilds and the ApplyDelta cap grow with the data relative to the scale
  // the model was calibrated at (ApplyDelta's per-row cost does not). Unknown
  // scale: the model as calibrated.
  const double scale =
      request.data_rows_hint != 0 && params.calibrated_scale_rows != 0
          ? static_cast<double>(request.data_rows_hint) /
                static_cast<double>(params.calibrated_scale_rows)
          : 1.0;
  std::vector<ActionCandidate> candidates;
  const bool apply_delta_eligible =
      request.incremental_supported && !request.whole_state_required &&
      static_cast<double>(request.rows_touched) <=
          static_cast<double>(params.apply_delta_max_rows) * scale;

  ActionCostEstimate apply_delta;
  apply_delta.immediate_us = params.apply_delta_overhead_us +
                             params.apply_delta_base_us +
                             params.apply_delta_per_row_us * rows;
  candidates.push_back(
      {MaintenanceAction::ApplyDelta, apply_delta, apply_delta_eligible});

  ActionCostEstimate rebuild_all;
  rebuild_all.immediate_us =
      params.invalidation_overhead_us + params.full_rebuild_us * scale;
  candidates.push_back({MaintenanceAction::RebuildAll, rebuild_all,
                        request.full_rebuild_supported});

  // Rebuild actions run Build+Publish at commit: their cost is immediate.
  // Without finer partitions RebuildPartition is a full rebuild, priced just
  // above RebuildAll so a tie goes to RebuildAll (it is chosen only where
  // RebuildAll is not available).
  const bool finer_partitions = request.partition_rebuild_supported;
  ActionCostEstimate rebuild_partition;
  rebuild_partition.immediate_us =
      finer_partitions
          ? params.invalidation_overhead_us +
                scale * (params.partition_rebuild_base_us +
                         params.partition_rebuild_us * partitions)
          : rebuild_all.immediate_us + params.decision_margin_us + 1.0;
  candidates.push_back({MaintenanceAction::RebuildPartition, rebuild_partition,
                        !request.whole_state_required});

  // InvalidateAndRebuildOnRead: invalidate now; the next reader rebuilds and
  // answers from the state (no SQL fallback). One full rebuild, paid only if
  // a read comes before the next write: it saves work only when writes
  // outnumber reads (at one read per write or more it ties RebuildAll, which
  // wins the tie).
  ActionCostEstimate deferred;
  deferred.immediate_us = params.invalidation_overhead_us;
  deferred.differential_future_query_us =
      std::min(1.0, expected_queries) * params.full_rebuild_us * scale;
  candidates.push_back({MaintenanceAction::InvalidateAndRebuildOnRead, deferred,
                        request.deferred_rebuild_supported &&
                            request.full_rebuild_supported});

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
  log_entry.data_rows_hint = request.data_rows_hint;
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
    case MaintenanceAction::InvalidateAndRebuildOnRead:
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
