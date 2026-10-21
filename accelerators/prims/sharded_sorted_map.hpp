#pragma once
//
// ShardedSortedMap<K, T, KeyOf, Hash, kMaxShardBits, kMidBits, Less>
//   (+ KeyedValue<K, V> and the ShardedSortedKvMap<K, V, ...> alias)
// PURPOSE    Copy-on-write map from an ordered key K to a stored element T.
//            Keys are hashed to one of 2^bits shards; a shard is an immutable,
//            key-sorted flat vector<T>. Shards hang under a two-level directory
//            (a top array of shared_ptr<const Mid>, each Mid an array of
//            2^kMidBits shared_ptr<const Leaf>), so copying a map copies the
//            top pointers only. T carries its own key (KeyOf extracts it), so
//            records keep their natural layout; KeyedValue<K, V> is the plain
//            {key, value} element.
// COMPLEXITY Find: one hash + two pointer hops + binary search in a shard of
//            ~100-500 entries. Copy: O(2^bits / 2^kMidBits) pointers. Txn: the
//            first write to a shard clones that shard (O(shard)); Finish clones
//            each touched Mid once and swaps the new shards in, optionally in
//            parallel. Builder: O(n log shard) bulk load.
// MEMORY     sizeof(T) per entry + ~24 B and one allocation per non-empty shard
//            + 8 B per directory slot (Builder(expected) picks the shard count
//            so shards average <= 256 entries; no per-entry node).
// COW/THREADS A published map is immutable: const reads from any number of
//            threads are safe. All mutation goes through a Txn (single writer,
//            private to one thread) that yields a new map in Finish(); older
//            maps are never altered and keep sharing every untouched shard.
// USE WHEN   a large keyed table (millions of rows) must publish a new immutable
//            version per batch of changes in O(touched shards), with point
//            lookups and key-ordered storage within a shard. Prefer
//            ShardedCowHashMap when keys are int64 and lookups dominate with
//            no need for in-shard order or a bulk sorted build; prefer
//            AnyKeyCowArray when keys are dense integers.
//

#include <algorithm>
#include <iterator>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "prims/parallelism.hpp"
#include "prims/row_pieces.hpp"

namespace accelerator_codegen::aux_state::core {

// Default shard hash: integral keys convert directly, others use std::hash.
// The map multiplies the result by a golden-ratio constant and takes the top
// bits, so the hash only needs to be injective-ish, not well mixed.
template <class K> struct ShardedSortedMapHash {
  uint64_t operator()(const K &k) const {
    if constexpr (std::is_integral_v<K> || std::is_enum_v<K>)
      return static_cast<uint64_t>(k);
    else
      return static_cast<uint64_t>(std::hash<K>{}(k));
  }
};

// Plain element for key -> value maps.
template <class K, class V> struct KeyedValue {
  K key{};
  V value{};
};

template <class K, class V> struct KeyedValueKey {
  const K &operator()(const KeyedValue<K, V> &e) const { return e.key; }
};

// KeyOf: callable `const K &operator()(const T &) const`.
template <class K, class T, class KeyOf, class Hash = ShardedSortedMapHash<K>,
          unsigned kMaxShardBits = 18, unsigned kMidBits = 8,
          class Less = std::less<K>>
class ShardedSortedMap {
  static_assert(kMaxShardBits >= 1 && kMaxShardBits <= 30, "shard bits");
  static_assert(kMidBits >= 1 && kMidBits <= kMaxShardBits, "mid bits");
  static constexpr uint32_t kMidSize = 1u << kMidBits;

public:
  using Element = T;
  using Leaf = std::vector<T>;
  struct Mid {
    std::array<std::shared_ptr<const Leaf>, kMidSize> leaves;
  };

  class Txn;
  class Builder;

  // Looks up `key`; the pointer stays valid as long as this map (or any copy
  // sharing the shard) lives.
  const T *Find(const K &key) const {
    if (top_.empty())
      return nullptr;
    const uint32_t shard = ShardOf(key, bits_);
    const auto &mid = top_[shard >> kMidBits];
    if (!mid)
      return nullptr;
    return FindIn(mid->leaves[shard & (kMidSize - 1)].get(), key);
  }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  uint32_t shard_bits() const { return bits_; }

  // Visits every element, shard by shard (sorted by key within a shard, no
  // global order).
  template <class F> void ForEach(F &&f) const {
    for (const auto &mid : top_) {
      if (!mid)
        continue;
      for (const auto &leaf : mid->leaves)
        if (leaf)
          for (const auto &e : *leaf)
            f(e);
    }
  }

