#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Copy-on-write chunked array indexed by a non-negative int64.
//
// Layout: a directory of shared_ptr<Chunk>, one entry per 2^kShift slots. A null
// entry is an unallocated chunk and reads as `fill` (so the same type serves as
// a dense array AND as a sparse id->value map with a sentinel value).
//
//   * Copy-construct / assign: O(#directory entries) pointer copies; chunk
//     payload is shared between the copies. Use this to "copy" an immutable
//     published state per write batch.
//   * Set / Mutable: clone ONLY the touched chunk if it is shared
//     (use_count() > 1), else write in place. Cost O(2^kShift * sizeof(T)) once per
//     touched chunk per version, O(1) afterwards.
//   * Const reads never allocate or copy.
//   * Versions are independent: mutating one never changes what another sees.
//     (Thread safety: concurrent const reads of a published copy are fine;
//     mutate only a private copy. use_count() is only a hint under races, so do
//     not mutate a copy while another thread copies *that same object*.)
//
// kShift = 12 (4096 slots) by default: a 1-byte payload chunk is 4 KB, a
// directory entry is 16 B per 4096 slots. Pick a bigger shift for tiny T.
// Any id in [0, INT64_MAX] works; Set fails only for negative ids.
// "DenseIdCow" below is the id-keyed alias: just use ChunkedCowArray<T>.
template <typename T, unsigned kShift = 12> class ChunkedCowArray {
public:
  static constexpr std::size_t kChunkSize = std::size_t{1} << kShift;
  static constexpr std::size_t kMask = kChunkSize - 1;
  using Chunk = std::vector<T>;

  explicit ChunkedCowArray(T fill = T{}) : fill_(fill) {}

  static bool Fits(int64_t key) { return key >= 0; }
  const T &fill() const { return fill_; }

  // Read; never allocates. Out-of-range / negative / unallocated read as fill.
  T Get(int64_t key) const { return Get(key, fill_); }
  T Get(int64_t key, const T &absent) const {
    const T *p = Find(key);
    return p ? *p : absent;
  }
  // nullptr when the chunk is unallocated or key negative. The slot may still
  // hold `fill`.
  const T *Find(int64_t key) const {
    if (key < 0)
      return nullptr;
    const auto id = static_cast<std::size_t>(key);
    const auto ci = id >> kShift;
    if (ci >= dir_.size() || !dir_[ci])
      return nullptr;
    return &(*dir_[ci])[id & kMask];
  }
  bool Has(int64_t key, const T &absent) const {
    const T *p = Find(key);
    return p && !(*p == absent);
  }

  // Writable reference (allocates / clones the chunk as needed). key must be >= 0.
  T &Mutable(int64_t key) {
    const auto id = static_cast<std::size_t>(key);
    const auto ci = id >> kShift;
    if (dir_.size() <= ci)
      dir_.resize(ci + 1);
    auto &c = dir_[ci];
    if (!c)
      c = std::make_shared<Chunk>(kChunkSize, fill_);
    else if (c.use_count() > 1)
      c = std::make_shared<Chunk>(*c);
    return (*c)[id & kMask];
  }
  bool Set(int64_t key, const T &value) {
    if (key < 0)
      return false;
    Mutable(key) = value;
    return true;
  }

  void Clear() { dir_.clear(); }

  // Directory introspection (iteration / diagnostics).
  std::size_t NumChunks() const { return dir_.size(); }
  const Chunk *ChunkAt(std::size_t ci) const {
    return ci < dir_.size() ? dir_[ci].get() : nullptr;
  }
  static constexpr int64_t ChunkBase(std::size_t ci) {
    return static_cast<int64_t>(ci << kShift);
  }
  std::size_t AllocatedChunks() const {
    std::size_t n = 0;
    for (const auto &c : dir_)
      n += c != nullptr;
    return n;
  }
  // Chunks whose payload is shared with at least one other copy.
  std::size_t SharedChunks() const {
    std::size_t n = 0;
    for (const auto &c : dir_)
      n += c && c.use_count() > 1;
    return n;
  }
  // True when chunk ci of *this and other is the same physical payload.
  bool SharesChunkWith(const ChunkedCowArray &other, std::size_t ci) const {
    return ci < dir_.size() && ci < other.dir_.size() && dir_[ci] &&
           dir_[ci] == other.dir_[ci];
  }
  // Bytes owned by this version (shared chunks counted in full).
  std::size_t MemoryBytes() const {
    return dir_.capacity() * sizeof(dir_[0]) + AllocatedChunks() * kChunkSize * sizeof(T);
  }

private:
  std::vector<std::shared_ptr<Chunk>> dir_;
  T fill_;
};

} // namespace accelerator_codegen::aux_state::core
