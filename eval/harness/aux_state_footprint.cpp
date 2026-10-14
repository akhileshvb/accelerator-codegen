// SCAFFOLDING — footprint helper (no per-query module includes after wipe).
#include "eval/harness/aux_state_footprint.hpp"

#include "substrate/aux/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state::harness {

uint64_t ComputeAuxStateFootprintBytes(const VersionedAuxStore &store,
                                       const char *accelerator_id) {
  (void)accelerator_id;
  // Without regenerated typed images, report slot count * 0 payload estimate.
  return static_cast<uint64_t>(store.ValidPartitionCount()) * 64ull;
}

} // namespace accelerator_codegen::aux_state::harness
