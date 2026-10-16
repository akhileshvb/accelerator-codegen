// SCAFFOLDING
// Coarse state-size estimate for reports.
#include "substrate/aux/state/aux_state_footprint.hpp"

#include "substrate/aux/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state {

uint64_t ComputeAuxStateFootprintBytes(const VersionedAuxStore &store,
                                       const char *accelerator_id) {
  (void)accelerator_id;
  constexpr uint64_t kBytesPerPartition = 64; // payloads are opaque to the substrate
  return static_cast<uint64_t>(store.ValidPartitionCount()) * kBytesPerPartition;
}

} // namespace accelerator_codegen::aux_state
