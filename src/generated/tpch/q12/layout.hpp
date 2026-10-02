// GENERATED QUERY: tpch Q12 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ12Layout {
  static constexpr const char *kAccelerator = "tpch_q12";
  static constexpr PartitionId kOrderPriorities = 0;
  // Day partitions are keyed by receipt-date days since epoch.
};

} // namespace accelerator_codegen::aux_state::core
