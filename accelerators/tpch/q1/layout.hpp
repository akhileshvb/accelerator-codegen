// GENERATED QUERY: tpch Q1 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ1Layout {
  static constexpr const char *kAccelerator = "tpch_q1";

  // Day partitions use shipdate days_since_epoch directly as the partition id.
  static constexpr PartitionId PartitionFromDays(int32_t days_since_epoch) {
    return static_cast<PartitionId>(days_since_epoch);
  }

  static int32_t DaysFromPartition(PartitionId partition) {
    return static_cast<int32_t>(partition);
  }
};

} // namespace accelerator_codegen::aux_state::core
