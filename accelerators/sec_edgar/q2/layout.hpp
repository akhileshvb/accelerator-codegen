// GENERATED QUERY: sec_edgar Q2 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ2Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q2";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q2_aux_parallel";
  // One partition: the whole state (groups, dictionaries, per-adsh sub info).
  static constexpr PartitionId kState = 0;

  static constexpr std::array<PartitionId, 1> Partitions() { return {kState}; }
};

} // namespace accelerator_codegen::aux_state::core
