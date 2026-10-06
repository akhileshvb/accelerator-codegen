#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::core {

// Copy-on-write int64 -> variable-length byte record store.
//
// For per-key payloads of a few to a few hundred bytes (a list of values per
// key) where a FlatHashMap<std::vector<...>> would cost a heap block and 24 B
// of header per key. Layout: N = 2^kShardBits shards behind shared_ptr; a shard
// is a FlatHashMap key -> offset plus one append-only byte arena holding
// [u32 length][payload] records. Overwriting or erasing a key leaves dead bytes
// in the arena, reclaimed by compacting the shard once more than half of it is
// dead. Per record: ~13 B hash slot + 4 B length + payload.
//
//   * Copying the store copies N pointers; the first write to a shard in a copy
//     clones that shard (index + arena, both contiguous).
//   * Find / ForEach never copy. A View stays valid until the next write to the
//     same shard (of the same store object).
//   * Same threading rule as ShardedCowHashMap: mutate a private copy only.
template <unsigned kShardBits = 8> class ShardedCowRecordStore {
  struct Shard {
    FlatHashMap<uint32_t> index; // key -> offset of its record in arena
    std::vector<uint8_t> arena;
    std::size_t dead = 0;
  };

public:
  static constexpr std::size_t kShards = std::size_t{1} << kShardBits;

  struct View {
    const uint8_t *data = nullptr;
    std::size_t size = 0;
  };

  ShardedCowRecordStore() : shards_(kShards) {}

  bool Find(int64_t key, View &out) const {
    const auto &s = shards_[ShardOf(key)];
    if (!s)
      return false;
    const uint32_t *offset = s->index.Find(key);
    if (!offset)
      return false;
    out = ViewAt(*s, *offset);
    return true;
  }
  bool Contains(int64_t key) const {
    const auto &s = shards_[ShardOf(key)];
    return s && s->index.Contains(key);
  }

  // Insert or replace the record of `key`.
  void Put(int64_t key, const void *bytes, std::size_t size) {
    Shard &s = MutableShard(key);
    if (const uint32_t *old = s.index.Find(key))
      s.dead += kHeader + ViewAt(s, *old).size;
    if (s.arena.size() + kHeader + size > 0xFFFFFFFFull)
      throw std::length_error("record store shard exceeds 4 GiB");
    const uint32_t offset = static_cast<uint32_t>(s.arena.size());
    const uint32_t length = static_cast<uint32_t>(size);
    s.arena.resize(s.arena.size() + kHeader + size);
    std::memcpy(&s.arena[offset], &length, kHeader);
    if (size)
      std::memcpy(&s.arena[offset + kHeader], bytes, size);
    s.index.Upsert(key, offset);
    MaybeCompact(s);
  }

  bool Erase(int64_t key) {
    if (!Contains(key))
      return false;
    Shard &s = MutableShard(key);
    const uint32_t offset = *s.index.Find(key);
    s.dead += kHeader + ViewAt(s, offset).size;
    s.index.Erase(key);
    if (s.index.empty()) {
      s.arena.clear();
      s.arena.shrink_to_fit();
      s.dead = 0;
    } else {
      MaybeCompact(s);
    }
    return true;
  }

  std::size_t size() const {
    std::size_t n = 0;
    for (const auto &s : shards_)
      n += s ? s->index.size() : 0;
    return n;
  }
  bool empty() const {
    for (const auto &s : shards_)
      if (s && !s->index.empty())
        return false;
    return true;
  }
  std::size_t memory_bytes() const {
    std::size_t n = shards_.capacity() * sizeof(shards_[0]);
    for (const auto &s : shards_)
      if (s)
        n += s->index.memory_bytes() + s->arena.capacity();
    return n;
  }

  // fn(key, View) for every record.
  template <typename F> void ForEach(F &&fn) const {
    for (const auto &s : shards_) {
      if (!s)
        continue;
      for (auto e : static_cast<const FlatHashMap<uint32_t> &>(s->index))
        fn(e.first, ViewAt(*s, e.second));
    }
  }

private:
  static constexpr std::size_t kHeader = sizeof(uint32_t);

  static std::size_t ShardOf(int64_t key) {
    const uint64_t x = static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ULL;
    return static_cast<std::size_t>(x >> (64 - kShardBits));
  }
  static View ViewAt(const Shard &s, uint32_t offset) {
    uint32_t length = 0;
    std::memcpy(&length, &s.arena[offset], kHeader);
    return {&s.arena[offset + kHeader], length};
  }
  Shard &MutableShard(int64_t key) {
    auto &s = shards_[ShardOf(key)];
    if (!s)
      s = std::make_shared<Shard>();
    else if (s.use_count() > 1)
      s = std::make_shared<Shard>(*s);
    return *s;
  }
  static void MaybeCompact(Shard &s) {
    if (s.dead < 4096 || s.dead * 2 < s.arena.size())
      return;
    std::vector<uint8_t> fresh;
    fresh.reserve(s.arena.size() - s.dead);
    for (auto e : s.index) {
      const View v = ViewAt(s, e.second);
      const uint32_t offset = static_cast<uint32_t>(fresh.size());
      const uint32_t length = static_cast<uint32_t>(v.size);
      fresh.resize(fresh.size() + kHeader + v.size);
      std::memcpy(&fresh[offset], &length, kHeader);
      if (v.size)
        std::memcpy(&fresh[offset + kHeader], v.data, v.size);
      e.second = offset;
    }
    s.arena = std::move(fresh);
    s.dead = 0;
  }

  std::vector<std::shared_ptr<Shard>> shards_;
};

} // namespace accelerator_codegen::aux_state::core
