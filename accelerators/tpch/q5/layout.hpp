// GENERATED QUERY: tpch Q5 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ5Layout {
  static constexpr const char *kAccelerator = "tpch_q5";
  static constexpr PartitionId kSide = 0;
  static constexpr PartitionId kTreeBase = 1000000;
};

} // namespace accelerator_codegen::aux_state::core
