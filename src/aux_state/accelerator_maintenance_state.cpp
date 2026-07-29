#include "accelerator_codegen/aux_state/accelerator_maintenance_state.hpp"

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"

namespace accelerator_codegen::aux_state {

uint64_t AcceleratorMaintenanceState::MergeGlobalPendingLazyRebuild(const InvalidationPlan &plan) {
	if (plan.IsEmpty()) {
		return pending_generation;
	}
	std::lock_guard<std::recursive_mutex> guard(rebuild_mutex);
	MergeInvalidationPlan(pending_plan, plan);
	pending_generation++;
	return pending_generation;
}

RebuildClaim AcceleratorMaintenanceState::BeginRebuild() {
	std::lock_guard<std::recursive_mutex> guard(rebuild_mutex);
	RebuildClaim claim;
	if (pending_plan.IsEmpty()) {
		return claim;
	}
	claim.captured_generation = pending_generation;
	claim.captured_plan = pending_plan;
	return claim;
}

void AcceleratorMaintenanceState::CompleteRebuild(const RebuildClaim &claim, const InvalidationPlan &completed_plan) {
	if (!claim.HasWork() || completed_plan.IsEmpty()) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(rebuild_mutex);
	SubtractInvalidationPlan(pending_plan, completed_plan);
}

void AcceleratorMaintenanceState::WithRebuildLock(const std::function<void()> &fn) {
	std::lock_guard<std::recursive_mutex> guard(rebuild_mutex);
	fn();
}

uint64_t AcceleratorMaintenanceState::BumpCommittedMutationEpoch() {
	std::lock_guard<std::recursive_mutex> guard(maintenance_publish_mutex);
	committed_mutation_epoch++;
	return committed_mutation_epoch;
}

uint64_t AcceleratorMaintenanceState::GetCommittedMutationEpoch() const {
	return committed_mutation_epoch;
}

void AcceleratorMaintenanceState::WithMaintenancePublishLock(const std::function<void()> &fn) {
	std::lock_guard<std::recursive_mutex> guard(maintenance_publish_mutex);
	fn();
}

} // namespace accelerator_codegen::aux_state