  // Approximate heap footprint of this version including shared parts.
  std::size_t MemoryBytes() const {
    std::size_t bytes = top_.capacity() * sizeof(top_[0]);
    for (const auto &mid : top_) {
      if (!mid)
        continue;
      bytes += sizeof(Mid);
      for (const auto &leaf : mid->leaves)
        if (leaf)
          bytes += sizeof(Leaf) + leaf->capacity() * sizeof(T);
    }
    return bytes;
  }

private:
  static uint32_t ShardOf(const K &key, uint32_t bits) {
    if (bits == 0)
      return 0;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(Hash{}(key)) * 0x9E3779B97F4A7C15ULL) >>
        (64 - bits));
  }
  static std::size_t TopCount(uint32_t bits) {
    return bits > kMidBits ? std::size_t{1} << (bits - kMidBits) : 1;
  }
  static typename Leaf::const_iterator LowerIn(const Leaf &leaf, const K &key) {
    return std::lower_bound(leaf.begin(), leaf.end(), key,
                            [](const T &e, const K &k) {
                              return Less{}(KeyOf{}(e), k);
                            });
  }
  static typename Leaf::iterator LowerIn(Leaf &leaf, const K &key) {
    return std::lower_bound(leaf.begin(), leaf.end(), key,
                            [](const T &e, const K &k) {
                              return Less{}(KeyOf{}(e), k);
                            });
  }
  static bool KeyEq(const K &a, const K &b) {
    return !Less{}(a, b) && !Less{}(b, a);
  }
  static const T *FindIn(const Leaf *leaf, const K &key) {
    if (!leaf)
      return nullptr;
    const auto it = LowerIn(*leaf, key);
    return (it != leaf->end() && KeyEq(KeyOf{}(*it), key)) ? &*it : nullptr;
  }

  std::vector<std::shared_ptr<const Mid>> top_;
  uint32_t bits_ = kMaxShardBits;
  std::size_t size_ = 0;
};

// Copy-on-write writer over a base map. Find sees the transaction's own
// writes. Each touched shard is cloned once; Finish() publishes a new map and
// leaves the base (and every map copied from it) unchanged.
template <class K, class T, class KeyOf, class Hash, unsigned kMaxShardBits,
          unsigned kMidBits, class Less>
class ShardedSortedMap<K, T, KeyOf, Hash, kMaxShardBits, kMidBits, Less>::Txn {
  using Map = ShardedSortedMap;

public:
  explicit Txn(const Map &base) : map_(base) {
    if (map_.top_.empty())
      map_.top_.resize(Map::TopCount(map_.bits_));
    owned_.resize(map_.top_.size());
  }

  const T *Find(const K &key) const {
    return Map::FindIn(Current(Map::ShardOf(key, map_.bits_)), key);
  }
  // Inserts `value` or replaces the element with the same key.
  void Upsert(const T &value) {
    const K &key = KeyOf{}(value);
    auto &leaf = Own(Map::ShardOf(key, map_.bits_));
    auto it = Map::LowerIn(leaf, key);
    if (it != leaf.end() && Map::KeyEq(KeyOf{}(*it), key)) {
      *it = value;
    } else {
      leaf.insert(it, value);
      ++map_.size_;
    }
  }
  // Mutable access to an existing element (nullptr if absent). The caller must
  // not change its key. Clones the shard on first use.
  T *Mutable(const K &key) {
    const uint32_t shard = Map::ShardOf(key, map_.bits_);
    if (!Map::FindIn(Current(shard), key))
      return nullptr;
    auto &leaf = Own(shard);
    return &*Map::LowerIn(leaf, key);
  }
  // Removes `key`; false if it was absent.
  bool Erase(const K &key) {
    const uint32_t shard = Map::ShardOf(key, map_.bits_);
    if (!Map::FindIn(Current(shard), key))
      return false;
    auto &leaf = Own(shard);
    leaf.erase(Map::LowerIn(leaf, key));
    --map_.size_;
    return true;
  }
  bool Dirty() const { return !touched_.empty(); }
  std::size_t size() const { return map_.size_; }

  // Publishes the new map. `parallel_for(n, fn)` must run fn(0..n-1), possibly
  // on several threads; each call owns one Mid.
  template <class ParallelFor> Map Finish(ParallelFor &&parallel_for) {
    std::vector<std::shared_ptr<Mid>> built(touched_.size());
    parallel_for(touched_.size(), [&](std::size_t t) {
      const uint32_t hi = touched_[t];
      auto mid = map_.top_[hi] ? std::make_shared<Mid>(*map_.top_[hi])
                               : std::make_shared<Mid>();
      auto &own = *owned_[hi];
      for (uint32_t lo = 0; lo < kMidSize; ++lo) {
        if (!own.leaves[lo])
          continue;
        if (own.leaves[lo]->empty())
          mid->leaves[lo] = nullptr;
        else
          mid->leaves[lo] = std::move(own.leaves[lo]);
      }
      built[t] = std::move(mid);
    });
    for (std::size_t t = 0; t < touched_.size(); ++t)
      map_.top_[touched_[t]] = std::move(built[t]);
    touched_.clear();
    owned_.clear();
    return std::move(map_);
  }
  Map Finish() {
    return Finish([](std::size_t n, const auto &fn) {
      for (std::size_t i = 0; i < n; ++i)
        fn(i);
    });
  }

private:
  // Shards cloned by this transaction, grouped by mid (no hashing on the
  // lookup path); `touched_` lists the mids that have any.
  struct Owned {
    std::array<std::shared_ptr<Leaf>, kMidSize> leaves;
  };

