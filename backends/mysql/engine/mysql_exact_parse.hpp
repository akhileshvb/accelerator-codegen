// Exact, strict text parsing for the MySQL host layer (UDF arguments, scan
// rows, mutation-capture rows).  Workload-agnostic; header-only.
//
// Contract: every function either returns the exact value of its text or
// returns false.  Nothing is rounded, saturated or defaulted: malformed text,
// out-of-range values and values that do not fit the target scale are errors,
// and the caller decides how to report them (UDF error, scan failure,
// aux-state invalidation).  No floating point is involved except in
// DecimalFromDouble, which is documented there.
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::mysql::exact {

namespace core = accelerator_codegen::aux_state::core;

// 10^n for 0 <= n <= 18.
inline int64_t Pow10(int n) {
  int64_t v = 1;
  for (int i = 0; i < n; ++i) v *= 10;
  return v;
}

// Strict base-10 integer: optional sign, at least one digit, nothing else,
// must fit int64.
inline bool ParseInt64(const char *s, std::size_t len, int64_t &out) {
  if (!s || len == 0) return false;
  if (*s == '+') { ++s; --len; if (len == 0) return false; }
  const auto r = std::from_chars(s, s + len, out);
  return r.ec == std::errc() && r.ptr == s + len;
}

inline bool ParseUInt64(const char *s, std::size_t len, uint64_t &out) {
  if (!s || len == 0) return false;
  if (*s == '+') { ++s; --len; if (len == 0) return false; }
  const auto r = std::from_chars(s, s + len, out);
  return r.ec == std::errc() && r.ptr == s + len;
}

// Strict decimal text -> unscaled integer and scale: [+-]digits[.digits]
// optionally followed by an exponent (e|E)[+-]digits (MySQL prints DOUBLE
// values that way).  "0.065" -> {65, 3}; "5" -> {5, 0}; "1.50" -> {150, 2};
// "1.5e-2" -> {15, 3}.  A positive exponent that would make the scale
// negative is folded into the value ("1e2" -> {100, 0}).  False if malformed
// or the unscaled value does not fit int64.
inline bool ParseDecimal(const char *s, std::size_t len, core::ScaledDecimal &out) {
  if (!s || len == 0) return false;
  std::size_t i = 0;
  bool negative = false;
  if (s[i] == '+' || s[i] == '-') { negative = (s[i] == '-'); ++i; }
  unsigned __int128 mag = 0;
  const unsigned __int128 limit = static_cast<unsigned __int128>(INT64_MAX) + (negative ? 1 : 0);
  int digits = 0;
  int frac = 0;
  bool seen_dot = false;
  for (; i < len; ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      mag = mag * 10 + static_cast<unsigned>(c - '0');
      if (mag > limit) return false;
      ++digits;
      if (seen_dot) ++frac;
    } else if (c == '.' && !seen_dot) {
      seen_dot = true;
    } else {
      break;
    }
  }
  if (digits == 0) return false;
  long exponent = 0;
  if (i < len) {
    if (s[i] != 'e' && s[i] != 'E') return false;
    ++i;
    bool eneg = false;
    if (i < len && (s[i] == '+' || s[i] == '-')) { eneg = (s[i] == '-'); ++i; }
    if (i == len) return false;
    for (; i < len; ++i) {
      if (s[i] < '0' || s[i] > '9') return false;
      exponent = exponent * 10 + (s[i] - '0');
      if (exponent > 1000) return false;
    }
    if (eneg) exponent = -exponent;
  }
  long scale = frac - exponent;
  while (scale < 0) { // fold a positive exponent into the value
    mag *= 10;
    if (mag > limit) return false;
    ++scale;
  }
  // Drop trailing fractional zeros only when the exponent produced an
  // oversized scale (keeps "1.50" at scale 2).
  while (scale > 18 && mag % 10 == 0) { mag /= 10; --scale; }
  if (scale > 18) return false;
  out.value = negative ? static_cast<int64_t>(-static_cast<__int128>(mag))
                       : static_cast<int64_t>(mag);
  out.scale = static_cast<int32_t>(scale);
  return true;
}

// `value` at `target_scale`, exactly: false if that needs rounding (a nonzero
// digit would be dropped) or overflows int64.
inline bool Rescale(const core::ScaledDecimal &value, int target_scale, int64_t &out) {
  if (target_scale < 0 || target_scale > 18 || value.scale < 0 || value.scale > 18) return false;
  if (target_scale >= value.scale) {
    const __int128 r = static_cast<__int128>(value.value) * Pow10(target_scale - value.scale);
    if (r > INT64_MAX || r < INT64_MIN) return false;
    out = static_cast<int64_t>(r);
    return true;
  }
  const int64_t div = Pow10(value.scale - target_scale);
  if (value.value % div != 0) return false;
  out = value.value / div;
  return true;
}

// The decimal a double prints as when written with the fewest digits that
// read back as the same double (std::to_chars shortest round trip):
// 0.065 -> {65, 3}.  This is what a user who typed the literal means, unlike
// llround(value * 100) which turns 0.065 into 0.07 (or 0.06).  False for
// NaN/inf or values that do not fit.
inline bool DecimalFromDouble(double v, core::ScaledDecimal &out) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
  if (r.ec != std::errc()) return false;
  return ParseDecimal(buf, static_cast<std::size_t>(r.ptr - buf), out);
}

// Days since 1970-01-01 of a civil date.
inline int32_t DaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// Strict 'YYYY-MM-DD' (an optional time part after a space or 'T' is
// accepted only when it is all zeros, as MySQL prints a DATE-valued
// DATETIME).  Validates month and day against the calendar; '0000-00-00' and
// every other invalid date is an error, never a default.
inline bool ParseDate(const char *s, std::size_t len, int32_t &out_days) {
  if (!s || len < 10) return false;
  auto num = [&](std::size_t pos, std::size_t n, int &v) {
    v = 0;
    for (std::size_t k = 0; k < n; ++k) {
      const char c = s[pos + k];
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    return true;
  };
  int y, m, d;
  if (!num(0, 4, y) || s[4] != '-' || !num(5, 2, m) || s[7] != '-' || !num(8, 2, d)) return false;
  if (len > 10) {
    if (s[10] != ' ' && s[10] != 'T') return false;
    for (std::size_t k = 11; k < len; ++k) {
      if (s[k] != '0' && s[k] != ':' && s[k] != '.') return false;
    }
  }
  if (y < 1 || m < 1 || m > 12 || d < 1) return false;
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  const int max_day = kDays[m - 1] + ((m == 2 && leap) ? 1 : 0);
  if (d > max_day) return false;
  out_days = DaysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
  return true;
}

} // namespace accelerator_codegen::aux_state::engine::mysql::exact
