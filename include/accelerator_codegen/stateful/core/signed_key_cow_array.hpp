#pragma once

#include <cstddef>
#include <cstdint>

#include <string>
#include <string_view>

#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/cow_blob_column.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::core {

// ChunkedCowArray for ANY int64 key, with a dense, chunk-scannable layout for
// small keys of both signs.
//
// Keys in [-2^35, 2^35) are zigzag-mapped (k >= 0 -> 2k, k < 0 -> -2k-1) onto
// the dense ChunkedCowArray, so positive and negative keys share chunks and a
// chunk scan can recover the key of every slot (KeyAt). Keys outside that
// range live in a ShardedCowHashMap (exact, pay-per-entry). Absent keys read
// as `fill`. Same copy-on-write / threading rules as ChunkedCowArray.
template <typename T, unsigned kShift = 12, unsigned kShardBits = 6>
class SignedKeyCowArray {
public:
  using Chunk = typename ChunkedCowArray<T, kShift>::Chunk;
  static constexpr std::size_t kChunkSize = ChunkedCowArray<T, kShift>::kChunkSize;
  static constexpr int64_t kDenseLimit = int64_t{1} << 36; // zigzag ids

  explicit SignedKeyCowArray(T fill = T{}) : dense_(fill), fill_(fill) {}

  const T &fill() const { return fill_; }

  static uint64_t Zig(int64_t k) {
    return k >= 0 ? static_cast<uint64_t>(k) << 1
                  : (~static_cast<uint64_t>(k) << 1) | 1;
  }
  static int64_t Unzig(uint64_t id) {
    return (id & 1) ? ~static_cast<int64_t>(id >> 1)
                    : static_cast<int64_t>(id >> 1);
  }
  static bool IsDense(int64_t k) {
    return k >= -(kDenseLimit >> 1) && k < (kDenseLimit >> 1);
  }

  T Get(int64_t key) const { return Get(key, fill_); }
  T Get(int64_t key, const T &absent) const {
    const T *p = Find(key);
    return p ? *p : absent;
  }
  // nullptr when never allocated (the slot may still hold `fill`).
  const T *Find(int64_t key) const {
    if (IsDense(key))
      return dense_.Find(static_cast<int64_t>(Zig(key)));
    return sparse_.Find(key);
  }
  bool Has(int64_t key, const T &absent) const {
    const T *p = Find(key);
    return p && !(*p == absent);
  }
  T &Mutable(int64_t key) {
    if (IsDense(key))
      return dense_.Mutable(static_cast<int64_t>(Zig(key)));
    if (!sparse_.Find(key))
      sparse_.Upsert(key, fill_);
    return sparse_[key];
  }
  void Set(int64_t key, const T &value) {
    if (IsDense(key)) {
      dense_.Mutable(static_cast<int64_t>(Zig(key))) = value;
      return;
    }
    sparse_.Upsert(key, value);
  }

  // Dense directory (chunk scans). Key of slot `i` in chunk `ci`:
  // KeyAt(ci, i). Entries outside the dense range: ForEachSparse.
  std::size_t NumChunks() const { return dense_.NumChunks(); }
  const Chunk *ChunkAt(std::size_t ci) const { return dense_.ChunkAt(ci); }
  static int64_t KeyAt(std::size_t ci, std::size_t i) {
    return Unzig((static_cast<uint64_t>(ci) << kShift) | i);
  }
  template <typename F> void ForEachSparse(F &&fn) const {
    sparse_.ForEach(fn);
  }

  std::size_t MemoryBytes() const {
    return dense_.MemoryBytes() + sparse_.memory_bytes();
  }

private:
  ChunkedCowArray<T, kShift> dense_;
  ShardedCowHashMap<T, kShardBits> sparse_;
  T fill_;
};

// ChunkedCowBlobColumn for any int64 key (same dense zigzag / sparse split).
template <unsigned kShift = 12, unsigned kShardBits = 6>
class SignedKeyBlobColumn {
public:
  using Zig = SignedKeyCowArray<uint8_t, kShift, kShardBits>;

  bool Get(int64_t key, std::string_view &out) const {
    if (Zig::IsDense(key))
      return dense_.Get(static_cast<int64_t>(Zig::Zig(key)), out);
    const std::string *p = sparse_.Find(key);
    if (!p)
      return false;
    out = *p;
    return true;
  }
  bool Has(int64_t key) const {
    std::string_view unused;
    return Get(key, unused);
  }
  void Set(int64_t key, std::string_view value) {
    if (Zig::IsDense(key))
      dense_.Set(static_cast<int64_t>(Zig::Zig(key)), value);
    else
      sparse_.Upsert(key, std::string(value));
  }
  void Erase(int64_t key) {
    if (Zig::IsDense(key))
      dense_.Erase(static_cast<int64_t>(Zig::Zig(key)));
    else
      sparse_.Erase(key);
  }
  std::size_t MemoryBytes() const {
    return dense_.MemoryBytes() + sparse_.memory_bytes();
  }

private:
  ChunkedCowBlobColumn<kShift> dense_;
  ShardedCowHashMap<std::string, kShardBits> sparse_;
};

} // namespace accelerator_codegen::aux_state::core
