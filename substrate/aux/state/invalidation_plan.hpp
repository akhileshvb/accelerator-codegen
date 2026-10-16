// SCAFFOLDING
// Which partitions of an accelerator's state a change invalidates.
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

// Adds `delta` to `accumulator`; AllPartitions absorbs any set.
void MergeInvalidationPlan(InvalidationPlan &accumulator, const InvalidationPlan &delta);

} // namespace accelerator_codegen::aux_state
