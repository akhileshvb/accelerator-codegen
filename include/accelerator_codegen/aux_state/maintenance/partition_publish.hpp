#pragma once

#include <cstdint>
#include <memory>

#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state::maintenance {

void PublishValidatedPartitionUpdates(PartitionedStateStore &store, const std::vector<StagedPartitionUpdate> &updates,
                                      uint64_t valid_through_mutation_epoch);

} // namespace accelerator_codegen::aux_state::maintenance
