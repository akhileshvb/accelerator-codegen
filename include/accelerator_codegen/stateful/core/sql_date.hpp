#pragma once
//
// sql_date
// PURPOSE    Proleptic-Gregorian civil date <-> days-since-1970 conversion and
//            PostgreSQL/MySQL month/year interval arithmetic with end-of-month
//            clamping.
// COMPLEXITY O(1), branch-light integer arithmetic.
// MEMORY     Stateless; dates are int32 day numbers.
// COW/THREADS Pure functions; thread-safe.
// USE WHEN   any date predicate, interval or extract over int32 day columns.
//

#include <cstdint>

namespace accelerator_codegen::aux_state::core::sql_date {

// Proleptic-Gregorian civil date <-> days since 1970-01-01 (day 0 = 1970-01-01).
// Correct for the whole int32 day range (negative days, years < 1970 / > 2100).
// Algorithms: H. Hinnant's days_from_civil / civil_from_days, in int64.
//
// Interval arithmetic matches PostgreSQL and MySQL `date + INTERVAL n MONTH|YEAR`:
// add to the (year, month), keep the day-of-month, and CLAMP it to the last day
// of the target month (Jan 31 + 1 month = Feb 28/29; Feb 29 + 1 year = Feb 28).
// Subtracting is adding a negative n. Results outside int32 days are undefined
// (callers wanting the SQL "date out of range" error must range-check).
struct Civil {
  int32_t year;
  int32_t month; // 1..12
  int32_t day;   // 1..31
};

constexpr bool IsLeapYear(int64_t y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

constexpr int DaysInMonth(int64_t y, int m) {
  constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && IsLeapYear(y) ? 29 : kDays[m - 1];
}

constexpr int64_t DaysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

constexpr Civil CivilFromDays(int64_t z) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  return {static_cast<int32_t>(y + (m <= 2)), m, d};
}

constexpr int32_t YearOf(int64_t days) { return CivilFromDays(days).year; }
constexpr int32_t MonthOf(int64_t days) { return CivilFromDays(days).month; }

constexpr int64_t AddDays(int64_t days, int64_t n) { return days + n; }

constexpr int64_t AddMonths(int64_t days, int64_t n) {
  const Civil c = CivilFromDays(days);
  const int64_t total = static_cast<int64_t>(c.year) * 12 + (c.month - 1) + n;
  int64_t y = total / 12, mo = total % 12;
  if (mo < 0) {
    mo += 12;
    --y;
  }
  const int m = static_cast<int>(mo) + 1;
  const int dim = DaysInMonth(y, m);
  return DaysFromCivil(y, m, c.day > dim ? dim : c.day);
}

constexpr int64_t AddYears(int64_t days, int64_t n) { return AddMonths(days, n * 12); }

// First day of the month containing `days`.
constexpr int64_t MonthStart(int64_t days) {
  const Civil c = CivilFromDays(days);
  return days - (c.day - 1);
}
constexpr bool IsMonthStart(int64_t days) { return CivilFromDays(days).day == 1; }
// Months since year 0 (year*12 + month-1): handy dense month index.
constexpr int64_t MonthIndex(int64_t days) {
  const Civil c = CivilFromDays(days);
  return static_cast<int64_t>(c.year) * 12 + (c.month - 1);
}

} // namespace accelerator_codegen::aux_state::core::sql_date
