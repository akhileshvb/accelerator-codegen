#pragma once
//
// DenseIdArray<T>
// PURPOSE    Plain (NOT copy-on-write) chunked dense array indexed by a non-negative
//            int64, 64K slots per leaf, for single-owner build-time or per-query
//            scratch columns.
// COMPLEXITY Get/Set O(1); Ensure may grow a leaf.
// MEMORY     sizeof(T) per slot of touched leaves + 24 B of directory per 64K ids.
// COW/THREADS No sharing: copying duplicates every leaf. Not thread-safe for writes;
//            concurrent const reads are fine.
// USE WHEN   the structure is never published as a shared version; otherwise use
//            ChunkedCowArray / AnyKeyCowArray.
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Chunked dense array indexed by a non-negative int64 id.
// Missing keys read as `absent`.
//
// Two-level layout: a growable top-level directory (`chunks`, one slot per 64K
// ids, grows with max(id)/64K) of lazily-filled leaf chunks. A leaf only
// allocates (and grows, up to 64K slots) when a key inside it is touched, so a
// sparse key space costs ~sizeof(T) per *spanned slot* of touched chunks plus
// 24 B of directory per 64K ids (e.g. 6e9 ids = ~2.2 MB of directory; keys where
// 1 in 4 is used cost ~1 B/slot = 4 B per used key for uint8 values).
// Any id in [0, INT64_MAX] is accepted; Set fails only for negative ids.
// Caller must still not pass garbage ids: an id of 1e13 needs a ~3.7 GB directory.
//
// Occupancy: slots that were never set read as `fill` (Ensure/Set) or `absent`
// (Get). Use a sentinel T value (e.g. fill = T{kAbsent}) and Has()/Find() when
// the value domain has a free value; no per-slot occupancy bit is kept.
// Chunk contents are plain std::vector<T> (iterate `chunks` with
// `base = chunk_idx << kShift`).
template <typename T>
struct DenseIdArray {
  static constexpr std::size_t kShift = 16;
  static constexpr std::size_t kSize = 1u << kShift;
  static constexpr std::size_t kMask = kSize - 1;
  static constexpr int64_t kMaxId = INT64_MAX;

  std::vector<std::vector<T>> chunks;

  static bool Fits(int64_t key) { return key >= 0; }

  void Clear() { chunks.clear(); }

  std::size_t SlotCount() const {
    std::size_t n = 0;
    for (const auto &chunk : chunks)
      n += chunk.size();
    return n;
  }

  bool Ensure(int64_t key, T fill = T{}) {
    if (!Fits(key))
      return false;
    const auto id = static_cast<std::size_t>(key);
    const auto chunk_idx = id >> kShift;
    const auto slot = id & kMask;
    if (chunks.size() <= chunk_idx)
      chunks.resize(chunk_idx + 1);
    auto &chunk = chunks[chunk_idx];
    if (chunk.size() <= slot) {
      // Round capacity up to 4096-slot steps (amortized, <=4K slack per chunk)
      // instead of vector doubling, which would waste up to 2x on sparse data.
      if (chunk.capacity() < slot + 1)
        chunk.reserve(std::min(kSize, (slot + 1 + 4095) & ~std::size_t{4095}));
      chunk.resize(slot + 1, fill);
    }
    return true;
  }

  // Allocates every leaf up to `max_key` (each filled with `fill`), so that threads
  // may then Set distinct in-range keys concurrently. `parallel_for(n, fn)` must run
  // fn(0..n-1), possibly on several threads; each call owns one leaf.
  template <class ParallelFor>
  void Allocate(int64_t max_key, ParallelFor &&parallel_for, T fill = T{}) {
    if (!Fits(max_key))
      return;
    const auto last = static_cast<std::size_t>(max_key);
    const std::size_t leaves = (last >> kShift) + 1;
    if (chunks.size() < leaves)
      chunks.resize(leaves);
    parallel_for(leaves, [&](std::size_t leaf) {
      const std::size_t size = leaf + 1 == leaves ? (last & kMask) + 1 : kSize;
      if (chunks[leaf].size() < size)
        chunks[leaf].resize(size, fill);
    });
  }

  T Get(int64_t key, T absent = T{}) const {
    if (!Fits(key))
      return absent;
    const auto id = static_cast<std::size_t>(key);
    const auto chunk_idx = id >> kShift;
    if (chunk_idx >= chunks.size())
      return absent;
    const auto &chunk = chunks[chunk_idx];
    const auto slot = id & kMask;
    if (slot >= chunk.size())
      return absent;
    return chunk[slot];
  }

  // Pointer to the slot if its chunk is allocated and large enough, else nullptr.
  // (A non-null pointer does not imply "set": compare with your sentinel.)
  const T *Find(int64_t key) const {
    if (!Fits(key))
      return nullptr;
    const auto id = static_cast<std::size_t>(key);
    const auto chunk_idx = id >> kShift;
    if (chunk_idx >= chunks.size())
      return nullptr;
    const auto &chunk = chunks[chunk_idx];
    const auto slot = id & kMask;
    return slot < chunk.size() ? &chunk[slot] : nullptr;
  }
  T *Find(int64_t key) {
    return const_cast<T *>(static_cast<const DenseIdArray *>(this)->Find(key));
  }
  // True when the slot exists and differs from `absent`.
  bool Has(int64_t key, const T &absent = T{}) const {
    const T *p = Find(key);
    return p && !(*p == absent);
  }

  std::size_t MemoryBytes() const {
    std::size_t n = chunks.capacity() * sizeof(chunks[0]);
    for (const auto &chunk : chunks)
      n += chunk.capacity() * sizeof(T);
    return n;
  }

  bool Set(int64_t key, T value, T fill = T{}) {
    if (!Ensure(key, fill))
      return false;
    const auto id = static_cast<std::size_t>(key);
    chunks[id >> kShift][id & kMask] = value;
    return true;
  }
};

} // namespace accelerator_codegen::aux_state::core
