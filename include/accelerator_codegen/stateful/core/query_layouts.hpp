// SCAFFOLDING
#pragma once

#include <array>
#include <cstdint>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

// These layouts are deliberately logical. Generated modules may choose a
// compact byte encoding for each image, but the slot identities are shared by
// DuckDB and PostgreSQL so routing and tests do not acquire backend-specific
// meanings.
struct SecEdgarQ2Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q2";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q2_aux_parallel";
  static constexpr PartitionId kMaxPureMap = 0;
  static constexpr PartitionId kFy2022Cohort = 1;
  static constexpr PartitionId kCandidates = 2;

  static constexpr std::array<PartitionId, 3> Partitions() {
    return {kMaxPureMap, kFy2022Cohort, kCandidates};
  }
};

struct SecEdgarQ3Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q3";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q3_aux_parallel";
  static constexpr PartitionId kFy2022Cohort = 0;
  static constexpr PartitionId kCikTotals = 1;

  static constexpr std::array<PartitionId, 2> Partitions() {
    return {kFy2022Cohort, kCikTotals};
  }
};

struct SecEdgarQ6Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q6";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q6_aux_parallel";
  static constexpr PartitionId kFy2023Cohort = 0;
  static constexpr PartitionId kPreIsBridge = 1;
  static constexpr PartitionId kGroupAggregates = 2;

  static constexpr std::array<PartitionId, 3> Partitions() {
    return {kFy2023Cohort, kPreIsBridge, kGroupAggregates};
  }
};

struct SecEdgarQ1Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q1";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q1_aux_parallel";
  static constexpr PartitionId kGroupKeys = 0;
  static constexpr PartitionId kGroupAggregates = 1;

  static constexpr std::array<PartitionId, 2> Partitions() {
    return {kGroupKeys, kGroupAggregates};
  }
};

struct SecEdgarQ4Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q4";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q4_aux_parallel";
  static constexpr PartitionId kSicCohort = 0;
  static constexpr PartitionId kPreEqBridge = 1;
  static constexpr PartitionId kTagTlabel = 2;
  static constexpr PartitionId kGroupAggregates = 3;

  static constexpr std::array<PartitionId, 4> Partitions() {
    return {kSicCohort, kPreEqBridge, kTagTlabel, kGroupAggregates};
  }
};

struct SecEdgarQ24Layout {
  static constexpr const char *kAccelerator = "sec_edgar_q24";
  static constexpr const char *kParallelAccelerator = "sec_edgar_q24_aux_parallel";
  static constexpr PartitionId kPreMembership = 0;
  static constexpr PartitionId kTagVersions = 1;
  static constexpr PartitionId kGroupAggregates = 2;

  static constexpr std::array<PartitionId, 3> Partitions() {
    return {kPreMembership, kTagVersions, kGroupAggregates};
  }
};

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

struct TpchQ12Layout {
  static constexpr const char *kAccelerator = "tpch_q12";
  static constexpr PartitionId kOrderPriorities = 0;
  // Day partitions are keyed by receipt-date days since epoch.
};

struct TpchQ14Layout {
  static constexpr const char *kAccelerator = "tpch_q14";
  static constexpr PartitionId kPartPromo = 0;
  // Month partitions are keyed by YYYYMM shipdate month.
};

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

struct TpchQ5Layout {
  static constexpr const char *kAccelerator = "tpch_q5";
  static constexpr PartitionId kSide = 0;
  static constexpr PartitionId kTreeBase = 1000000;
};
struct TpchQ7Layout {
  static constexpr const char *kAccelerator = "tpch_q7";
  static constexpr PartitionId kSidePartition = 0;
  static constexpr PartitionId kFirstMonth = 1000000;
};


struct TpchQ2Layout {
  static constexpr const char *kAccelerator = "tpch_q2";
  static constexpr PartitionId kFull = 0;

  static constexpr std::array<PartitionId, 1> Partitions() {
    return {kFull};
  }
};

struct TpchQ13Layout {
  static constexpr const char *kAccelerator = "tpch_q13_aux";
  static constexpr PartitionId kCustomerCounts = 0;
  static constexpr PartitionId kMain = kCustomerCounts;

  static constexpr std::array<PartitionId, 1> Partitions() {
    return {kCustomerCounts};
  }
};

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
