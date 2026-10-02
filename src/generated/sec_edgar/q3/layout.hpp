// GENERATED QUERY: sec_edgar Q3 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ3Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q3";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q3_aux_parallel";
  static constexpr PartitionId kFy2022Cohort = 0;
  static constexpr PartitionId kCikTotals = 1;

  static constexpr std::array<PartitionId, 2> Partitions() {
    return {kFy2022Cohort, kCikTotals};
  }
};

} // namespace accelerator_codegen::aux_state::core
