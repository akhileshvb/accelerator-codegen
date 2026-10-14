// GENERATED QUERY: tpch Q7 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ7Layout {
  static constexpr const char *kAccelerator = "tpch_q7";
  static constexpr PartitionId kSidePartition = 0;
  static constexpr PartitionId kFirstMonth = 1000000;
};

} // namespace accelerator_codegen::aux_state::core
