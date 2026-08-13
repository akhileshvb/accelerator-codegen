// SCAFFOLDING
#pragma once

#include <cstdint>

namespace accelerator_codegen::aux_state {
class PartitionedStateStore;
}

namespace accelerator_codegen::aux_state::harness {

// Sum serialized partition payloads for all valid slots in the store.
uint64_t ComputeAuxStateFootprintBytes(const PartitionedStateStore &store,
                                       const char *accelerator_id);

} // namespace accelerator_codegen::aux_state::harness
