// SCAFFOLDING
#pragma once

#include <cstdint>

namespace accelerator_codegen::aux_state {
class VersionedAuxStore;
}

namespace accelerator_codegen::aux_state::harness {

// Estimated bytes of the store's newest valid partitions.
uint64_t ComputeAuxStateFootprintBytes(const VersionedAuxStore &store,
                                       const char *accelerator_id);

} // namespace accelerator_codegen::aux_state::harness
