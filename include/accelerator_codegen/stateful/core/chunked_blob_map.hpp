#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::core {

// ---------------------------------------------------------------------------
// Varint helpers (LEB128, 7 bits per byte) for compact record encodings.
// ---------------------------------------------------------------------------
inline void PutVarint(std::vector<uint8_t> &out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}
// Reads one varint at `p` (< end). Returns false on truncated / overlong input.
inline bool GetVarint(const uint8_t *&p, const uint8_t *end, uint64_t &v) {
  v = 0;
  for (unsigned shift = 0; p < end && shift < 64; shift += 7) {
    const uint8_t b = *p++;
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80))
      return true;
  }
  return false;
}
inline uint64_t ZigZagEncode(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
inline int64_t ZigZagDecode(uint64_t v) {
  return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}
inline void PutSignedVarint(std::vector<uint8_t> &out, int64_t v) {
  PutVarint(out, ZigZagEncode(v));
}
inline bool GetSignedVarint(const uint8_t *&p, const uint8_t *end, int64_t &v) {
  uint64_t u = 0;
  if (!GetVarint(p, end, u))
    return false;
  v = ZigZagDecode(u);
  return true;
}

// ---------------------------------------------------------------------------
// ChunkedBlobMap: copy-on-write sparse map  int64 key (>= 0)  ->  short byte
// string. The compact way to keep ONE variable-length record per key (a
// parent row plus the child rows that hang off it, an encoded list, ...)
// for 1e7..1e10 keys.
//
// Layout: keys are grouped 2^kKeyShift (1024) per chunk. A chunk is
//   * a presence bitmap (1024 bits = 128 B, so a sparse key space such as
//     "8 used keys of every 32" costs 4 bits per key of overhead, not a slot),
//   * a 4-byte arena offset per PRESENT key (key order, found by popcount rank),
//   * an append-only byte arena holding [varint length][bytes] per value.
// Cost per present key: 4 B offset + ~1 B length + payload (+ dead bytes <= 50%
// of the arena, compacted automatically).
//
// COW: chunks are shared_ptr; the directory is a ChunkedCowArray of chunk
// pointers (two-level: copying a map is O(#leaf directory pages), not
// O(#chunks)). Set/Erase clone only the touched chunk (and its directory page)
// if it is shared with another copy, then edit it in place: first touch of a
// chunk per version costs one chunk copy (~ a few KB-20 KB), later edits are
// in place (a bitmap bit, one offset-array insert, an arena append).
//   * const methods never allocate; concurrent readers of a published copy are
//     fine, mutate only a private copy (same rule as ChunkedCowArray).
//   * string_views returned by Get are valid until the next mutation of THE
//     SAME CHUNK in this map object (copy the bytes if you keep them).
//   * Any int64 key works. Keys in [0, 2^36) use the chunked layout above;
//     negative or larger keys (which would need an absurd directory) live in a
//     sharded COW hash map of immutable per-key byte strings (exact, but
//     ~60 B overhead per key: they are expected to be rare).
// ---------------------------------------------------------------------------
class ChunkedBlobMap {
public:
  static constexpr unsigned kKeyShift = 10;
  static constexpr std::size_t kChunkKeys = std::size_t{1} << kKeyShift;
  static constexpr std::size_t kMask = kChunkKeys - 1;

  struct Chunk {
    uint64_t present[kChunkKeys / 64] = {};
    std::vector<uint32_t> off;   // arena offset of each present key's record
    std::vector<uint8_t> arena;  // [varint len][bytes]...
    uint32_t dead = 0;           // arena bytes no longer referenced
  };

  ChunkedBlobMap() : dir_(nullptr) {}

  std::size_t size() const { return count_; }

  // Value of `key`; false when absent. See the view-lifetime note above.
  bool Get(int64_t key, std::string_view &out) const {
    if (!Dense(key)) {
      const auto *p = sparse_.Find(key);
      if (!p)
        return false;
      out = {reinterpret_cast<const char *>((*p)->data()), (*p)->size()};
      return true;
    }
    const Chunk *c = ChunkOf(key);
    if (!c)
      return false;
    const std::size_t slot = static_cast<std::size_t>(key) & kMask;
    if (!(c->present[slot >> 6] >> (slot & 63) & 1))
      return false;
    out = RecordAt(*c, c->off[Rank(*c, slot)]);
    return true;
  }
  bool Contains(int64_t key) const {
    if (!Dense(key))
      return sparse_.Find(key) != nullptr;
    const Chunk *c = ChunkOf(key);
    if (!c)
      return false;
    const std::size_t slot = static_cast<std::size_t>(key) & kMask;
    return (c->present[slot >> 6] >> (slot & 63)) & 1;
  }

  // Insert or replace. Always succeeds (returns true).
  bool Set(int64_t key, const uint8_t *data, std::size_t n) {
    if (!Dense(key)) {
      if (!sparse_.Find(key))
        ++count_;
      sparse_.Upsert(key, std::make_shared<const std::vector<uint8_t>>(data, data + n));
      return true;
    }
    Chunk &c = MutableChunk(key);
    const std::size_t slot = static_cast<std::size_t>(key) & kMask;
    const std::size_t rank = Rank(c, slot);
    const uint32_t at = static_cast<uint32_t>(c.arena.size());
    PutVarint(c.arena, n);
    c.arena.insert(c.arena.end(), data, data + n);
    if ((c.present[slot >> 6] >> (slot & 63)) & 1) {
      c.dead += RecordBytes(c, c.off[rank]);
      c.off[rank] = at;
    } else {
      c.present[slot >> 6] |= uint64_t{1} << (slot & 63);
      c.off.insert(c.off.begin() + static_cast<std::ptrdiff_t>(rank), at);
      ++count_;
    }
    MaybeCompact(c);
    return true;
  }
  bool Set(int64_t key, const std::vector<uint8_t> &bytes) {
    return Set(key, bytes.data(), bytes.size());
  }

  // Remove `key` (no-op when absent).
  void Erase(int64_t key) {
    if (!Contains(key))
      return;
    if (!Dense(key)) {
      sparse_.Erase(key);
      --count_;
      return;
    }
    Chunk &c = MutableChunk(key);
    const std::size_t slot = static_cast<std::size_t>(key) & kMask;
    const std::size_t rank = Rank(c, slot);
    c.dead += RecordBytes(c, c.off[rank]);
    c.off.erase(c.off.begin() + static_cast<std::ptrdiff_t>(rank));
    c.present[slot >> 6] &= ~(uint64_t{1} << (slot & 63));
    --count_;
    MaybeCompact(c);
  }

  // fn(key, std::string_view) for every key, ascending within a chunk.
  template <typename F> void ForEach(F &&fn) const {
    for (std::size_t ci = 0; ci < NumChunkSlots(); ++ci)
      ForEachInChunk(ci, fn);
    sparse_.ForEach([&](int64_t key, const std::shared_ptr<const std::vector<uint8_t>> &v) {
      fn(key, std::string_view(reinterpret_cast<const char *>(v->data()), v->size()));
    });
  }
  // Chunk-granular iteration (to split a scan across threads: chunk indices
  // [0, NumChunkSlots()) are independent; empty slots are skipped).
  std::size_t NumChunkSlots() const { return dir_.NumChunks() << kDirShift; }
  template <typename F> void ForEachInChunk(std::size_t ci, F &&fn) const {
    const Chunk *c = ChunkAtSlot(ci);
    if (!c)
      return;
    std::size_t rank = 0;
    for (std::size_t w = 0; w < kChunkKeys / 64; ++w) {
      uint64_t bits = c->present[w];
      while (bits) {
        const unsigned b = static_cast<unsigned>(__builtin_ctzll(bits));
        bits &= bits - 1;
        const int64_t key =
            static_cast<int64_t>((ci << kKeyShift) | (w << 6) | b);
        fn(key, RecordAt(*c, c->off[rank++]));
      }
    }
  }

  // Drop dead arena bytes and spare capacity (call once after a bulk build).
  void Shrink() {
    for (std::size_t ci = 0; ci < NumChunkSlots(); ++ci) {
      const auto *sp = SlotPtr(ci);
      if (!sp || !*sp || sp->use_count() > 1)
        continue;
      Chunk &c = **sp;
      if (c.dead)
        Compact(c);
      c.arena.shrink_to_fit();
      c.off.shrink_to_fit();
    }
  }

  // Bytes owned by this version (shared chunks counted in full).
  std::size_t MemoryBytes() const {
    std::size_t n = dir_.MemoryBytes() + sparse_.memory_bytes();
    for (std::size_t ci = 0; ci < NumChunkSlots(); ++ci)
      if (const Chunk *c = ChunkAtSlot(ci))
        n += sizeof(Chunk) + c->off.capacity() * 4 + c->arena.capacity();
    return n;
  }

private:
  // dir_ is a ChunkedCowArray<shared_ptr<Chunk>, kDirShift>: one page holds
  // 2^kDirShift chunk pointers (=1M keys).
  static constexpr unsigned kDirShift = 10;
  using Dir = ChunkedCowArray<std::shared_ptr<Chunk>, kDirShift>;

  const std::shared_ptr<Chunk> *SlotPtr(std::size_t ci) const {
    return dir_.Find(static_cast<int64_t>(ci));
  }
  const Chunk *ChunkAtSlot(std::size_t ci) const {
    const auto *sp = SlotPtr(ci);
    return sp ? sp->get() : nullptr;
  }
  const Chunk *ChunkOf(int64_t key) const {
    if (!Dense(key))
      return nullptr;
    return ChunkAtSlot(static_cast<std::size_t>(key) >> kKeyShift);
  }
  Chunk &MutableChunk(int64_t key) {
    auto &sp = dir_.Mutable(static_cast<int64_t>(static_cast<std::size_t>(key) >> kKeyShift));
    if (!sp)
      sp = std::make_shared<Chunk>();
    else if (sp.use_count() > 1)
      sp = std::make_shared<Chunk>(*sp);
    return *sp;
  }
  static std::size_t Rank(const Chunk &c, std::size_t slot) {
    std::size_t r = 0;
    const std::size_t w = slot >> 6;
    for (std::size_t i = 0; i < w; ++i)
      r += static_cast<std::size_t>(__builtin_popcountll(c.present[i]));
    const uint64_t below = (slot & 63) ? (c.present[w] & ((uint64_t{1} << (slot & 63)) - 1)) : 0;
    return r + static_cast<std::size_t>(__builtin_popcountll(below));
  }
  static std::string_view RecordAt(const Chunk &c, uint32_t at) {
    const uint8_t *p = c.arena.data() + at;
    const uint8_t *end = c.arena.data() + c.arena.size();
    uint64_t len = 0;
    GetVarint(p, end, len);
    return {reinterpret_cast<const char *>(p), static_cast<std::size_t>(len)};
  }
  static uint32_t RecordBytes(const Chunk &c, uint32_t at) {
    const uint8_t *p = c.arena.data() + at;
    const uint8_t *end = c.arena.data() + c.arena.size();
    uint64_t len = 0;
    GetVarint(p, end, len);
    return static_cast<uint32_t>((p - (c.arena.data() + at)) + len);
  }
  static void Compact(Chunk &c) {
    std::vector<uint8_t> fresh;
    fresh.reserve(c.arena.size() - c.dead);
    for (auto &o : c.off) {
      const uint32_t at = o;
      const uint32_t n = RecordBytes(c, at);
      o = static_cast<uint32_t>(fresh.size());
      fresh.insert(fresh.end(), c.arena.begin() + at, c.arena.begin() + at + n);
    }
    c.arena = std::move(fresh);
    c.dead = 0;
  }
  static void MaybeCompact(Chunk &c) {
    if (c.dead > 256 && c.dead * 2 > c.arena.size())
      Compact(c);
  }

  static constexpr int64_t kDenseKeyLimit = int64_t{1} << 36;
  static bool Dense(int64_t key) { return key >= 0 && key < kDenseKeyLimit; }

  Dir dir_;
  ShardedCowHashMap<std::shared_ptr<const std::vector<uint8_t>>, 6> sparse_;
  std::size_t count_ = 0;
};

} // namespace accelerator_codegen::aux_state::core
