// GENERATED QUERY: sec_edgar Q24 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct SecEdgarQ24Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q24";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q24_aux_parallel";
  static constexpr PartitionId kPreMembership = 0;
  static constexpr PartitionId kTagVersions = 1;
  static constexpr PartitionId kGroupAggregates = 2;

  static constexpr std::array<PartitionId, 3> Partitions() {
    return {kPreMembership, kTagVersions, kGroupAggregates};
  }
};

} // namespace accelerator_codegen::aux_state::core
