// GENERATED BACKEND
#pragma once
//
// mysql_decimal (Div, Avg)
// PURPOSE    MySQL DECIMAL result scales over int128 coefficients: a / b has
//            scale(a) + div_precision_increment (default 4), AVG(decimal) has
//            scale(arg) + div_precision_increment; the quotient is rounded half
//            away from zero at that scale. Division by zero yields NULL (SELECT,
//            not INSERT/UPDATE) -> the functions return false.
// COMPLEXITY O(result scale) long division.
// MEMORY     Stateless.
// COW/THREADS Pure functions; thread-safe.
// USE WHEN   emitting a quotient / average whose text must equal what MySQL's
//            DECIMAL arithmetic prints (the PostgreSQL backend's pg_numeric_div.hpp
//            is the counterpart; the two scale rules differ). Text:
//            core::decimal_text::ToText.
//

#include <cstdint>
#include <stdexcept>

#include "prims/decimal_text.hpp"

namespace accelerator_codegen::mysql_decimal {

namespace dt = aux_state::core::decimal_text;

inline constexpr int kDivPrecisionIncrement = 4; // @@div_precision_increment default
inline constexpr int kMaxScale = 30;             // DECIMAL_MAX_SCALE

// (ca / 10^sa) / (cb / 10^sb) at scale min(sa + increment, 30). False when
// cb == 0 (SQL NULL). Throws std::overflow_error beyond 127 bits.
inline bool Div(dt::Int128 ca, int sa, dt::Int128 cb, int sb, dt::Decimal &out,
                int increment = kDivPrecisionIncrement) {
  if (cb == 0)
    return false;
  int rscale = sa + increment;
  if (rscale > kMaxScale)
    rscale = kMaxScale;
  const int shift = rscale - sa + sb; // |ca| * 10^shift / |cb|, shift >= 0 for sa <= 30
  if (shift < 0)
    throw std::overflow_error("mysql_decimal::Div: operand scale too large");
  const dt::UInt128 num = dt::Abs(ca);
  const dt::UInt128 den = dt::Abs(cb);
  dt::UInt128 q = num / den;
  dt::UInt128 r = num % den;
  const dt::UInt128 limit = (~static_cast<dt::UInt128>(0) >> 1) / 10;
  for (int i = 0; i < shift; ++i) {
    if (q > limit || r > (~static_cast<dt::UInt128>(0)) / 10)
      throw std::overflow_error("mysql_decimal::Div: coefficient exceeds 127 bits");
    r *= 10;
    q = q * 10 + r / den;
    r %= den;
  }
  if (r != 0 && r >= den - r)
    q += 1;
  const bool negative = (ca < 0) != (cb < 0);
  out.coeff = negative ? -static_cast<dt::Int128>(q) : static_cast<dt::Int128>(q);
  out.scale = rscale;
  return true;
}

// AVG over `n` decimal values with exact sum `sum` at scale s: sum / n at scale
// min(s + increment, 30). False for n <= 0 (NULL).
inline bool Avg(dt::Int128 sum, int s, int64_t n, dt::Decimal &out, int increment = kDivPrecisionIncrement) {
  if (n <= 0)
    return false;
  return Div(sum, s, static_cast<dt::Int128>(n), 0, out, increment);
}

} // namespace accelerator_codegen::mysql_decimal
