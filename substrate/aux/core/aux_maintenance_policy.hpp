// SCAFFOLDING
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "substrate/aux/maintenance/action_selector.hpp"

namespace accelerator_codegen::aux_state::core {

struct MaintenanceRequest {
  uint64_t rows_touched = 0;
  uint64_t affected_partition_count = 0;
  bool incremental_supported = false;
  bool partition_rebuild_supported = false;
  bool full_rebuild_supported = true;
  bool whole_state_required = false;
  bool low_confidence = false;
  maintenance::MaintenanceAction last_action = maintenance::MaintenanceAction::NoOp;
  double expected_queries_before_next_write = 10.0;
  // (Last: modules initialize the fields above positionally.)
  // InvalidateAndRebuildOnRead: the next reader rebuilds (set from
  // AuxEngineServices::ReadersCanBuildState by ChooseMaintenance).
  bool deferred_rebuild_supported = true;
};

struct MaintenanceDecision {
  maintenance::MaintenanceAction action = maintenance::MaintenanceAction::NoOp;
  double selected_cost_us = 0.0;
};

// Process-local override for ChooseMaintenance (Postgres SQL can set this).
// Empty / unknown name clears the override. Also honors getenv when unset.
void SetForcedMaintenanceActionName(std::string_view action_name);
std::optional<maintenance::MaintenanceAction> ForcedMaintenanceAction();

// Process-local override for expected queries before the next write.
// Negative expected_queries clears the override. Also honors getenv when unset.
void SetExpectedQueriesOverride(double expected_queries);
double ExpectedQueriesOverride();

MaintenanceDecision SelectAuxMaintenanceAction(
    const MaintenanceRequest &, const maintenance::CostModelParams &);

} // namespace accelerator_codegen::aux_state::core
