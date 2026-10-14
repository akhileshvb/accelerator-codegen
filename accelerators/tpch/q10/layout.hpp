// GENERATED QUERY: tpch Q10 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ10Layout {
  static constexpr const char *kAccelerator = "tpch_q10";
  static constexpr PartitionId kCustomerDimensions = 0;
  static constexpr PartitionId kNationDimensions = 1;
  static constexpr PartitionId kOrderMetadata = 2;
  static constexpr PartitionId kRevenueBase = 3;

  // Month partitions begin at this value and are encoded as YYYYMM, matching
  // the existing Q10 state runtime. The first four slots are fixed side stores.
  static constexpr PartitionId kFirstMonth = 1000000;
};

} // namespace accelerator_codegen::aux_state::core
