// SCAFFOLDING
#pragma once

#include <unordered_set>

#include "substrate/aux/state/partition_key.hpp"

namespace accelerator_codegen::aux_state {

enum class InvalidationScope { None, PartitionSet, AllPartitions };

struct InvalidationPlan {
	InvalidationScope scope = InvalidationScope::None;
	std::unordered_set<PartitionKey> partitions;

	bool IsEmpty() const {
		return scope == InvalidationScope::None ||
		       (scope == InvalidationScope::PartitionSet && partitions.empty());
	}
};

void MergeInvalidationPlan(InvalidationPlan &accumulator, const InvalidationPlan &delta);
void SubtractInvalidationPlan(InvalidationPlan &accumulator, const InvalidationPlan &completed);

} // namespace accelerator_codegen::aux_state
