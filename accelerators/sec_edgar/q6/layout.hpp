// GENERATED QUERY: sec_edgar Q6 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

// One partition holds the whole (immutable, copy-on-write) state: the join
// structure, the aggregates and the top-K cache move together in a single
// atomic publish.
struct SecEdgarQ6Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q6";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q6_aux_parallel";
  static constexpr PartitionId kState = 0;

  static constexpr std::array<PartitionId, 1> Partitions() { return {kState}; }
};

} // namespace accelerator_codegen::aux_state::core
