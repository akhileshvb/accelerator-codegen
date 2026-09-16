#pragma once

#include <cstdint>

// When ACCEL_VERIFICATION_CBMC is set, Postgres Q1 CollectQ1DayPartitions reads
// only these days (harness-populated) so CBMC need not unwind ~2500 iterations.
namespace accel_cbmc {

inline constexpr unsigned kMaxQ1ActiveDays = 4;

extern int32_t q1_active_days[kMaxQ1ActiveDays];
extern unsigned q1_active_day_count;

inline void ResetQ1ActiveDays() { q1_active_day_count = 0; }

inline void AddQ1ActiveDay(int32_t day) {
  for (unsigned i = 0; i < q1_active_day_count; ++i) {
    if (q1_active_days[i] == day)
      return;
  }
  if (q1_active_day_count < kMaxQ1ActiveDays) {
    q1_active_days[q1_active_day_count++] = day;
  }
}

} // namespace accel_cbmc
