#pragma once

// AnyKeyCowArray<T, kShift, kShardBits>: copy-on-write array indexed by ANY int64.
//
// PURPOSE   Default per-key column (int64 key -> fixed-size value) for state that
//           is published as immutable versions and edited on a private copy.
//           Negative keys, huge keys and sparse key spaces are all exact.
// LAYOUT    Keys in [-2^(b-1), 2^(b-1)) with b = min(36, kShift+24) (default kShift
//           12: +-2^35; kShift 6: +-2^29) are zigzag-mapped (k >= 0 -> 2k, k < 0 -> -2k-1)
//           to an id, so small keys of both signs share the first chunks. Ids are
//           grouped 2^kShift per chunk (std::vector<T>, allocated on first
//           write); chunks hang off a two-level directory (pages of 256 chunk
//           pointers), so the directory costs memory only for populated pages.
//           Keys outside the dense range live in a ShardedCowHashMap
//           (~pay-per-entry, tens of bytes each): exact but meant to be rare.
//           Absent keys read as `fill`.
// COMPLEXITY Get/Find: O(1) (directory page, chunk pointer, slot). Set/Mutable:
//           O(1) plus, once per touched chunk per version, a clone of that chunk
//           and its 256-pointer directory page. Copy: O(#directory pages + 64).
// MEMORY    sizeof(T) per slot of every touched chunk (2^kShift slots), plus
//           16 B per chunk pointer and a small per-page overhead; ~60 B per far
//           key in the hash part. Pick kShift so a chunk is ~4-64 KB for dense
//           columns, or smaller (5-8) when only a few slots per chunk are used.
// COW/THREADS Copy-construct/assign shares chunks; writes clone only the touched
//           chunk/page. Concurrent const reads of a published copy are fine;
//           mutate a private copy only (use_count is a hint under races).
// ITERATION ForEachChunk(fn(ci, chunk)) visits allocated dense chunks (cost
//           independent of key spread); KeyAt(ci,i) gives a slot's key.
//           NumChunks()/ChunkAt(ci) is the indexed form (O(max chunk index)
//           to sweep, prefer ForEachChunk for sparse key spaces); ForEachSparse visits the hash part;
//           ForEach visits both; ForEachOrdered visits all slots of allocated
//           chunks in ascending key order. Slots equal to `fill` may be visited.
// DELETION  Set(key, fill) restores "absent" (and removes a far key's hash
//           entry); dense chunks are not freed. Set() skips identical writes when
//           T is ==-comparable, so a no-op update never clones a chunk.
// USE WHEN  keyed by an integer id of unknown sign/range and the value is a
//           small trivially-copyable-ish type. For bounded non-negative ids use
//           ChunkedCowArray directly; for variable-length values use
//           ChunkedBlobMap; for hash-style point lookups of fat records use
//           ShardedCowHashMap / ShardedCowRecordStore.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "prims/chunked_cow_array.hpp"
#include "prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::core {

namespace any_key_detail {
template <typename T, typename = void> struct HasEq : std::false_type {};
template <typename T>
struct HasEq<T, std::void_t<decltype(static_cast<bool>(std::declval<const T &>() ==
                                                      std::declval<const T &>()))>>
    : std::true_type {};
} // namespace any_key_detail

template <typename T, unsigned kShift = 12, unsigned kShardBits = 6>
class AnyKeyCowArray {
public:
  using Chunk = std::vector<T>;
  static constexpr std::size_t kChunkSize = std::size_t{1} << kShift;
  // Dense zigzag ids are < 2^kDenseBits: at most 2^36, and at most 2^(kShift+24)
  // so the top directory level never exceeds 2^16 entries (1 MB per copy).
  static constexpr unsigned kDenseBits = kShift + 24 < 36 ? kShift + 24 : 36;
  static constexpr int64_t kDenseLimit = int64_t{1} << kDenseBits; // zigzag ids

  explicit AnyKeyCowArray(T fill = T{}) : dir_(nullptr), fill_(std::move(fill)) {}

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
  // nullptr when never allocated (an allocated slot may still hold `fill`).
  const T *Find(int64_t key) const {
    if (!IsDense(key))
      return sparse_.Find(key);
    const uint64_t id = Zig(key);
    const Chunk *c = ChunkAt(static_cast<std::size_t>(id >> kShift));
    return c ? &(*c)[id & (kChunkSize - 1)] : nullptr;
  }
  bool Has(int64_t key, const T &absent) const {
    const T *p = Find(key);
    return p && !(*p == absent);
  }

