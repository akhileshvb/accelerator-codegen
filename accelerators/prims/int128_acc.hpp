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

#include "prims/pg_numeric.hpp"

namespace accelerator_codegen::aux_state::core::i128 {

// Exact decimal text of a scaled int128 coefficient already lives in
// pg_numeric::ToText(coeff, scale) (PostgreSQL numeric_out format); use that.
// This header adds overflow-checked 128-bit accumulation for SUM-style state.
using Int128 = pg_numeric::Int128;

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
inline std::string ToString(Int128 v) { return pg_numeric::ToText(v, 0); }

} // namespace accelerator_codegen::aux_state::core::i128
