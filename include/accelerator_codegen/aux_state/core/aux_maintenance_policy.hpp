#pragma once

#include <cstdint>

#include "accelerator_codegen/aux_state/maintenance/action_selector.hpp"

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
};

struct MaintenanceDecision {
  maintenance::MaintenanceAction action = maintenance::MaintenanceAction::NoOp;
  double selected_cost_us = 0.0;
};

MaintenanceDecision SelectAuxMaintenanceAction(
    const MaintenanceRequest &, const maintenance::CostModelParams &);

} // namespace accelerator_codegen::aux_state::core
