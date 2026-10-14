// GENERATED QUERY: tpch Q3 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ3Layout {
  static constexpr const char *kAccelerator = "tpch_q3";

  // Per-segment interval indexes are keyed by market-segment id.
  static constexpr PartitionId kSegmentBuilding = 1;
  static constexpr PartitionId kSegmentFurniture = 2;
  static constexpr PartitionId kSegmentHousehold = 3;
  static constexpr PartitionId kSegmentMachinery = 4;
  static constexpr PartitionId kSegmentAutomobile = 5;
  static constexpr PartitionId kFirstSegment = kSegmentBuilding;
  static constexpr PartitionId kLastSegment = kSegmentAutomobile;
  // Dense orderkey → segment routing table (Side partition).
  static constexpr PartitionId kOrderSegmentSide = 6;
  static constexpr PartitionId kCustomerSegments = 7;
  static constexpr PartitionId kOrderRoutes = 8;

  static constexpr bool IsSegmentPartition(PartitionId partition) {
    return partition >= kFirstSegment && partition <= kLastSegment;
  }
};

} // namespace accelerator_codegen::aux_state::core
