#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace accelerator_codegen::aux_state::core {

// Open-addressing int64 -> V hash map, Swiss-table style.
//
//  * Three flat arrays (control bytes, keys, values): no per-node allocation,
//    ~9 B/slot + sizeof(V), at most 7/8 full (so ~10.3 B + 1.14*sizeof(V) per
//    live entry at max load, 2x that right after a doubling).
//  * Groups of 16 control bytes are probed together (SSE2 when available,
//    scalar otherwise). Probe sequence over groups is triangular, so all groups
//    are visited.
//  * Erase leaves a tombstone only if its group has no empty slot (otherwise
//    the slot becomes empty again). Tombstones are reclaimed on the next rehash.
//  * Pointers/references/iterators are invalidated by any insert (rehash) and by
//    Reserve/Clear. Erase does not move other entries.
//  * V must be default-constructible and movable. Iteration order is arbitrary.
template <typename V> class FlatHashMap {
  static constexpr uint8_t kEmpty = 0x80, kDeleted = 0xFE;
  static constexpr std::size_t kGroup = 16;

public:
  struct Entry {
    int64_t first;
    V &second;
  };
  struct ConstEntry {
    int64_t first;
    const V &second;
  };
  template <bool Const> class Iter {
    using Map = std::conditional_t<Const, const FlatHashMap, FlatHashMap>;
    Map *m_;
    std::size_t i_;
    void Skip() {
      while (i_ < m_->cap_ && m_->ctrl_[i_] >= 0x80)
        ++i_;
    }

  public:
    Iter(Map *m, std::size_t i) : m_(m), i_(i) { Skip(); }
    std::conditional_t<Const, ConstEntry, Entry> operator*() const {
      return {m_->keys_[i_], m_->vals_[i_]};
    }
    Iter &operator++() {
      ++i_;
      Skip();
      return *this;
    }
    bool operator!=(const Iter &o) const { return i_ != o.i_; }
    bool operator==(const Iter &o) const { return i_ == o.i_; }
  };
  using iterator = Iter<false>;
  using const_iterator = Iter<true>;
  iterator begin() { return {this, 0}; }
  iterator end() { return {this, cap_}; }
  const_iterator begin() const { return {this, 0}; }
  const_iterator end() const { return {this, cap_}; }

  FlatHashMap() = default;
  explicit FlatHashMap(std::size_t expected) { Reserve(expected); }

  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  std::size_t capacity() const { return cap_; }
  std::size_t tombstones() const { return tomb_; }
  std::size_t memory_bytes() const {
    return ctrl_.capacity() + keys_.capacity() * sizeof(int64_t) +
           vals_.capacity() * sizeof(V);
  }

  void Clear() {
    ctrl_.clear(); keys_.clear(); vals_.clear();
    cap_ = size_ = tomb_ = 0;
  }
  void Reserve(std::size_t n) {
    std::size_t need = kGroup;
    while (need - need / 8 < n)  // max load 7/8
      need *= 2;
    if (need > cap_)
      Rehash(need);
  }

  V *Find(int64_t key) {
    const std::size_t s = FindSlot(key);
    return s == kNpos ? nullptr : &vals_[s];
  }
  const V *Find(int64_t key) const {
    const std::size_t s = FindSlot(key);
    return s == kNpos ? nullptr : &vals_[s];
  }
  bool Contains(int64_t key) const { return FindSlot(key) != kNpos; }

  // Insert if absent (does not overwrite). Returns {slot value, inserted}.
  std::pair<V *, bool> Insert(int64_t key, V value) {
    std::size_t s = FindSlot(key);
    if (s != kNpos)
      return {&vals_[s], false};
    s = InsertNew(key);
    vals_[s] = std::move(value);
    return {&vals_[s], true};
  }
  // Insert or overwrite. Returns the stored value.
  V &Upsert(int64_t key, V value) {
    std::size_t s = FindSlot(key);
    if (s == kNpos)
      s = InsertNew(key);
    vals_[s] = std::move(value);
    return vals_[s];
  }
  // Default-inserting access (std::unordered_map::operator[] semantics).
  V &operator[](int64_t key) {
    std::size_t s = FindSlot(key);
    if (s == kNpos)
      s = InsertNew(key);
    return vals_[s];
  }
  bool Erase(int64_t key) {
    const std::size_t s = FindSlot(key);
    if (s == kNpos)
      return false;
    const std::size_t g = s / kGroup * kGroup;
    if (MatchEmpty(g) != 0) {
      ctrl_[s] = kEmpty;
    } else {
      ctrl_[s] = kDeleted;
      ++tomb_;
    }
    vals_[s] = V{};
    --size_;
    return true;
  }

private:
  static constexpr std::size_t kNpos = ~std::size_t{0};

  static uint64_t Hash(int64_t k) {
    uint64_t x = static_cast<uint64_t>(k);
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }
  static uint8_t H2(uint64_t h) { return static_cast<uint8_t>(h & 0x7f); }
  std::size_t GroupMask() const { return cap_ / kGroup - 1; }

  uint32_t MatchByte(std::size_t g, uint8_t b) const {
#if defined(__SSE2__)
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&ctrl_[g]));
    return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(static_cast<char>(b)))));
#else
    uint32_t m = 0;
    for (std::size_t i = 0; i < kGroup; ++i)
      m |= static_cast<uint32_t>(ctrl_[g + i] == b) << i;
    return m;
#endif
  }
  uint32_t MatchEmpty(std::size_t g) const { return MatchByte(g, kEmpty); }
  // Empty or deleted (high bit set).
  uint32_t MatchFree(std::size_t g) const {
#if defined(__SSE2__)
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&ctrl_[g]));
    return static_cast<uint32_t>(_mm_movemask_epi8(v));
