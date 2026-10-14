// GENERATED QUERY: sec_edgar Q4 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ4Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q4";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q4_aux_parallel";
  static constexpr PartitionId kSicCohort = 0;
  static constexpr PartitionId kPreEqBridge = 1;
  static constexpr PartitionId kTagTlabel = 2;
  static constexpr PartitionId kGroupAggregates = 3;

  static constexpr std::array<PartitionId, 4> Partitions() {
    return {kSicCohort, kPreEqBridge, kTagTlabel, kGroupAggregates};
  }
};

} // namespace accelerator_codegen::aux_state::core
