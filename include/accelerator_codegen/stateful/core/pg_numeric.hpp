// PostgreSQL NUMERIC arithmetic needed to emit exact aggregate results.
//
// Follows PostgreSQL's numeric division (numeric.c: the weight and first
// digit of each operand, select_div_scale, rounding to the result scale):
//
//   avg(numeric) over values of display scale s with integer coefficient sum
//   `sum` and count `n` is sum/10^s / n rounded half away from zero at
//   rscale = select_div_scale(sum/10^s [dscale s], n [dscale 0]).
//
// Values are integer coefficients at a decimal scale (value = coeff / 10^scale).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace accelerator_codegen::pg_numeric {

#if !defined(__SIZEOF_INT128__)
#error "pg_numeric requires __int128"
#endif

using Int128 = __int128;
using UInt128 = unsigned __int128;

inline constexpr int kDecDigits = 4;          // DEC_DIGITS
inline constexpr int kMinSigDigits = 16;      // NUMERIC_MIN_SIG_DIGITS
inline constexpr int kMinDisplayScale = 0;    // NUMERIC_MIN_DISPLAY_SCALE
inline constexpr int kMaxDisplayScale = 1000; // NUMERIC_MAX_DISPLAY_SCALE

struct Decimal {
  Int128 coeff = 0;
  int scale = 0;
};

inline UInt128 Abs(Int128 v) {
  return v < 0 ? static_cast<UInt128>(-(v + 1)) + 1 : static_cast<UInt128>(v);
}

// Number of decimal digits of a (1 for 0).
inline int DigitCount(UInt128 a) {
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
inline int Weight(Int128 coeff, int scale) {
  if (coeff == 0) {
    return 0;
  }
  return FloorDiv(DigitCount(Abs(coeff)) - 1 - scale, kDecDigits);
}

// Leading base-10000 digit (numeric_pg_first_digit).
inline int64_t FirstDigit(Int128 coeff, int scale) {
  if (coeff == 0) {
    return 0;
  }
  const int exponent = scale + Weight(coeff, scale) * kDecDigits;
  UInt128 a = Abs(coeff);
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
inline int DivScale(Int128 c1, int s1, Int128 c2, int s2) {
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
inline Decimal AvgOfScaledSum(Int128 sum, int s, int64_t n) {
  if (n <= 0) {
    throw std::invalid_argument("AvgOfScaledSum: count must be positive");
  }
  const int rscale = DivScale(sum, s, static_cast<Int128>(n), 0);
  const UInt128 den = static_cast<UInt128>(n);
  // |sum| * 10^(rscale - s) / n by long division (rscale >= s).
  UInt128 q = Abs(sum) / den;
  UInt128 r = Abs(sum) % den;
  const UInt128 limit = (~static_cast<UInt128>(0) >> 1) / 10;
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
  Decimal out;
  out.coeff = sum < 0 ? -static_cast<Int128>(q) : static_cast<Int128>(q);
  out.scale = rscale;
  return out;
}

// PostgreSQL numeric_out text for coeff / 10^scale with display scale `scale`.
inline std::string ToText(Int128 coeff, int scale) {
  const bool negative = coeff < 0;
  UInt128 a = Abs(coeff);
  std::string digits;
  do {
    digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(a % 10)));
    a /= 10;
  } while (a != 0);
  if (scale > 0) {
    if (static_cast<int>(digits.size()) <= scale) {
      digits.insert(0, static_cast<size_t>(scale + 1 - static_cast<int>(digits.size())), '0');
    }
    digits.insert(digits.size() - static_cast<size_t>(scale), 1, '.');
  }
  if (negative && coeff != 0) {
    digits.insert(digits.begin(), '-');
  }
  return digits;
}

inline std::string ToText(const Decimal &d) { return ToText(d.coeff, d.scale); }

} // namespace accelerator_codegen::pg_numeric
