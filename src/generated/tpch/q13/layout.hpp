// GENERATED QUERY: tpch Q13 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ13Layout {
  static constexpr const char *kAccelerator = "tpch_q13_aux";
  static constexpr PartitionId kCustomerCounts = 0;
  static constexpr PartitionId kMain = kCustomerCounts;

  static constexpr std::array<PartitionId, 1> Partitions() {
    return {kCustomerCounts};
  }
};

} // namespace accelerator_codegen::aux_state::core
