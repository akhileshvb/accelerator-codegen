// SCAFFOLDING
// Coarse state-size estimate for reports.
#pragma once

#include <cstdint>

namespace accelerator_codegen::aux_state {

class VersionedAuxStore;

// A rough estimate (a fixed size per valid partition: payloads are opaque here).
uint64_t ComputeAuxStateFootprintBytes(const VersionedAuxStore &store,
                                       const char *accelerator_id);

} // namespace accelerator_codegen::aux_state
