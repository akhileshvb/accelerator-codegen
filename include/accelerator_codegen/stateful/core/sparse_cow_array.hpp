#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Copy-on-write chunked array indexed by ANY int64 (negative keys included),
// with a SPARSE chunk directory.
//
// Like ChunkedCowArray, but the directory is a sorted vector of
// (chunk id, shared_ptr<chunk>) for the chunks that exist, instead of a
// vector with one slot per 2^kShift ids. So a key space that is huge or sparse
// (ids around 2^40, or a handful of far-apart keys) costs memory only for the
// chunks that were touched, and chunks are visited in ascending key order.
//
//   * Copy: O(#allocated chunks) pointer copies; payload is shared.
//   * Mutable / Set: clone only the touched chunk if it is shared, else write
//     in place. Locating a chunk is a binary search (with a one-entry hint, so
//     runs of nearby keys cost O(1)).
//   * Const reads never allocate. Unallocated chunks read as `fill`.
//   * Same threading rule as ChunkedCowArray: concurrent const reads of a
//     published copy are fine; mutate only a private copy.
template <typename T, unsigned kShift = 12> class SparseCowArray {
public:
  static constexpr std::size_t kChunkSize = std::size_t{1} << kShift;
  static constexpr std::size_t kMask = kChunkSize - 1;
  using Chunk = std::vector<T>;

  explicit SparseCowArray(T fill = T{}) : fill_(fill) {}

  const T &fill() const { return fill_; }

  // Order-preserving unsigned image of a key (flips the sign bit).
  static uint64_t Ordinal(int64_t key) {
    return static_cast<uint64_t>(key) ^ (uint64_t{1} << 63);
  }
  static int64_t KeyOfOrdinal(uint64_t ord) {
    return static_cast<int64_t>(ord ^ (uint64_t{1} << 63));
  }
  static uint64_t ChunkIdOf(int64_t key) { return Ordinal(key) >> kShift; }

  const T *Find(int64_t key) const {
    const Chunk *c = FindChunk(ChunkIdOf(key));
    return c ? &(*c)[Ordinal(key) & kMask] : nullptr;
  }
  T Get(int64_t key) const {
    const T *p = Find(key);
    return p ? *p : fill_;
  }

  T &Mutable(int64_t key) {
    const uint64_t cid = ChunkIdOf(key);
    std::size_t pos = hint_;
    if (pos >= dir_.size() || dir_[pos].id != cid) {
      auto it = std::lower_bound(
          dir_.begin(), dir_.end(), cid,
          [](const Slot &s, uint64_t id) { return s.id < id; });
      pos = static_cast<std::size_t>(it - dir_.begin());
      if (it == dir_.end() || it->id != cid) {
        dir_.insert(it, Slot{cid, std::make_shared<Chunk>(kChunkSize, fill_)});
      }
      hint_ = pos;
    }
    auto &c = dir_[pos].chunk;
    if (c.use_count() > 1)
      c = std::make_shared<Chunk>(*c);
    return (*c)[Ordinal(key) & kMask];
  }
  void Set(int64_t key, const T &value) { Mutable(key) = value; }

  // Directory introspection: chunks in ascending key order.
  std::size_t NumChunks() const { return dir_.size(); }
  uint64_t ChunkIdAt(std::size_t i) const { return dir_[i].id; }
  const Chunk *ChunkAt(std::size_t i) const { return dir_[i].chunk.get(); }
  // Key of slot 0 of the chunk with id `cid`.
  static int64_t ChunkBase(uint64_t cid) {
    return KeyOfOrdinal(cid << kShift);
  }
  const Chunk *FindChunk(uint64_t cid) const {
    auto it = std::lower_bound(
        dir_.begin(), dir_.end(), cid,
        [](const Slot &s, uint64_t id) { return s.id < id; });
    return it != dir_.end() && it->id == cid ? it->chunk.get() : nullptr;
  }

  std::size_t SharedChunks() const {
    std::size_t n = 0;
    for (const auto &s : dir_)
      n += s.chunk.use_count() > 1;
    return n;
  }
  std::size_t MemoryBytes() const {
    return dir_.capacity() * sizeof(Slot) + dir_.size() * kChunkSize * sizeof(T);
  }

private:
  struct Slot {
    uint64_t id;
    std::shared_ptr<Chunk> chunk;
  };
  std::vector<Slot> dir_;
  std::size_t hint_ = 0;
  T fill_;
};

} // namespace accelerator_codegen::aux_state::core
