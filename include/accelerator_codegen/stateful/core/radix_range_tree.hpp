#pragma once

#include <cstddef>
#include <cstdint>

#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"

namespace accelerator_codegen::aux_state::core {

// Additive point-update / range-sum index over keys in [0, kKeySpan): a
// counting tree with fan-out 64 and 4 levels (level l, cell k = sum of keys
// [k * 64^l, (k + 1) * 64^l)). Values are any additive T (counts, int128 sums).
//
//   * Add(key, by): touches one cell per level (4 chunk clones at most).
//   * SumRange(a, b): <= 2 * 63 cells per level, i.e. O(log N) reads; never
//     allocates.
//   * Copy = four directory copies (~80 KB for sparse keys) - chunks are
//     shared with the source until written (ChunkedCowArray).
// Keys outside [0, kKeySpan) must be kept elsewhere by the caller (a small
// ordered map of outliers is typical). A zero cell reads as T{}.
template <typename T> class RadixRangeTree {
public:
  static constexpr int kLevels = 4;
  static constexpr int kBits = 6;
  static constexpr int64_t kKeySpan = int64_t{1} << (kBits * kLevels);

  static bool InRange(int64_t key) { return key >= 0 && key < kKeySpan; }

  void Add(int64_t key, const T &by) {
    l0_.Mutable(key) += by;
    l1_.Mutable(key >> kBits) += by;
    l2_.Mutable(key >> (2 * kBits)) += by;
    l3_.Mutable(key >> (3 * kBits)) += by;
  }

  // Sum over keys [a, b) (clamped to [0, kKeySpan)).
  T SumRange(int64_t a, int64_t b) const {
    if (a < 0)
      a = 0;
    if (b > kKeySpan)
      b = kKeySpan;
    T sum{};
    for (int level = 0; level < kLevels && a < b; ++level) {
      const int64_t next_a = (a + 63) & ~int64_t{63};
      const int64_t next_b = b & ~int64_t{63};
      if (level == kLevels - 1 || next_a > next_b) {
        for (int64_t k = a; k < b; ++k)
          sum += Cell(level, k);
        break;
      }
      for (int64_t k = a; k < next_a; ++k)
        sum += Cell(level, k);
      for (int64_t k = next_b; k < b; ++k)
        sum += Cell(level, k);
      a = next_a >> kBits;
      b = next_b >> kBits;
    }
    return sum;
  }

  std::size_t MemoryBytes() const {
    return l0_.MemoryBytes() + l1_.MemoryBytes() + l2_.MemoryBytes() +
           l3_.MemoryBytes();
  }

private:
  T Cell(int level, int64_t k) const {
    switch (level) {
    case 0:
      return l0_.Get(k);
    case 1:
      return l1_.Get(k);
    case 2:
      return l2_.Get(k);
    default:
      return l3_.Get(k);
    }
  }

  // Chunk sizes shrink with the level (upper levels have few cells).
  ChunkedCowArray<T, 12> l0_;
  ChunkedCowArray<T, 8> l1_;
  ChunkedCowArray<T, 6> l2_;
  ChunkedCowArray<T, 6> l3_;
};

} // namespace accelerator_codegen::aux_state::core
