#pragma once
//
// ShardedCowMap<K, V, Hash> / ShardedCowMapTxn<K, V, Hash>
// PURPOSE    Copy-on-write hash map for arbitrary keys (strings, composites): a power-of-
//            two number of shards, each an immutable std::unordered_map<K, V> shared
//            between versions. A ShardedCowMapTxn is a mutating view over a copy of a
//            published map that clones each shard the first time it is written.
// COMPLEXITY Find O(1) expected (one hash, one directory read, one unordered_map probe).
//            A write batch costs O(distinct touched shards * shard size) once, then O(1)
//            per write. Copy is O(#shards) pointer copies.
// MEMORY     std::unordered_map node cost (~32 B + sizeof(K, V) + key heap) per entry,
//            8 B per bucket, 16 B per directory entry. Pick the shard count so a shard
//            holds ~1e2-1e4 entries (a touched shard is copied whole).
// COW/THREADS Published maps are immutable and may be read concurrently. Writes go
//            through a Txn on a private copy; Finish() yields the next version. Pointers
//            from Find are invalidated by Txn writes to the same shard (not by the
//            published map, which never changes).
// USE WHEN   keys are strings or composites that do not fit ShardedCowOpenMap's
//            key-in-slot design, lookups are not latency-batched, or values own heap
//            memory. For small fixed-size keys with batched lookups use
//            ShardedCowOpenMap; for int64 keys with fat values ShardedCowHashMap.
//
// `Hash` picks the shard (hash & (shards - 1)); inside a shard the table uses
// std::hash<K>. The shard count is a constructor argument (rounded up to a power of
// two); a default-constructed map has zero shards, is empty, and cannot be written.
//

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::core {

template <typename K, typename V, typename Hash> class ShardedCowMapTxn;

template <typename K, typename V, typename Hash = std::hash<K>> class ShardedCowMap {
public:
  using Shard = std::unordered_map<K, V>;

  ShardedCowMap() = default;
  explicit ShardedCowMap(std::size_t shard_count) {
    std::size_t n = 1;
    while (n < std::max<std::size_t>(1, shard_count)) n <<= 1;
    shards_.resize(n);
  }

  const V *Find(const K &key) const {
    if (shards_.empty()) return nullptr;
    const auto &shard = shards_[ShardOf(key)];
    if (!shard) return nullptr;
    const auto it = shard->find(key);
    return it == shard->end() ? nullptr : &it->second;
  }
  bool Contains(const K &key) const { return Find(key) != nullptr; }
  std::size_t size() const { return size_; }
  std::size_t shard_count() const { return shards_.size(); }
  // fn(const K &, const V &) for every entry, in no particular order.
  template <typename F> void ForEach(F &&f) const {
    for (const auto &shard : shards_) {
      if (!shard) continue;
      for (const auto &entry : *shard) f(entry.first, entry.second);
    }
  }
  // Estimated heap bytes. `extra(key, value)` returns the heap bytes an entry owns
  // beyond its node (e.g. a string's capacity); omit it when entries own none.
  template <typename ExtraBytes> std::size_t memory_bytes(ExtraBytes &&extra) const {
    std::size_t total = shards_.capacity() * sizeof(shards_[0]);
    for (const auto &shard : shards_) {
      if (!shard) continue;
      total += sizeof(Shard) + shard->bucket_count() * sizeof(void *);
      for (const auto &e : *shard) total += 32 + sizeof(V) + extra(e.first, e.second);
    }
    return total;
  }
  std::size_t memory_bytes() const {
    return memory_bytes([](const K &, const V &) { return std::size_t{0}; });
  }

private:
  friend class ShardedCowMapTxn<K, V, Hash>;
  std::size_t ShardOf(const K &key) const {
    return static_cast<std::size_t>(Hash()(key)) & (shards_.size() - 1);
  }
  std::vector<std::shared_ptr<const Shard>> shards_;
  std::size_t size_ = 0;
};

// Mutating view that clones a shard on first write. Construct from the published
// map (copies the directory), mutate, then take the next version with Finish().
template <typename K, typename V, typename Hash = std::hash<K>> class ShardedCowMapTxn {
public:
  using Map = ShardedCowMap<K, V, Hash>;
  explicit ShardedCowMapTxn(const Map &base) : map_(base), owned_(base.shards_.size()) {}
  // Read-only lookup (does not clone the shard or mark the txn dirty).
  const V *Find(const K &key) const { return map_.Find(key); }
  // Mutable pointer to an existing entry (clones its shard if not yet private), or nullptr.
  V *FindMut(const K &key) {
    if (map_.shards_.empty()) return nullptr;
    const std::size_t s = map_.ShardOf(key);
    const auto &shard = map_.shards_[s];
    if (!shard || shard->find(key) == shard->end()) return nullptr;
    auto &mutable_shard = Mutable(s);
    dirty_ = true;
    return &mutable_shard.find(key)->second;
  }
  // The entry for `key`, default-constructed when absent.
  V &GetOrCreate(const K &key) {
    auto &shard = Mutable(map_.ShardOf(key));
    dirty_ = true;
    const auto result = shard.emplace(key, V());
    if (result.second) map_.size_++;
    return result.first->second;
  }
  void Erase(const K &key) {
    if (map_.shards_.empty()) return;
    const std::size_t s = map_.ShardOf(key);
    if (!map_.shards_[s] || map_.shards_[s]->count(key) == 0) return;
    auto &shard = Mutable(s);
    dirty_ = true;
    map_.size_ -= shard.erase(key);
  }
  Map Finish() {
    owned_.clear();
    return std::move(map_);
  }
  bool dirty() const { return dirty_; }
  std::size_t size() const { return map_.size(); }

private:
  typename Map::Shard &Mutable(std::size_t shard) {
    if (!owned_[shard]) {
      owned_[shard] = map_.shards_[shard] ? std::make_shared<typename Map::Shard>(*map_.shards_[shard])
                                          : std::make_shared<typename Map::Shard>();
      map_.shards_[shard] = owned_[shard];
    }
    return *owned_[shard];
  }
  Map map_;
  std::vector<std::shared_ptr<typename Map::Shard>> owned_;
  bool dirty_ = false;
};

} // namespace accelerator_codegen::aux_state::core
