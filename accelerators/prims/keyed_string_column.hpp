#pragma once
//
// KeyedStringColumn
// PURPOSE    Copy-on-write column of strings keyed by int64 that can be scanned with a
//            SQL LIKE matcher (multi-threaded, SIMD substring search).
// COMPLEXITY Get O(1). Set/Erase of an existing key rewrite its 4096-key segment
//            (O(segment bytes)); AppendNew is O(1) bulk load. ForEachMatch O(total
//            bytes / threads).
// MEMORY     ~12 B per key + string bytes (StringBlob + uint16 maps); 2 B per absent
//            slot of a touched segment.
// COW/THREADS Copy = pointer copies; mutate a private copy only. ForEachMatch runs
//            worker threads over a const column; the callback must be thread-safe or
//            worker-local.
// USE WHEN   you must evaluate LIKE / pattern predicates over a whole string column.
//            For plain point lookups of strings use ChunkedBlobMap.
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "prims/chunked_cow_array.hpp"
#include "prims/flat_hash_map.hpp"
#include "prims/parallelism.hpp"
#include "prims/sql_like.hpp"
#include "prims/string_blob.hpp"

namespace accelerator_codegen::aux_state::core {

// A string column keyed by a non-negative int64 id (a "name" per primary key),
// stored as copy-on-write segments of 2^12 ids and scannable with a LIKE
// matcher.
//
//   * Segment = StringBlob (uint64 offsets, so segments of any byte size work)
//     + uint16 id->slot and slot->id maps: ~12 B/key + string bytes, no per-key
//     allocation. Absent keys cost 2 B in their segment's slot table only.
//   * Copy of the column = pointer copies; Set/Erase clone only the touched
//     segment (O(segment bytes)); const reads never allocate.
//   * ForEachMatch runs sql_like::MatchBlobBitmap per segment on a pool of
//     worker threads (each takes a contiguous share of the segments), O(total string bytes)
//     / threads with SIMD substring search for `%x%` shapes.
//   * AppendNew is the O(1) bulk-load path for keys known to be absent (loading
//     a primary-keyed table); it writes in place into an unshared segment.
// Threading: same rule as ChunkedCowArray (mutate a private copy only).
class KeyedStringColumn {
public:
  static constexpr unsigned kSegShift = 12;
  static constexpr std::size_t kSegKeys = std::size_t{1} << kSegShift;
  static constexpr uint16_t kNoSlot = 0xFFFF;

  struct Segment {
    StringBlob strings;               // append order
    std::vector<uint16_t> key_slot;   // per string id: key & (kSegKeys-1)
    std::vector<uint16_t> id_of_slot; // slot -> string id (kNoSlot if absent)
    uint32_t live = 0;
  };

  KeyedStringColumn() : segs_(nullptr) {}
  // Any int64 key works: segments of keys in [0, 2^36) live in a chunked array,
  // the (negative or huge) rest in a sharded COW hash keyed by segment index.

  std::size_t size() const { return size_; }

  bool Get(int64_t key, std::string_view &out) const {
    const Segment *s = SegmentOf(key);
    if (!s)
      return false;
    const uint16_t id = s->id_of_slot[Slot(key)];
    if (id == kNoSlot)
      return false;
    out = s->strings.View(id);
    return true;
  }
  bool Contains(int64_t key) const {
    std::string_view ignored;
    return Get(key, ignored);
  }

  // Bulk load: `key` must not be present yet (e.g. rows of a primary-keyed
  // table). Only valid while the touched segment is unshared (a fresh column).
  void AppendNew(int64_t key, std::string_view value) {
    Segment &s = MutableSegment(key);
    s.id_of_slot[Slot(key)] = static_cast<uint16_t>(s.strings.size());
    s.key_slot.push_back(static_cast<uint16_t>(Slot(key)));
    s.strings.Append(value);
    ++s.live;
    ++size_;
  }

  // Insert or replace. Replacing rewrites the segment (strings are append-only).
  void Set(int64_t key, std::string_view value) {
    const Segment *old = SegmentOf(key);
    if (old && old->id_of_slot[Slot(key)] != kNoSlot) {
      Rewrite(key, &value);
      return;
    }
    AppendNew(key, value);
  }
  bool Erase(int64_t key) {
    const Segment *old = SegmentOf(key);
    if (!old || old->id_of_slot[Slot(key)] == kNoSlot)
      return false;
    Rewrite(key, nullptr);
    return true;
  }

