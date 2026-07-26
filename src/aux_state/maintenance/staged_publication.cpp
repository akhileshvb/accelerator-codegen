#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"

namespace accelerator_codegen::aux_state::maintenance {

void StagedMaintenanceOutcome::Clear() {
	selected = MaintenanceAction::NoOp;
	partition_updates.clear();
	invalidations = InvalidationPlan {};
	pending_lazy_rebuild = InvalidationPlan {};
	trace = MaintenanceDecisionTrace {};
	active = false;
}

} // namespace accelerator_codegen::aux_state::maintenance
