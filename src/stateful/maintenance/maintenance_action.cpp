// SCAFFOLDING
#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"

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

const char *RepairTimingName(RepairTiming timing) {
	switch (timing) {
	case RepairTiming::LazyOnQuery:
		return "LazyOnQuery";
	case RepairTiming::PostCommitImmediate:
		return "PostCommitImmediate";
	}
	return "Unknown";
}

} // namespace accelerator_codegen::aux_state::maintenance
