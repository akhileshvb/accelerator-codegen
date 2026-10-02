// GENERATED QUERY: tpch Q14 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ14Layout {
  static constexpr const char *kAccelerator = "tpch_q14";
  static constexpr PartitionId kPartPromo = 0;
  // Month partitions are keyed by YYYYMM shipdate month.
};

} // namespace accelerator_codegen::aux_state::core
