// GENERATED QUERY: tpch Q2 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ2Layout {
  static constexpr const char *kAccelerator = "tpch_q2";
  static constexpr PartitionId kFull = 0;

  static constexpr std::array<PartitionId, 1> Partitions() {
    return {kFull};
  }
};

} // namespace accelerator_codegen::aux_state::core
