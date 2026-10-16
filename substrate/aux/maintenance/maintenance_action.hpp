// SCAFFOLDING
// The maintenance actions a commit can take on an accelerator's state.
#pragma once

#include <cstdint>

namespace accelerator_codegen::aux_state::maintenance {

enum class MaintenanceAction : uint8_t {
  NoOp = 0,
  ApplyDelta,
  RebuildPartition,
  RebuildAll,
  InvalidateAndRebuildOnRead,
};

const char *MaintenanceActionName(MaintenanceAction action);

} // namespace accelerator_codegen::aux_state::maintenance
