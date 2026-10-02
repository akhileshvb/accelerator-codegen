// GENERATED QUERY: sec_edgar Q1 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ1Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q1";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q1_aux_parallel";
  static constexpr PartitionId kGroupKeys = 0;
  static constexpr PartitionId kGroupAggregates = 1;

  static constexpr std::array<PartitionId, 2> Partitions() {
    return {kGroupKeys, kGroupAggregates};
  }
};

} // namespace accelerator_codegen::aux_state::core