#else
    uint32_t m = 0;
    for (std::size_t i = 0; i < kGroup; ++i)
      m |= static_cast<uint32_t>(ctrl_[g + i] >= 0x80) << i;
    return m;
#endif
  }

  std::size_t FindSlot(int64_t key) const {
    if (cap_ == 0)
      return kNpos;
    const uint64_t h = Hash(key);
    const std::size_t gm = GroupMask();
    std::size_t grp = static_cast<std::size_t>(h >> 7) & gm;
    for (std::size_t step = 0;; ++step) {
      const std::size_t base = grp * kGroup;
      uint32_t m = MatchByte(base, H2(h));
      while (m) {
        const std::size_t s = base + static_cast<std::size_t>(__builtin_ctz(m));
        if (keys_[s] == key)
          return s;
        m &= m - 1;
      }
      if (MatchEmpty(base))
        return kNpos;
      grp = (grp + step + 1) & gm;
    }
  }

  // Place key (known absent) in a free slot; returns slot index.
  std::size_t InsertNew(int64_t key) {
    if (cap_ == 0)
      Rehash(kGroup);
    else if (size_ + tomb_ + 1 > cap_ - cap_ / 8)
      Rehash(size_ + 1 > (cap_ - cap_ / 8) / 2 ? cap_ * 2 : cap_);
    const uint64_t h = Hash(key);
    const std::size_t gm = GroupMask();
    std::size_t grp = static_cast<std::size_t>(h >> 7) & gm;
    for (std::size_t step = 0;; ++step) {
      const std::size_t base = grp * kGroup;
      const uint32_t m = MatchFree(base);
      if (m) {
        const std::size_t s = base + static_cast<std::size_t>(__builtin_ctz(m));
        if (ctrl_[s] == kDeleted)
          --tomb_;
        ctrl_[s] = H2(h);
        keys_[s] = key;
        ++size_;
        return s;
      }
      grp = (grp + step + 1) & gm;
    }
  }

  void Rehash(std::size_t new_cap) {
    std::vector<uint8_t> oc(std::move(ctrl_));
    std::vector<int64_t> ok(std::move(keys_));
    std::vector<V> ov(std::move(vals_));
    const std::size_t old_cap = cap_;
    ctrl_.assign(new_cap, kEmpty);
    keys_.assign(new_cap, 0);
    vals_.assign(new_cap, V{});
    cap_ = new_cap;
    size_ = 0;
    tomb_ = 0;
    for (std::size_t i = 0; i < old_cap; ++i)
      if (oc[i] < 0x80) {
        const std::size_t s = InsertNew(ok[i]);
        vals_[s] = std::move(ov[i]);
      }
  }

  std::vector<uint8_t> ctrl_;
  std::vector<int64_t> keys_;
  std::vector<V> vals_;
  std::size_t cap_ = 0, size_ = 0, tomb_ = 0;
};

// COW-friendly map: N = 2^kShardBits FlatHashMap shards held by shared_ptr.
// Copying the container copies N pointers; the first write to a shard in a
// copy clones only that shard (O(shard size)). Choose kShardBits so a shard is
// ~1e4-1e5 entries at your expected size (kShardBits 8-12 for 1e6-1e8 keys),
// then a write batch of B keys costs O(min(B, N) * shard size).
// Reads (Find) never copy. Pointers from Find are invalidated by writes to the
// same shard. Same threading rule as ChunkedCowArray.
template <typename V, unsigned kShardBits = 8> class ShardedCowHashMap {
public:
  static constexpr std::size_t kShards = std::size_t{1} << kShardBits;
  using Shard = FlatHashMap<V>;

  ShardedCowHashMap() : shards_(kShards) {}

  const V *Find(int64_t key) const {
    const auto &s = shards_[ShardOf(key)];
    return s ? s->Find(key) : nullptr;
  }
  bool Contains(int64_t key) const { return Find(key) != nullptr; }
  V &Upsert(int64_t key, V value) { return MutableShard(key).Upsert(key, std::move(value)); }
  V &operator[](int64_t key) { return MutableShard(key)[key]; }
  bool Erase(int64_t key) {
    if (!Find(key))
      return false;
    return MutableShard(key).Erase(key);
  }

  std::size_t size() const {
    std::size_t n = 0;
    for (const auto &s : shards_)
      n += s ? s->size() : 0;
    return n;
  }
  std::size_t memory_bytes() const {
    std::size_t n = shards_.capacity() * sizeof(shards_[0]);
    for (const auto &s : shards_)
      n += s ? s->memory_bytes() : 0;
    return n;
  }
  std::size_t SharedShards() const {
    std::size_t n = 0;
    for (const auto &s : shards_)
      n += s && s.use_count() > 1;
    return n;
  }
  // fn(key, const V&) for every entry.
  template <typename F> void ForEach(F &&fn) const {
    for (const auto &s : shards_)
      if (s)
        for (auto e : static_cast<const Shard &>(*s))
          fn(e.first, e.second);
  }

private:
  static std::size_t ShardOf(int64_t key) {
    uint64_t x = static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ULL;
    return static_cast<std::size_t>(x >> (64 - kShardBits));
  }
  Shard &MutableShard(int64_t key) {
    auto &s = shards_[ShardOf(key)];
    if (!s)
      s = std::make_shared<Shard>();
    else if (s.use_count() > 1)
      s = std::make_shared<Shard>(*s);
    return *s;
  }
  std::vector<std::shared_ptr<Shard>> shards_;
};

} // namespace accelerator_codegen::aux_state::core
