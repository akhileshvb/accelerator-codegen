// GENERATED QUERY: sec_edgar Q1 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ1Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q1";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q1_aux_parallel";
  // The whole state (group aggregates + per-group accession refcounts +
  // accession dictionary) is one immutable image: a maintenance step publishes
  // a new image that shares every untouched refcount chunk with the previous.
  static constexpr PartitionId kState = 0;

  static constexpr std::array<PartitionId, 1> Partitions() { return {kState}; }
};

} // namespace accelerator_codegen::aux_state::core
