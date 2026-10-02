// GENERATED QUERY: sec_edgar Q6 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ6Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q6";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q6_aux_parallel";
  static constexpr PartitionId kFy2023Cohort = 0;
  static constexpr PartitionId kPreIsBridge = 1;
  static constexpr PartitionId kGroupAggregates = 2;

  static constexpr std::array<PartitionId, 3> Partitions() {
    return {kFy2023Cohort, kPreIsBridge, kGroupAggregates};
  }
};

} // namespace accelerator_codegen::aux_state::core
