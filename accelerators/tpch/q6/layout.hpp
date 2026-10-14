// GENERATED QUERY: tpch Q6 state layout (slot identities shared by every engine).
#pragma once

#include <array>
#include <cstdint>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

struct TpchQ6Layout {
  static constexpr const char *kAccelerator = "tpch_q6";
  static constexpr PartitionId kMonthPartitionBase = 1'000'000'000ULL;

  static constexpr PartitionId DayPartition(int32_t days_since_epoch) {
    return static_cast<PartitionId>(days_since_epoch);
  }

  static constexpr PartitionId MonthPartition(int32_t month_key) {
    return kMonthPartitionBase + static_cast<PartitionId>(month_key);
  }

  static constexpr bool IsMonthPartition(PartitionId partition) {
    return partition >= kMonthPartitionBase;
  }

  static constexpr bool IsDayPartition(PartitionId partition) {
    return partition < kMonthPartitionBase;
  }

  static int32_t DaysFromPartition(PartitionId partition) {
    return static_cast<int32_t>(partition);
  }

  static int32_t MonthKeyFromPartition(PartitionId partition) {
    return static_cast<int32_t>(partition - kMonthPartitionBase);
  }

  static PartitionId PartitionFromDays(int32_t days_since_epoch) {
    return DayPartition(days_since_epoch);
  }
};

} // namespace accelerator_codegen::aux_state::core
