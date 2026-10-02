// GENERATED QUERY: sec_edgar Q2 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ2Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q2";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q2_aux_parallel";
  static constexpr PartitionId kMaxPureMap = 0;
  static constexpr PartitionId kFy2022Cohort = 1;
  static constexpr PartitionId kCandidates = 2;

  static constexpr std::array<PartitionId, 3> Partitions() {
    return {kMaxPureMap, kFy2022Cohort, kCandidates};
  }
};

} // namespace accelerator_codegen::aux_state::core
