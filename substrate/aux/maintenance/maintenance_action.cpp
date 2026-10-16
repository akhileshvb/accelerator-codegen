// SCAFFOLDING
// Names of the maintenance actions.
#include "substrate/aux/maintenance/maintenance_action.hpp"

namespace accelerator_codegen::aux_state::maintenance {

const char *MaintenanceActionName(MaintenanceAction action) {
  switch (action) {
  case MaintenanceAction::NoOp:
    return "NoOp";
  case MaintenanceAction::ApplyDelta:
    return "ApplyDelta";
  case MaintenanceAction::RebuildPartition:
    return "RebuildPartition";
  case MaintenanceAction::RebuildAll:
    return "RebuildAll";
  case MaintenanceAction::InvalidateAndRebuildOnRead:
    return "InvalidateAndRebuildOnRead";
  }
  return "Unknown";
}

} // namespace accelerator_codegen::aux_state::maintenance
