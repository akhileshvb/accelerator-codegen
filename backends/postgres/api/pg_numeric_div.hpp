// GENERATED BACKEND
// PostgreSQL NUMERIC a / b over scaled integer coefficients (numeric_div).
//
// Result scale = select_div_scale(a, b) (pg_numeric::DivScale), quotient
// rounded half away from zero at that scale. Companion to pg_numeric.hpp
// (which has the avg(numeric) special case).
#pragma once
//
// pg_numeric::DivRound
// PURPOSE    PostgreSQL NUMERIC division over scaled int128 coefficients
//            (select_div_scale result scale, round half away from zero).
// COMPLEXITY O(result scale) long division.
// MEMORY     Stateless.
// COW/THREADS Pure function; thread-safe.
// USE WHEN   emitting exact quotient/ratio columns that must match PostgreSQL's
//            numeric_div.
//

#include <stdexcept>

#include "backends/postgres/api/pg_numeric.hpp"

namespace accelerator_codegen::pg_numeric {

// (ca / 10^sa) / (cb / 10^sb). Throws std::domain_error on cb == 0 (SQL:
// "division by zero") and std::overflow_error when the coefficient does not
// fit in 127 bits.
inline Decimal DivRound(Int128 ca, int sa, Int128 cb, int sb) {
  if (cb == 0)
    throw std::domain_error("division by zero");
  const int rscale = DivScale(ca, sa, cb, sb);
  const UInt128 num = Abs(ca);
  const UInt128 den = Abs(cb);
  // |ca| * 10^(rscale - sa + sb) / |cb| by long division; rscale >= sa so the
  // shift e = rscale - sa + sb is >= sb >= 0.
  const int shift = rscale - sa + sb;
  UInt128 q = num / den;
  UInt128 r = num % den;
  const UInt128 limit = (~static_cast<UInt128>(0) >> 1) / 10;
  for (int i = 0; i < shift; ++i) {
    if (q > limit)
      throw std::overflow_error("DivRound: coefficient exceeds 127 bits");
    // r < den < 2^127, so r * 10 can wrap only for den >= 2^124 (unsupported).
    if (r > (~static_cast<UInt128>(0)) / 10)
      throw std::overflow_error("DivRound: divisor too large");
    r *= 10;
    q = q * 10 + r / den;
    r %= den;
  }
  if (r != 0 && r >= den - r) // 2r >= den, without overflow
    q += 1;
  const bool negative = (ca < 0) != (cb < 0);
  Decimal out;
  out.coeff = negative ? -static_cast<Int128>(q) : static_cast<Int128>(q);
  out.scale = rscale;
  return out;
}

} // namespace accelerator_codegen::pg_numeric
