#pragma once
//
// i128 overflow-checked accumulation
// PURPOSE    Overflow-checked add/sub/mul-add on __int128 for exact SUM-style state;
//            throws std::overflow_error instead of wrapping.
// COMPLEXITY O(1).
// MEMORY     16 B per accumulator.
// COW/THREADS Pure functions on caller-owned accumulators.
// USE WHEN   exact SUM/AVG state over decimals; never accumulate scaled decimals in
//            int64.
//

#include <cstdint>
#include <stdexcept>
#include <string>


namespace accelerator_codegen::aux_state::core::i128 {

// This header adds overflow-checked 128-bit accumulation for SUM-style state.
using Int128 = __int128;

// acc += v; throws std::overflow_error instead of wrapping.
inline void AddChecked(Int128 &acc, Int128 v) {
  Int128 r;
  if (__builtin_add_overflow(acc, v, &r))
    throw std::overflow_error("int128 accumulation overflow");
  acc = r;
}
// a * b widened to int128 (int64 x int64 never overflows).
inline Int128 Mul64(int64_t a, int64_t b) { return static_cast<Int128>(a) * b; }
// Decimal text of an int128 integer (no scale).
inline std::string ToString(Int128 v) {
  const bool negative = v < 0;
  unsigned __int128 magnitude = negative ? ~static_cast<unsigned __int128>(v) + 1 : static_cast<unsigned __int128>(v);
  std::string digits;
  do {
    digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(magnitude % 10)));
    magnitude /= 10;
  } while (magnitude != 0);
  return negative ? "-" + digits : digits;
}

} // namespace accelerator_codegen::aux_state::core::i128
