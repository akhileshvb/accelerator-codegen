// SCAFFOLDING
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

enum class RepairTiming : uint8_t {
	LazyOnQuery = 0,
	// Reserved for phase 2 when QueryEnd deferral is verified.
	PostCommitImmediate,
};

const char *MaintenanceActionName(MaintenanceAction action);
const char *RepairTimingName(RepairTiming timing);

} // namespace accelerator_codegen::aux_state::maintenance