  const Leaf *Current(uint32_t shard) const {
    const uint32_t hi = shard >> kMidBits, lo = shard & (kMidSize - 1);
    if (const auto &own = owned_[hi])
      if (const Leaf *leaf = own->leaves[lo].get())
        return leaf;
    const auto &mid = map_.top_[hi];
    return mid ? mid->leaves[lo].get() : nullptr;
  }
  Leaf &Own(uint32_t shard) {
    const uint32_t hi = shard >> kMidBits, lo = shard & (kMidSize - 1);
    auto &own = owned_[hi];
    if (!own) {
      own = std::make_unique<Owned>();
      touched_.push_back(hi);
    }
    auto &slot = own->leaves[lo];
    if (slot)
      return *slot;
    const auto &mid = map_.top_[hi];
    const Leaf *cur = mid ? mid->leaves[lo].get() : nullptr;
    slot = std::make_shared<Leaf>();
    if (cur) {
      // One slot of headroom: the common write is a single insert, which
      // would otherwise reallocate and copy the whole shard a second time.
      slot->reserve(cur->size() + 1);
      slot->assign(cur->begin(), cur->end());
    }
    return *slot;
  }

  Map map_;
  std::vector<std::unique_ptr<Owned>> owned_;
  std::vector<uint32_t> touched_;
};

// Bulk loader: Add (any order), Sort / SortDedupe, mutable Find / ForEach,
// then Finish() moves the shards into an immutable map.
template <class K, class T, class KeyOf, class Hash, unsigned kMaxShardBits,
          unsigned kMidBits, class Less>
class ShardedSortedMap<K, T, KeyOf, Hash, kMaxShardBits, kMidBits, Less>::Builder {
  using Map = ShardedSortedMap;
  struct Bits {
    uint32_t v;
  };

public:
  // Full-width directory (2^kMaxShardBits shards).
  Builder() : Builder(Bits{kMaxShardBits}) {}
  // Picks the smallest shard count (<= 2^kMaxShardBits) that keeps shards at
  // about `target_shard` entries or fewer.
  explicit Builder(std::size_t expected_entries, std::size_t target_shard = 256)
      : Builder(Bits{BitsFor(expected_entries, target_shard)}) {}