  // Writable reference (allocates / clones as needed). The reference is
  // invalidated by the next write to the same chunk or hash shard.
  T &Mutable(int64_t key) {
    if (!IsDense(key)) {
      if (!sparse_.Find(key))
        sparse_.Upsert(key, fill_);
      return sparse_[key];
    }
    const uint64_t id = Zig(key);
    auto &slot = dir_.Mutable(static_cast<int64_t>(id >> kShift));
    if (!slot)
      slot = std::make_shared<Chunk>(kChunkSize, fill_);
    else if (slot.use_count() > 1)
      slot = std::make_shared<Chunk>(*slot);
    return (*slot)[id & (kChunkSize - 1)];
  }
  void Set(int64_t key, const T &value) {
    if constexpr (any_key_detail::HasEq<T>::value) {
      if (const T *p = Find(key); p ? *p == value : value == fill_)
        return; // no-op write: never clone a chunk for it
      if (!IsDense(key) && value == fill_) {
        sparse_.Erase(key);
        return;
      }
    }
    Mutable(key) = value;
  }
  void Add(int64_t key, const T &by) { Set(key, Get(key) + by); }

  // ---- iteration -----------------------------------------------------------
  // Dense directory in zigzag order: chunk `ci` holds keys KeyAt(ci, 0..).
  // ChunkAt is nullptr for unallocated chunks (also when ci >= NumChunks()).
  std::size_t NumChunks() const { return dir_.NumChunks() << kPageShift; }
  const Chunk *ChunkAt(std::size_t ci) const {
    const auto *p = dir_.Find(static_cast<int64_t>(ci));
    return p ? p->get() : nullptr;
  }
  static int64_t KeyAt(std::size_t ci, std::size_t i) {
    return Unzig((static_cast<uint64_t>(ci) << kShift) | i);
  }
  // fn(ci, const Chunk&) for every allocated chunk in ascending ci; skips
  // unallocated directory pages in O(1) each, so cost does not depend on how
  // far apart the populated keys are.
  template <typename F> void ForEachChunk(F &&fn) const {
    for (std::size_t p = 0, np = dir_.NumChunks(); p < np; ++p) {
      const auto *page = dir_.ChunkAt(p);
      if (!page)
        continue;
      for (std::size_t j = 0; j < page->size(); ++j)
        if ((*page)[j])
          fn((p << kPageShift) | j, *(*page)[j]);
    }
  }
  // fn(key, const T&) for entries outside the dense range (unordered).
  template <typename F> void ForEachSparse(F &&fn) const {
    sparse_.ForEach(fn);
  }
  // fn(key, const T&): dense slots in zigzag order, then the sparse part.
  template <typename F> void ForEach(F &&fn) const {
    ForEachChunk([&](std::size_t ci, const Chunk &c) {
      for (std::size_t i = 0; i < kChunkSize; ++i)
        fn(KeyAt(ci, i), c[i]);
    });
    sparse_.ForEach(fn);
  }
  // fn(key, const T&) in ascending key order over every slot of every
  // allocated chunk plus every sparse entry. O(slots + far log far).
  template <typename F> void ForEachOrdered(F &&fn) const {
    std::vector<std::pair<int64_t, const T *>> far;
    sparse_.ForEach([&](int64_t k, const T &v) { far.emplace_back(k, &v); });
    std::sort(far.begin(), far.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    std::size_t f = 0;
    for (; f < far.size() && far[f].first < 0; ++f)
      fn(far[f].first, *far[f].second);
    std::vector<std::pair<std::size_t, const Chunk *>> chunks;
    ForEachChunk([&](std::size_t ci, const Chunk &c) { chunks.emplace_back(ci, &c); });
    for (std::size_t n = chunks.size(); n-- > 0;) // negatives: odd ids, descending
      for (std::size_t i = kChunkSize; i-- > 0;)
        if (i & 1)
          fn(KeyAt(chunks[n].first, i), (*chunks[n].second)[i]);
    for (const auto &e : chunks) // non-negatives: even ids, ascending
      for (std::size_t i = 0; i < kChunkSize; i += 2)
        fn(KeyAt(e.first, i), (*e.second)[i]);
    for (; f < far.size(); ++f)
      fn(far[f].first, *far[f].second);
  }

  std::size_t AllocatedChunks() const {
    std::size_t n = 0;
    ForEachChunk([&](std::size_t, const Chunk &) { ++n; });
    return n;
  }
  // Chunks whose payload is shared with at least one other copy.
  std::size_t SharedChunks() const {
    std::size_t n = 0;
    for (std::size_t p = 0, np = dir_.NumChunks(); p < np; ++p)
      if (const auto *page = dir_.ChunkAt(p)) {
        const bool page_shared = dir_.IsChunkShared(p); // then every chunk is
        for (const auto &c : *page)
          n += c && (page_shared || c.use_count() > 1);
      }
    return n;
  }
  // Bytes owned by this version (shared chunks counted in full).
  std::size_t MemoryBytes() const {
    return dir_.MemoryBytes() + AllocatedChunks() * kChunkSize * sizeof(T) +
           sparse_.memory_bytes();
  }

private:
  static constexpr unsigned kPageShift = 8; // chunk pointers per directory page
  ChunkedCowArray<std::shared_ptr<Chunk>, kPageShift> dir_;
  ShardedCowHashMap<T, kShardBits> sparse_;
  T fill_;
};

} // namespace accelerator_codegen::aux_state::core
