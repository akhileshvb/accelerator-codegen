// SCAFFOLDING
// Merging of invalidation plans.
#include "substrate/aux/state/invalidation_plan.hpp"

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

} // namespace accelerator_codegen::aux_state