  void Add(const T &value) {
    shards_[Map::ShardOf(KeyOf{}(value), bits_)].push_back(value);
  }
  // Appends the elements of `from` (same directory width), shard by shard:
  // merging builders in partition order equals adding their rows in that order.
  void Merge(Builder &&from) {
    for (std::size_t g = 0; g < shards_.size(); ++g) {
      auto &into = shards_[g];
      auto &part = from.shards_[g];
      if (into.empty()) {
        into = std::move(part);
      } else {
        into.insert(into.end(), std::make_move_iterator(part.begin()),
                    std::make_move_iterator(part.end()));
      }
      part = Leaf();
    }
  }
  // Adds every element of `pieces` as if Add were called in piece order (the
  // pieces of a partitioned scan, see RowPieces), filling disjoint groups of
  // shards in parallel. Empties `pieces`.
  void AddPieces(std::vector<std::vector<T>> &pieces, const Parallelism &parallelism) {
    const unsigned group_bits = std::min(bits_, kGroupBits);
    const unsigned shift = bits_ - group_bits;
    const std::size_t groups = std::size_t{1} << group_bits;
    auto grouped = GroupRows(parallelism, pieces, groups, [&](const T &e) {
      return static_cast<std::size_t>(Map::ShardOf(KeyOf{}(e), bits_) >> shift);
    });
    parallelism.For(groups, [&](std::size_t g) {
      const std::size_t first = g << shift;
      std::vector<std::size_t> extra(std::size_t{1} << shift, 0);
      ForGroup(grouped, g, [&](T &e) { ++extra[Map::ShardOf(KeyOf{}(e), bits_) - first]; });
      for (std::size_t k = 0; k < extra.size(); ++k)
        shards_[first + k].reserve(shards_[first + k].size() + extra[k]);
      ForGroup(grouped, g, [&](T &e) {
        shards_[Map::ShardOf(KeyOf{}(e), bits_)].push_back(std::move(e));
      });
    });
  }
  // Sorts every shard by key. Keys must be unique. `parallel_for(n, fn)` must
  // run fn(0..n-1), possibly on several threads; each call owns one shard.
  template <class ParallelFor> void Sort(ParallelFor &&parallel_for) {
    parallel_for(shards_.size(), [&](std::size_t g) {
      std::sort(shards_[g].begin(), shards_[g].end(), Cmp());
    });
  }
  void Sort() {
    Sort([](std::size_t n, const auto &fn) {
      for (std::size_t i = 0; i < n; ++i)
        fn(i);
    });
  }
  // Sorts every shard by key; of duplicate keys the last added wins.
  template <class ParallelFor> void SortDedupe(ParallelFor &&parallel_for) {
    parallel_for(shards_.size(), [&](std::size_t g) {
      auto &shard = shards_[g];
      std::stable_sort(shard.begin(), shard.end(), Cmp());
      std::size_t w = 0;
      for (std::size_t r = 0; r < shard.size(); ++r) {
        if (w > 0 && Map::KeyEq(KeyOf{}(shard[w - 1]), KeyOf{}(shard[r])))
          shard[w - 1] = std::move(shard[r]);
        else
          shard[w++] = std::move(shard[r]);
      }
      shard.resize(w);
      shard.shrink_to_fit();
    });
  }
  void SortDedupe() {
    SortDedupe([](std::size_t n, const auto &fn) {
      for (std::size_t i = 0; i < n; ++i)
        fn(i);
    });
  }
  // Valid only after Sort / SortDedupe.
  T *Find(const K &key) {
    auto &shard = shards_[Map::ShardOf(key, bits_)];
    auto it = Map::LowerIn(shard, key);
    return (it != shard.end() && Map::KeyEq(KeyOf{}(*it), key)) ? &*it : nullptr;
  }
  template <class F> void ForEach(F &&f) {
    for (auto &shard : shards_)
      for (auto &e : shard)
        f(e);
  }
  // ForEach over the shards [begin, end): disjoint ranges may run concurrently.
  std::size_t shard_count() const { return shards_.size(); }
  template <class F> void ForEachInShards(std::size_t begin, std::size_t end, F &&f) {
    for (std::size_t g = begin; g < end; ++g)
      for (auto &e : shards_[g])
        f(e);
  }
  Map Finish() {
    Map out;
    out.bits_ = bits_;
    out.top_.resize(Map::TopCount(bits_));
    for (std::size_t hi = 0; hi < out.top_.size(); ++hi) {
      std::shared_ptr<Mid> mid;
      for (uint32_t lo = 0; lo < kMidSize; ++lo) {
        const std::size_t g = (hi << kMidBits) | lo;
        if (g >= shards_.size())
          break;
        auto &shard = shards_[g];
        if (shard.empty())
          continue;
        if (!mid)
          mid = std::make_shared<Mid>();
        out.size_ += shard.size();
        mid->leaves[lo] = std::make_shared<const Leaf>(std::move(shard));
      }
      out.top_[hi] = std::move(mid);
    }
    shards_.clear();
    return out;
  }

private:
  // Shard groups AddPieces hands to the pool: enough to balance 1000+ workers.
  static constexpr unsigned kGroupBits = 10;

  template <class Grouped, class F> static void ForGroup(Grouped &grouped, std::size_t g, F &&f) {
    for (auto &piece : grouped)
      for (std::size_t i = piece.begin[g]; i < piece.begin[g + 1]; ++i)
        f(piece.rows[i]);
  }
  explicit Builder(Bits b) : bits_(b.v), shards_(std::size_t{1} << b.v) {}
  static uint32_t BitsFor(std::size_t n, std::size_t target) {
    uint32_t b = 0;
    while (b < kMaxShardBits && (n >> b) > target)
      ++b;
    return b;
  }
  static auto Cmp() {
    return [](const T &a, const T &b) { return Less{}(KeyOf{}(a), KeyOf{}(b)); };
  }

  uint32_t bits_;
  std::vector<Leaf> shards_;
};

// Key -> value convenience alias; elements are KeyedValue<K, V>.
template <class K, class V, class Hash = ShardedSortedMapHash<K>,
          unsigned kMaxShardBits = 18, unsigned kMidBits = 8,
          class Less = std::less<K>>
using ShardedSortedKvMap =
    ShardedSortedMap<K, KeyedValue<K, V>, KeyedValueKey<K, V>, Hash,
                     kMaxShardBits, kMidBits, Less>;

} // namespace accelerator_codegen::aux_state::core
