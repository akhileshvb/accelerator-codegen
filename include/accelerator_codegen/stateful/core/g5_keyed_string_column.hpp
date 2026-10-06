#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/sql_like.hpp"
#include "accelerator_codegen/stateful/core/string_blob.hpp"

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
//     worker threads (segments handed out in blocks), O(total string bytes)
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
  // `max_threads` workers concurrently (worker in [0, workers)); it must only
  // touch worker-local state or be thread-safe. Returns the worker count used.
  template <typename Fn>
  unsigned ForEachMatch(const sql_like::LikeMatcher &matcher, unsigned max_threads,
                        Fn &&fn) const {
    const std::size_t nseg = segs_.NumChunks() * SegArray::kChunkSize;
    constexpr std::size_t kBlock = 8;
    unsigned workers = 1;
    if (max_threads > 1 && size_ >= 8 * kSegKeys)
      workers = std::min<unsigned>(max_threads, static_cast<unsigned>((nseg + kBlock - 1) / kBlock));
    if (workers == 0)
      workers = 1;
    std::atomic<std::size_t> next{0};
    auto run = [&](unsigned worker) {
      for (;;) {
        const std::size_t lo = next.fetch_add(kBlock);
        if (lo >= nseg)
          return;
        for (std::size_t si = lo; si < std::min(nseg, lo + kBlock); ++si) {
          const auto *sp = segs_.Find(static_cast<int64_t>(si));
          if (!sp || !*sp || (*sp)->live == 0)
            continue;
          const Segment &s = **sp;
          const std::size_t n = s.strings.size();
          const auto bits = sql_like::MatchBlobBitmap(matcher, s.strings.offsets(), n,
                                                      s.strings.data(), 1);
          for (std::size_t w = 0; w < bits.size(); ++w) {
            uint64_t b = bits[w];
            while (b) {
              const std::size_t id = w * 64 + static_cast<std::size_t>(__builtin_ctzll(b));
              b &= b - 1;
              if (s.id_of_slot[s.key_slot[id]] != id)
                continue; // superseded string (never happens after Rewrite)
              fn(worker, static_cast<int64_t>((si << kSegShift) | s.key_slot[id]));
            }
          }
        }
      }
    };
    if (workers == 1) {
      run(0);
    } else {
      std::vector<std::thread> pool;
      for (unsigned t = 0; t < workers; ++t)
        pool.emplace_back(run, t);
      for (auto &th : pool)
        th.join();
    }
    return workers;
  }

  std::size_t MemoryBytes() const {
    std::size_t n = segs_.MemoryBytes();
    for (std::size_t i = 0; i < segs_.NumChunks() * SegArray::kChunkSize; ++i)
      if (const auto *p = segs_.Find(static_cast<int64_t>(i)); p && *p)
        n += sizeof(Segment) + (*p)->strings.MemoryBytes() + (*p)->key_slot.capacity() * 2 +
             (*p)->id_of_slot.capacity() * 2;
    return n;
  }

private:
  using SegArray = ChunkedCowArray<std::shared_ptr<Segment>, 10>;

  static std::size_t Slot(int64_t key) { return static_cast<std::size_t>(key) & (kSegKeys - 1); }
  const Segment *SegmentOf(int64_t key) const {
    if (key < 0)
      return nullptr;
    const auto *p = segs_.Find(key >> kSegShift);
    return p ? p->get() : nullptr;
  }
  Segment &MutableSegment(int64_t key) {
    auto &p = segs_.Mutable(key >> kSegShift);
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
    const std::shared_ptr<Segment> old = *segs_.Find(key >> kSegShift);
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
    segs_.Mutable(key >> kSegShift) = std::move(fresh);
  }

  SegArray segs_;
  std::size_t size_ = 0;
};

} // namespace accelerator_codegen::aux_state::core
