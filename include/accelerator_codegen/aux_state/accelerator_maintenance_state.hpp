#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"

namespace accelerator_codegen::aux_state {

struct RebuildClaim {
	uint64_t captured_generation = 0;
	InvalidationPlan captured_plan;

	bool HasWork() const {
		return !captured_plan.IsEmpty();
	}
};

struct AcceleratorMaintenanceState {
	InvalidationPlan pending_plan;
	uint64_t pending_generation = 0;
	uint64_t committed_mutation_epoch = 0;
	std::recursive_mutex rebuild_mutex;
	std::recursive_mutex maintenance_publish_mutex;

	uint64_t MergeGlobalPendingLazyRebuild(const InvalidationPlan &plan);
	RebuildClaim BeginRebuild();
	void CompleteRebuild(const RebuildClaim &claim, const InvalidationPlan &completed_plan);
	void WithRebuildLock(const std::function<void()> &fn);
	uint64_t BumpCommittedMutationEpoch();
	uint64_t GetCommittedMutationEpoch() const;

	void WithMaintenancePublishLock(const std::function<void()> &fn);
};

} // namespace accelerator_codegen::aux_state
