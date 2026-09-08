// SCAFFOLDING
#pragma once

#include <cstdint>
#include <optional>

#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"

namespace accelerator_codegen::aux_state::core {

struct MaintenanceDecisionLogEntry {
  maintenance::MaintenanceAction selected =
      maintenance::MaintenanceAction::NoOp;
  maintenance::MaintenanceAction staged_action =
      maintenance::MaintenanceAction::NoOp;
  double selected_predicted_cost_us = 0.0;
  double predicted_apply_delta_us = 0.0;
  double predicted_rebuild_partition_us = 0.0;
  double predicted_rebuild_all_us = 0.0;
  double predicted_invalidate_us = 0.0;
  double actual_maintenance_us = 0.0;
  uint64_t rows_touched = 0;
  uint64_t affected_partition_count = 0;
  double expected_queries_before_next_write = 0.0;
  bool valid = false;
};

void RecordMaintenanceSelectorDecision(const MaintenanceDecisionLogEntry &entry);
void RecordStagedMaintenanceAction(maintenance::MaintenanceAction action);
void RecordMaintenanceActualCostUs(double actual_us);
std::optional<MaintenanceDecisionLogEntry> LastMaintenanceDecision();
void ClearMaintenanceDecisionLog();

} // namespace accelerator_codegen::aux_state::core
