// SCAFFOLDING
// The last maintenance decision (what was chosen, predicted and measured), for reports and calibration.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "substrate/aux/maintenance/maintenance_action.hpp"

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
  // The coordinator's own maintenance time for the commit (staging, any
  // synchronous rebuild, publish): no capture, no engine commit.
  double maintenance_us = 0.0;
  uint64_t rows_touched = 0;
  uint64_t affected_partition_count = 0;
  uint64_t data_rows_hint = 0; // rows in the dependency tables at the decision; 0: unknown
  double expected_queries_before_next_write = 0.0;
  bool valid = false;
};

// While alive on a thread, that thread's Record*/Last/Clear calls use a private
// slot instead of the process-wide one (parallel commits: each runtime's
// decision stays its own). Not nestable across threads; same thread only.
class ScopedThreadMaintenanceDecisionLog {
public:
  ScopedThreadMaintenanceDecisionLog();
  ~ScopedThreadMaintenanceDecisionLog();
  ScopedThreadMaintenanceDecisionLog(const ScopedThreadMaintenanceDecisionLog &) = delete;
  ScopedThreadMaintenanceDecisionLog &operator=(const ScopedThreadMaintenanceDecisionLog &) = delete;

private:
  MaintenanceDecisionLogEntry slot_;
  MaintenanceDecisionLogEntry *previous_;
};

// Overwrites the process-wide slot (what the last single-threaded decision would have left).
void SetGlobalMaintenanceDecision(const MaintenanceDecisionLogEntry &entry);

void RecordMaintenanceSelectorDecision(const MaintenanceDecisionLogEntry &entry);
void RecordStagedMaintenanceAction(maintenance::MaintenanceAction action);
void RecordMaintenanceActualCostUs(double actual_us);
void RecordCoordinatorMaintenanceUs(double us);
std::optional<MaintenanceDecisionLogEntry> LastMaintenanceDecision();
void ClearMaintenanceDecisionLog();

// "action=A rows=N partitions=M maintenance_us=T data_rows=D" for the last decision (engines expose it
// over SQL for calibration), and its parse ({} if malformed).
std::string FormatLastMaintenanceDecision();
std::optional<MaintenanceDecisionLogEntry> ParseMaintenanceDecision(const std::string &text);

} // namespace accelerator_codegen::aux_state::core
