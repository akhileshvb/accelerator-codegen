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

bool MaintenanceOutcomeHasPublishEffect(const StagedMaintenanceOutcome &outcome) {
	return !outcome.partition_updates.empty() || !outcome.invalidations.IsEmpty() ||
	       !outcome.pending_lazy_rebuild.IsEmpty();
}

} // namespace accelerator_codegen::aux_state::maintenance
