#pragma once

#include <cstddef>
#include <cstdint>

#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::core {

// ChunkedCowArray that accepts ANY int64 key.
//
// Keys in [0, kDenseKeyLimit) live in a ChunkedCowArray (dense columnar chunks,
// copy-on-write per chunk); keys outside that range (negative, or absurdly
// large - a directory slot per chunk would then cost gigabytes) live in a
// ShardedCowHashMap, so they are exact but only pay for what is present.
// Absent keys read as `fill` in both representations, and storing `fill` in
// the hash part removes the entry. Copy = directory + shard pointer copies;
// a write clones one chunk or one shard. Same threading rule as
// ChunkedCowArray (mutate only a private copy).
//
// kShift: slots per chunk is 2^kShift. Pick it so a chunk is ~64-256 KB and the
// directory (16 B per chunk) stays small at the largest expected key: for a
// 1-byte T use 16, for 4-byte T 14-15.
template <typename T, unsigned kShift = 12, unsigned kShardBits = 6>
class WideKeyCowArray {
public:
  static constexpr int64_t kDenseKeyLimit = int64_t{1} << 36;

  explicit WideKeyCowArray(T fill = T{}) : dense_(fill), fill_(fill) {}

  const T &fill() const { return fill_; }

  T Get(int64_t key) const {
    if (Dense(key))
      return dense_.Get(key);
    const T *p = sparse_.Find(key);
    return p ? *p : fill_;
  }
  // Writes only when the value changes, so an unchanged chunk is never cloned.
  void Set(int64_t key, const T &value) {
    if (Dense(key)) {
      if (!(dense_.Get(key) == value))
        dense_.Set(key, value);
      return;
    }
    if (value == fill_)
      sparse_.Erase(key);
    else
      sparse_.Upsert(key, value);
  }
  void Add(int64_t key, const T &by) { Set(key, Get(key) + by); }

  std::size_t MemoryBytes() const {
    return dense_.MemoryBytes() + sparse_.memory_bytes();
  }
  std::size_t SharedChunks() const { return dense_.SharedChunks(); }

private:
  static bool Dense(int64_t key) { return key >= 0 && key < kDenseKeyLimit; }

  ChunkedCowArray<T, kShift> dense_;
  ShardedCowHashMap<T, kShardBits> sparse_;
  T fill_;
};

} // namespace accelerator_codegen::aux_state::core
