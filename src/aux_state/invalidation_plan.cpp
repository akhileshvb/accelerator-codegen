#include "accelerator_codegen/aux_state/invalidation_plan.hpp"

namespace accelerator_codegen::aux_state {

void MergeInvalidationPlan(InvalidationPlan &accumulator, const InvalidationPlan &delta) {
	if (delta.IsEmpty()) {
		return;
	}
	if (delta.scope == InvalidationScope::AllPartitions) {
		accumulator.scope = InvalidationScope::AllPartitions;
		accumulator.partitions.clear();
		return;
	}
	if (accumulator.scope == InvalidationScope::AllPartitions) {
		return;
	}
	if (delta.scope == InvalidationScope::PartitionSet) {
		accumulator.scope = InvalidationScope::PartitionSet;
		for (auto partition : delta.partitions) {
			accumulator.partitions.insert(partition);
		}
	}
}

void SubtractInvalidationPlan(InvalidationPlan &accumulator, const InvalidationPlan &completed) {
	if (completed.scope == InvalidationScope::AllPartitions) {
		accumulator = InvalidationPlan {};
		return;
	}
	if (accumulator.scope == InvalidationScope::AllPartitions) {
		return;
	}
	if (completed.scope != InvalidationScope::PartitionSet) {
		return;
	}
	for (auto partition : completed.partitions) {
		accumulator.partitions.erase(partition);
	}
	if (accumulator.partitions.empty() && accumulator.scope == InvalidationScope::PartitionSet) {
		accumulator.scope = InvalidationScope::None;
	}
}

} // namespace accelerator_codegen::aux_state
