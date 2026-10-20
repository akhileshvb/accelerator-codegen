#pragma once
//
// decimal_text::Parse, ToText
// PURPOSE    Exact parse of engine-printed NUMERIC/DECIMAL text into an int128
//            coefficient at a fixed scale, and the inverse: the plain decimal
//            text of a scaled coefficient (every engine prints a fixed-scale
//            decimal this way).
// COMPLEXITY O(length).
// MEMORY     Stateless (ToText allocates the result string).
// COW/THREADS Pure functions; thread-safe.
// USE WHEN   reading decimal values as text from an engine API, or writing an
//            exact decimal result as text. Engine-specific result scales and
//            rounding (division, AVG) are not here: they are the backend's
//            value-semantics headers.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace accelerator_codegen::aux_state::core::decimal_text {

#if defined(__SIZEOF_INT128__)
using Int128 = __int128;
using UInt128 = unsigned __int128;

// value = coeff / 10^scale.
struct Decimal {
  Int128 coeff = 0;
  int scale = 0;
};

inline UInt128 Abs(Int128 v) {
  return v < 0 ? static_cast<UInt128>(-(v + 1)) + 1 : static_cast<UInt128>(v);
}

// The text of coeff / 10^scale with exactly `scale` fraction digits.
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

// Exact decimal text -> integer coefficient at a fixed scale (value =
// coeff / 10^scale), as engines print NUMERIC / DECIMAL values: optional sign,
// digits, optional ".digits". Digits beyond `scale` are rounded half away from
// zero (the caller passes the column's own scale, so nothing is lost there).
// Returns false for malformed text (empty, no digits, stray characters) or when
// the coefficient does not fit in 128 bits.
inline bool Parse(std::string_view text, int scale, Int128 &out) {
  std::size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '-' || text[i] == '+'))
    negative = text[i++] == '-';
  Int128 coeff = 0;
  int frac = 0;
  bool seen_digit = false, seen_dot = false, round_up = false;
  const Int128 limit = static_cast<Int128>(
      (~static_cast<unsigned __int128>(0) >> 1) / 10 - 1);
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '.' && !seen_dot) {
      seen_dot = true;
      continue;
    }
    if (c < '0' || c > '9')
      return false;
    seen_digit = true;
    if (seen_dot && frac >= scale) {
      if (frac == scale)
        round_up = c >= '5';
      ++frac;
      continue;
    }
    if (coeff > limit)
      return false;
    coeff = coeff * 10 + (c - '0');
    if (seen_dot)
      ++frac;
  }
  if (!seen_digit)
    return false;
  for (; frac < scale; ++frac) {
    if (coeff > limit)
      return false;
    coeff *= 10;
  }
  if (round_up)
    ++coeff;
  out = negative ? -coeff : coeff;
  return true;
}
#endif

} // namespace accelerator_codegen::aux_state::core::decimal_text
