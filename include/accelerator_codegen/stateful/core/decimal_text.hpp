#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace accelerator_codegen::aux_state::core::decimal_text {

#if defined(__SIZEOF_INT128__)
using Int128 = __int128;

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