  // Calls fn(worker, key) for every key whose string matches. fn runs on up to
  // `par.workers` workers of par's pool concurrently (worker in [0, workers));
  // it must only touch worker-local state or be thread-safe. Returns the
  // worker count used.
  template <typename Fn>
  unsigned ForEachMatch(const sql_like::LikeMatcher &matcher, const Parallelism &par,
                        Fn &&fn) const {
    std::vector<std::pair<int64_t, const Segment *>> work;
    for (std::size_t si = 0; si < segs_.NumChunks() * SegArray::kChunkSize; ++si)
      if (const auto *sp = segs_.Find(static_cast<int64_t>(si)); sp && *sp && (*sp)->live)
        work.emplace_back(static_cast<int64_t>(si), sp->get());
    other_.ForEach([&](int64_t si, const std::shared_ptr<Segment> &sp) {
      if (sp && sp->live)
        work.emplace_back(si, sp.get());
    });
    // Callers keep per-worker state indexed by `worker`, and the pool does not
    // say which thread runs a task, so a task is a worker's contiguous share of
    // the segments (deterministic, no shared cursor).
    const std::size_t nseg = work.size();
    const unsigned workers = (par.workers > 1 && size_ >= 8 * kSegKeys)
                                 ? static_cast<unsigned>(std::max<std::size_t>(1, std::min(par.workers, nseg)))
                                 : 1;
    par.For(workers, [&](std::size_t worker) {
      for (std::size_t wi = nseg * worker / workers; wi < nseg * (worker + 1) / workers; ++wi) {
        const int64_t si = work[wi].first;
        const Segment &s = *work[wi].second;
        const auto bits = sql_like::MatchBlobBitmap(matcher, s.strings.offsets(),
                                                    s.strings.size(), s.strings.data());
        for (std::size_t w = 0; w < bits.size(); ++w) {
          for (uint64_t b = bits[w]; b; b &= b - 1) {
            const std::size_t id = w * 64 + static_cast<std::size_t>(__builtin_ctzll(b));
            fn(static_cast<unsigned>(worker),
               static_cast<int64_t>((static_cast<uint64_t>(si) << kSegShift) | s.key_slot[id]));
          }
        }
      }
    });
    return workers;
  }

  std::size_t MemoryBytes() const {
    std::size_t n = segs_.MemoryBytes() + other_.memory_bytes();
    auto add = [&](const std::shared_ptr<Segment> &p) {
      if (p)
        n += sizeof(Segment) + p->strings.MemoryBytes() + p->key_slot.capacity() * 2 +
             p->id_of_slot.capacity() * 2;
    };
    for (std::size_t i = 0; i < segs_.NumChunks() * SegArray::kChunkSize; ++i)
      if (const auto *p = segs_.Find(static_cast<int64_t>(i)); p)
        add(*p);
    other_.ForEach([&](int64_t, const std::shared_ptr<Segment> &p) { add(p); });
    return n;
  }

private:
  using SegArray = ChunkedCowArray<std::shared_ptr<Segment>, 10>;
  static constexpr int64_t kDenseSegLimit = int64_t{1} << 24; // keys < 2^36

  static std::size_t Slot(int64_t key) { return static_cast<std::size_t>(key) & (kSegKeys - 1); }
  static int64_t SegIndex(int64_t key) { return key >> kSegShift; } // floor
  static bool DenseSeg(int64_t si) { return si >= 0 && si < kDenseSegLimit; }

  const Segment *SegmentOf(int64_t key) const {
    const int64_t si = SegIndex(key);
    if (DenseSeg(si)) {
      const auto *p = segs_.Find(si);
      return p ? p->get() : nullptr;
    }
    const auto *p = other_.Find(si);
    return p ? p->get() : nullptr;
  }
  std::shared_ptr<Segment> &SegSlot(int64_t si) {
    return DenseSeg(si) ? segs_.Mutable(si) : other_[si];
  }
  Segment &MutableSegment(int64_t key) {
    auto &p = SegSlot(SegIndex(key));
    if (!p) {
      p = std::make_shared<Segment>();
      p->id_of_slot.assign(kSegKeys, kNoSlot);
    } else if (p.use_count() > 1) {
      p = std::make_shared<Segment>(*p);
    }
    return *p;
  }
  // Rebuilds the key's segment with `key` replaced by *value (or dropped).
  void Rewrite(int64_t key, const std::string_view *value) {
    const std::shared_ptr<Segment> old(const_cast<Segment *>(SegmentOf(key)), [](Segment *) {});
    auto fresh = std::make_shared<Segment>();
    fresh->id_of_slot.assign(kSegKeys, kNoSlot);
    fresh->strings.Reserve(old->live, old->strings.bytes());
    fresh->key_slot.reserve(old->live);
    for (std::size_t id = 0; id < old->strings.size(); ++id) {
      const uint16_t slot = old->key_slot[id];
      std::string_view v = old->strings.View(id);
      if (slot == Slot(key)) {
        if (!value)
          continue;
        v = *value;
      }
      fresh->id_of_slot[slot] = static_cast<uint16_t>(fresh->strings.size());
      fresh->key_slot.push_back(slot);
      fresh->strings.Append(v);
      ++fresh->live;
    }
    if (!value)
      --size_;
    SegSlot(SegIndex(key)) = std::move(fresh);
  }

  SegArray segs_;
  ShardedCowHashMap<std::shared_ptr<Segment>, 4> other_;
  std::size_t size_ = 0;
};

} // namespace accelerator_codegen::aux_state::core
