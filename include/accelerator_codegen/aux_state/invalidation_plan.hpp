#pragma once

#include <unordered_set>

#include "accelerator_codegen/aux_state/partition_key.hpp"

namespace accelerator_codegen::aux_state {

enum class InvalidationScope { None, PartitionSet, AllPartitions };

struct InvalidationPlan {
	InvalidationScope scope = InvalidationScope::None;
	std::unordered_set<PartitionKey> partitions;

	bool IsEmpty() const {
		return scope == InvalidationScope::None;
	}
};

void MergeInvalidationPlan(InvalidationPlan &accumulator, const InvalidationPlan &delta);

} // namespace accelerator_codegen::aux_state
