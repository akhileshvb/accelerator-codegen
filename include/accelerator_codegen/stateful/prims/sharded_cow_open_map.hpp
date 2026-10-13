#pragma once
//
// ShardedCowOpenMap<K, V, kShardBits, Traits>
// PURPOSE    Copy-on-write key -> V map for point lookups on the hot path: 2^kShardBits
//            shards, each a linear-probing open-addressing table whose slots hold
//            {key, value} inline (a lookup touches one slot cache line), with batch
//            prefetch (Prefetch / FindBatch) so many lookups overlap their misses.
// COMPLEXITY Find/FindHashed O(1) expected: one directory read (small, cache-resident:
//            the directory caches each shard's slot array and mask) plus ~1 slot line.
//            Upsert/Erase O(1) expected; the first write to a shared shard clones it
//            (O(shard slots)), a shard grows by doubling at 75% load. Copy is
//            O(2^kShardBits) pointer copies; Erase is backward-shift (no tombstones).
// MEMORY     sizeof({K, V}) per slot, 1.33-2.7 slots per key (load factor <= 0.75),
//            plus 32 B per directory entry.
// COW/THREADS Copies share shards via shared_ptr; mutate only a private copy, concurrent
//            const reads of a published copy are safe. Pointers from Find/FindMut are
//            invalidated by writes to the same shard. Hold the map by shared_ptr (or in
//            a CowBox) in a larger state so versions that do not touch it do not even
//            copy the directory.
// USE WHEN   keys are cheap to compare and hash (integers, small structs), lookups are
//            latency-critical and batchable, and one slot per key is acceptable. The
//            Traits supply the hash and one key value that is never inserted (the empty
//            marker). Prefer ShardedCowHashMap for fat values / int64 keys with no
//            reserved value; ShardedCowMap for strings or other heavy keys.
//
// Traits requirements:
//   static uint64_t Hash(const K &);  // high bits pick the shard, low bits the slot
//   static K Empty();                 // never a real key; == must be defined on K
// `OpenMapIntTraits<K>` is the default for integral K (murmur3 finalizer of key + 1,
// numeric_limits<K>::min() / max() for signed / unsigned K as the empty marker).
//

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::core {

template <typename K> struct OpenMapIntTraits {
  static_assert(std::is_integral_v<K>, "OpenMapIntTraits needs an integral key");
  static uint64_t Hash(const K &key) {
    uint64_t k = static_cast<uint64_t>(key) + 1;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
  }
  static constexpr K Empty() {
    return std::is_signed_v<K> ? std::numeric_limits<K>::min() : std::numeric_limits<K>::max();
  }
};

template <typename K, typename V, unsigned kShardBits, typename Traits = OpenMapIntTraits<K>>
class ShardedCowOpenMap {
  static_assert(kShardBits >= 1 && kShardBits < 32, "kShardBits out of range");

public:
  struct Slot {
    K key;
    V val;
  };
  static constexpr size_t kShards = size_t{1} << kShardBits;

  ShardedCowOpenMap() : dir_(kShards) {}

  static uint64_t Hash(const K &key) { return Traits::Hash(key); }
  // Top bits of the hash. Bulk loaders that lay entries out per shard use this.
  static size_t ShardOf(const K &key) { return static_cast<size_t>(Hash(key) >> (64 - kShardBits)); }

  // `h` must equal Hash(key).
  const V *FindHashed(const K &key, uint64_t h) const {
    const Dir &d = dir_[h >> (64 - kShardBits)];
    if (!d.slots) return nullptr;
    for (size_t j = h & d.mask;; j = (j + 1) & d.mask) {
      const Slot &s = d.slots[j];
      if (s.key == key) return &s.val;
      if (s.key == Traits::Empty()) return nullptr;
    }
  }
  const V *Find(const K &key) const { return FindHashed(key, Hash(key)); }
  bool Contains(const K &key) const { return Find(key) != nullptr; }
  // Pulls the slot line of a key with hash `h` into cache.
  void Prefetch(uint64_t h) const {
    const Dir &d = dir_[h >> (64 - kShardBits)];
    if (d.slots) __builtin_prefetch(&d.slots[h & d.mask]);
  }
  // out[i] = Find(keys[i]), with the slot lines prefetched ahead.
  void FindBatch(const K *keys, size_t n, const V **out) const {
    constexpr size_t kAhead = 16;
    thread_local std::vector<uint64_t> hs;
    if (hs.size() < n) hs.resize(n);
    for (size_t i = 0; i < n; ++i) hs[i] = Hash(keys[i]);
    for (size_t i = 0; i < n; ++i) {
      if (i + kAhead < n) Prefetch(hs[i + kAhead]);
      out[i] = FindHashed(keys[i], hs[i]);
    }
  }

  // Inserts or overwrites; returns the stored value. `h` must equal Hash(key).
  V &UpsertHashed(const K &key, uint64_t h, V value) {
    Dir &d = Mut(h >> (64 - kShardBits), 1);
    size_t j = h & d.mask;
    while (!(d.slots[j].key == key) && !(d.slots[j].key == Traits::Empty())) j = (j + 1) & d.mask;
    if (d.slots[j].key == Traits::Empty()) {
      d.slots[j].key = key;
      ++d.shard->size;
    }
    d.slots[j].val = std::move(value);
    return d.slots[j].val;
  }
  V &Upsert(const K &key, V value) { return UpsertHashed(key, Hash(key), std::move(value)); }
  // Mutable pointer to the value of an existing key (clones its shard if shared), or nullptr.
  V *FindMut(const K &key) {
    const uint64_t h = Hash(key);
    if (!FindHashed(key, h)) return nullptr;
    Dir &d = Mut(h >> (64 - kShardBits), 0);
    size_t j = h & d.mask;
    while (!(d.slots[j].key == key)) j = (j + 1) & d.mask;
    return &d.slots[j].val;
  }
  bool Erase(const K &key) {
    const uint64_t h = Hash(key);
    if (!FindHashed(key, h)) return false;
    Dir &d = Mut(h >> (64 - kShardBits), 0);
    size_t hole = h & d.mask;
    while (!(d.slots[hole].key == key)) hole = (hole + 1) & d.mask;
    // backward-shift deletion: no tombstones
    for (size_t k = (hole + 1) & d.mask; !(d.slots[k].key == Traits::Empty()); k = (k + 1) & d.mask) {
      const size_t home = Hash(d.slots[k].key) & d.mask;
      if (((k - home) & d.mask) >= ((k - hole) & d.mask)) {
        d.slots[hole] = std::move(d.slots[k]);
        hole = k;
      }
    }
    d.slots[hole].key = Traits::Empty();
    d.slots[hole].val = V{};
    --d.shard->size;
    return true;
  }

  size_t size() const {
    size_t n = 0;
    for (const auto &d : dir_) n += d.shard ? d.shard->size : 0;
    return n;
  }
  size_t memory_bytes() const {
    size_t n = dir_.capacity() * sizeof(Dir);
    for (const auto &d : dir_) n += d.shard ? d.shard->slots.capacity() * sizeof(Slot) + sizeof(Shard) : 0;
    return n;
  }
  // fn(const K &, const V &) for every entry, in no particular order.
  template <typename F> void ForEach(F &&fn) const {
    for (const auto &d : dir_) {
      if (!d.slots) continue;
      for (size_t j = 0; j <= d.mask; ++j) {
        if (!(d.slots[j].key == Traits::Empty())) fn(d.slots[j].key, d.slots[j].val);
      }
    }
  }

  // Room for about n keys spread evenly over the shards, so a bulk build of n
  // inserts does not rehash. Only grows; keeps existing entries.
  void Reserve(size_t n) {
    const size_t per = n / kShards + n / (kShards * 4) + 1;
    size_t cap = 16;
    while (cap - cap / 4 < per) cap <<= 1;
    for (size_t i = 0; i < kShards; ++i) {
      Dir &d = dir_[i];
      if (!d.shard || d.shard->slots.size() < cap) Resize(i, cap);
    }
  }
  // Loads `entries` (distinct keys, any order) into an empty map.
  void BulkLoad(std::vector<std::pair<K, V>> &entries) {
    std::vector<std::vector<std::pair<K, V>>> parts(kShards);
    const size_t per = entries.size() / kShards + 8;
    for (auto &p : parts) p.reserve(per + per / 4);
    for (auto &e : entries) parts[ShardOf(e.first)].push_back(std::move(e));
    std::vector<std::pair<K, V>>().swap(entries);
    BulkLoadParts(parts);
  }
  // Same, from entries already partitioned by ShardOf (parts.size() == kShards).
  void BulkLoadParts(std::vector<std::vector<std::pair<K, V>>> &parts) {
    for (size_t s = 0; s < kShards; ++s) {
      auto &part = parts[s];
      if (part.empty()) continue;
      size_t cap = 16;
      while (cap * 3 < part.size() * 5) cap *= 2; // load <= 0.6
      auto shard = std::make_shared<Shard>();
      shard->slots.assign(cap, Slot{Traits::Empty(), V{}});
      Dir &d = dir_[s];
      d.shard = std::move(shard);
      d.slots = d.shard->slots.data();
      d.mask = cap - 1;
      for (auto &e : part) {
        size_t j = Hash(e.first) & d.mask;
        while (!(d.slots[j].key == Traits::Empty())) j = (j + 1) & d.mask;
        d.slots[j].key = e.first;
        d.slots[j].val = std::move(e.second);
      }
      d.shard->size = part.size();
      std::vector<std::pair<K, V>>().swap(part);
    }
  }

private:
  struct Shard {
    std::vector<Slot> slots; // power-of-two size
    size_t size = 0;
  };
  struct Dir {
    std::shared_ptr<Shard> shard;
    Slot *slots = nullptr;
    size_t mask = 0;
  };

  // Rehashes the (private) shard of `d` into a table of `cap` slots.
  void Rebuild(Dir &d, size_t cap) {
    std::vector<Slot> old(std::move(d.shard->slots));
    d.shard->slots.assign(cap, Slot{Traits::Empty(), V{}});
    for (auto &s : old) {
      if (s.key == Traits::Empty()) continue;
      size_t j = Hash(s.key) & (cap - 1);
      while (!(d.shard->slots[j].key == Traits::Empty())) j = (j + 1) & (cap - 1);
      d.shard->slots[j] = std::move(s);
    }
  }
  // Private (unshared) shard i, table size >= cap.
  void Resize(size_t i, size_t cap) {
    Dir &d = dir_[i];
    if (!d.shard) {
      d.shard = std::make_shared<Shard>();
      d.shard->slots.assign(cap, Slot{Traits::Empty(), V{}});
    } else {
      if (d.shard.use_count() > 1) d.shard = std::make_shared<Shard>(*d.shard);
      Rebuild(d, cap);
    }
    d.slots = d.shard->slots.data();
    d.mask = cap - 1;
  }
  // Private (unshared) shard i with room for `extra` more keys.
  Dir &Mut(size_t i, size_t extra) {
    Dir &d = dir_[i];
    if (!d.shard) {
      d.shard = std::make_shared<Shard>();
      d.shard->slots.assign(16, Slot{Traits::Empty(), V{}});
    } else if (d.shard.use_count() > 1) {
      d.shard = std::make_shared<Shard>(*d.shard);
    }
    if ((d.shard->size + extra) * 4 > d.shard->slots.size() * 3) Rebuild(d, d.shard->slots.size() * 2);
    d.slots = d.shard->slots.data();
    d.mask = d.shard->slots.size() - 1;
    return d;
  }
  std::vector<Dir> dir_;
};

} // namespace accelerator_codegen::aux_state::core
