// GENERATED BACKEND
// PostgreSQL NUMERIC arithmetic needed to emit exact aggregate results.
//
// Follows PostgreSQL's numeric division (numeric.c: the weight and first
// digit of each operand, select_div_scale, rounding to the result scale):
//
//   avg(numeric) over values of display scale s with integer coefficient sum
//   `sum` and count `n` is sum/10^s / n rounded half away from zero at
//   rscale = select_div_scale(sum/10^s [dscale s], n [dscale 0]).
//
// Values are integer coefficients at a decimal scale (value = coeff / 10^scale);
// the text of a result is core::decimal_text::ToText (prims/decimal_text.hpp).
#pragma once
//
// pg_numeric (DivScale, AvgOfScaledSum)
// PURPOSE    PostgreSQL NUMERIC helpers over int128 coefficients: result-scale
//            selection and avg(numeric).
// COMPLEXITY O(digits).
// MEMORY     Stateless.
// COW/THREADS Pure functions; thread-safe.
// USE WHEN   emitting exact aggregate results whose scale must equal PostgreSQL's NUMERIC.
//

#include <cstdint>
#include <stdexcept>
#include <string>

#include "prims/decimal_text.hpp"

namespace accelerator_codegen::pg_numeric {

namespace dt = aux_state::core::decimal_text;

#if !defined(__SIZEOF_INT128__)
#error "pg_numeric requires __int128"
#endif

inline constexpr int kDecDigits = 4;          // DEC_DIGITS
inline constexpr int kMinSigDigits = 16;      // NUMERIC_MIN_SIG_DIGITS
inline constexpr int kMinDisplayScale = 0;    // NUMERIC_MIN_DISPLAY_SCALE
inline constexpr int kMaxDisplayScale = 1000; // NUMERIC_MAX_DISPLAY_SCALE

// Number of decimal digits of a (1 for 0).
inline int DigitCount(dt::UInt128 a) {
  int digits = 1;
  while (a >= 10) {
    a /= 10;
    ++digits;
  }
  return digits;
}

// Floor division for a positive divisor.
inline int FloorDiv(int a, int b) {
  int q = a / b;
  if ((a % b != 0) && (a < 0)) {
    --q;
  }
  return q;
}

// Base-10000 weight of coeff / 10^scale (numeric_pg_weight).
inline int Weight(dt::Int128 coeff, int scale) {
  if (coeff == 0) {
    return 0;
  }
  return FloorDiv(DigitCount(dt::Abs(coeff)) - 1 - scale, kDecDigits);
}

// Leading base-10000 digit (numeric_pg_first_digit).
inline int64_t FirstDigit(dt::Int128 coeff, int scale) {
  if (coeff == 0) {
    return 0;
  }
  const int exponent = scale + Weight(coeff, scale) * kDecDigits;
  dt::UInt128 a = dt::Abs(coeff);
  if (exponent >= 0) {
    for (int i = 0; i < exponent && a != 0; ++i) {
      a /= 10;
    }
  } else {
    for (int i = 0; i < -exponent; ++i) {
      a *= 10; // exponent >= -3 (the first digit of the quotient), cannot overflow
    }
  }
  return static_cast<int64_t>(a);
}

// select_div_scale (numeric_pg_div_scale).
inline int DivScale(dt::Int128 c1, int s1, dt::Int128 c2, int s2) {
  const int w1 = Weight(c1, s1);
  const int w2 = Weight(c2, s2);
  const int64_t d1 = FirstDigit(c1, s1);
  const int64_t d2 = FirstDigit(c2, s2);
  const int qweight = (d1 <= d2) ? w1 - w2 - 1 : w1 - w2;
  int rscale = kMinSigDigits - qweight * kDecDigits;
  if (rscale < s1) rscale = s1;
  if (rscale < s2) rscale = s2;
  if (rscale < kMinDisplayScale) rscale = kMinDisplayScale;
  if (rscale > kMaxDisplayScale) rscale = kMaxDisplayScale;
  return rscale;
}

// avg(numeric): (sum / 10^s) / n at PostgreSQL's result scale, rounded half
// away from zero (numeric_avg_from_scale_state -> numeric_div_at_scales).
// Requires n > 0. Throws if the coefficient does not fit in 128 bits.
inline dt::Decimal AvgOfScaledSum(dt::Int128 sum, int s, int64_t n) {
  if (n <= 0) {
    throw std::invalid_argument("AvgOfScaledSum: count must be positive");
  }
  const int rscale = DivScale(sum, s, static_cast<dt::Int128>(n), 0);
  const dt::UInt128 den = static_cast<dt::UInt128>(n);
  // |sum| * 10^(rscale - s) / n by long division (rscale >= s).
  dt::UInt128 q = dt::Abs(sum) / den;
  dt::UInt128 r = dt::Abs(sum) % den;
  const dt::UInt128 limit = (~static_cast<dt::UInt128>(0) >> 1) / 10;
  for (int i = 0; i < rscale - s; ++i) {
    if (q > limit) {
      throw std::overflow_error("AvgOfScaledSum: coefficient exceeds 128 bits");
    }
    r *= 10;
    q = q * 10 + r / den;
    r %= den;
  }
  if (2 * r >= den) { // round half away from zero (on the magnitude)
    q += 1;
  }
  dt::Decimal out;
  out.coeff = sum < 0 ? -static_cast<dt::Int128>(q) : static_cast<dt::Int128>(q);
  out.scale = rscale;
  return out;
}

} // namespace accelerator_codegen::pg_numeric
