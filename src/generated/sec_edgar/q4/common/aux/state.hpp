#pragma once

// Compact, copy-on-write state for the SEC-EDGAR Q4 auxiliary accelerator.
//
// Query semantics (see benchmarks/sec_edgar/queries/q4.sql), parameterized:
//   SELECT s.sic, t.tlabel, p.stmt, COUNT(DISTINCT s.cik), SUM(n.value), AVG(n.value)
//   FROM num n JOIN sub s JOIN tag t JOIN pre p
//   WHERE n.uom = :uom AND p.stmt = :stmt AND s.sic BETWEEN :sic_lo AND :sic_hi
//     AND n.value IS NOT NULL AND t.abstract=0
//   GROUP BY s.sic, t.tlabel, p.stmt HAVING COUNT(DISTINCT s.cik) >= 2
//   ORDER BY total_value DESC LIMIT 500
//
// State model.  The query groups by (sic, tlabel) *within* one (stmt, uom), so
// every parameter value is answered by a read-time filter over per-group
// aggregates keyed by (sic, tlabel, stmt, uom):
//   * cohort   adsh -> (cik id, sic id) for every sub row with sic IS NOT NULL
//   * tag map  (tag id, version id) -> tlabel id for every tag row with abstract = 0
//   * pre map  (adsh, tag, version) -> {(stmt, mult)}: the pre rows (stmt IS NOT
//              NULL) of every key whose adsh / tag qualify
//   * groups   (sic, tl, stmt, uom) -> SUM (double-double), COUNT, COUNT(DISTINCT
//              cik) (backed by a (group, cik) -> joined-row refcount map)
// No per-num-key record is kept: a batch of num deltas is netted per
// (adsh, tag, version, uom) and added straight into the groups, so a value
// update touches only its group(s); structural writes (sub/tag/pre) re-derive
// the old contribution of the affected keys from the current num rows minus the
// transaction's own num deltas.
//
// Groups live per (stmt, uom) "slot", in 64-group shards ordered by sic at
// build time, so a read scans only its (stmt, uom) and skips shards outside
// [sic_lo, sic_hi].  Every container is shared between published versions
// (shared_ptr / copy-on-write shards); a write clones only what it touches.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

inline constexpr uint32_t kQ4None = 0xFFFFFFFFu;

inline uint64_t Q4Mix(uint64_t x) {
  x ^= x >> 32;
  x *= 0x9E3779B97F4A7C15ull;
  x ^= x >> 29;
  return x;
}

// 64-bit finalizer (murmur3 fmix64).
inline uint64_t Q4Fmix(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

inline uint64_t Q4HashBytes(const char *p, size_t n) {
  uint64_t h = 0x243F6A8885A308D3ull ^ (static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull);
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    h = (h ^ w) * 0x9FB21C651E98DF25ull;
    h ^= h >> 32;
    p += 8;
    n -= 8;
  }
  if (n) {
    uint64_t w = 0;
    std::memcpy(&w, p, n);
    h = (h ^ w) * 0x9FB21C651E98DF25ull;
    h ^= h >> 32;
  }
  return Q4Mix(h);
}

// Double-double accumulator helpers: hi+lo carries ~106 bits so that add /
// subtract of arbitrary doubles stays exact enough that deletes cancel inserts.
inline void Q4DDAdd(double &hi, double &lo, double x) {
  const double s = hi + x;
  if (!std::isfinite(s)) { // inf / NaN / overflow: IEEE result, no error term
    hi = s;
    lo = 0.0;
    return;
  }
  const double bb = s - hi;
  const double err = (hi - (s - bb)) + (x - bb);
  const double l = lo + err;
  const double t = s + l;
  lo = l - (t - s);
  hi = t;
}

// hi/lo += x * m, exactly (the product is split with an FMA so that multiples
// of large values keep their low-order bits and later subtraction cancels).
inline void Q4DDAddScaled(double &hi, double &lo, double x, double m) {
  if (m == 1.0) {
    Q4DDAdd(hi, lo, x);
  } else if (m == -1.0) {
    Q4DDAdd(hi, lo, -x);
  } else {
    const double p = x * m;
    if (!std::isfinite(p)) {
      Q4DDAdd(hi, lo, p);
      return;
    }
    const double e = std::fma(x, m, -p);
    Q4DDAdd(hi, lo, p);
    Q4DDAdd(hi, lo, e);
  }
}

// Append-only interned string table with an open-addressing index whose slots
// hold (32-bit hash, id), so a probe touches the string heap only on a likely
// hit.  FindBatch pipelines the lookups of many strings (prefetching the index,
// then the offsets, then the heap) for the bulk build.
class SecEdgarQ4StrDict {
public:
  static constexpr uint32_t kNone = kQ4None;

  uint32_t Find(const char *p, size_t n) const { return FindHashed(p, n, Q4HashBytes(p, n)); }
  uint32_t Find(std::string_view s) const { return Find(s.data(), s.size()); }

  uint32_t FindHashed(const char *p, size_t n, uint64_t h) const {
    if (table.empty()) {
      return kNone;
    }
    const uint64_t h32 = h >> 32;
    const size_t mask = table.size() - 1;
    for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
      const uint64_t slot = table[i];
      if (slot == kEmpty) {
        return kNone;
      }
      if ((slot >> 32) == h32) {
        const uint32_t id = static_cast<uint32_t>(slot);
        if (off[id + 1] - off[id] == n && std::memcmp(arena.data() + off[id], p, n) == 0) {
          return id;
        }
      }
    }
  }

  // out[i] = Find(s[i]) for i < n.  Three pipelined passes (index probe,
  // offsets, string compare) so that the cache misses of different strings
  // overlap instead of serializing.
  void FindBatch(const std::string_view *s, size_t n, uint32_t *out) const {
    if (table.empty()) {
      for (size_t i = 0; i < n; ++i) out[i] = kNone;
      return;
    }
    constexpr size_t kAhead = 16;
    thread_local std::vector<uint64_t> hs;
    thread_local std::vector<uint64_t> begin;
    if (hs.size() < n) {
      hs.resize(n);
      begin.resize(n);
    }
    const size_t mask = table.size() - 1;
    for (size_t i = 0; i < n; ++i) hs[i] = Q4HashBytes(s[i].data(), s[i].size());
    for (size_t i = 0; i < n; ++i) {
      if (i + kAhead < n) __builtin_prefetch(&table[hs[i + kAhead] & mask]);
      const uint64_t h32 = hs[i] >> 32;
      uint32_t c = kNone;
      for (size_t j = static_cast<size_t>(hs[i]) & mask;; j = (j + 1) & mask) {
        const uint64_t slot = table[j];
        if (slot == kEmpty) break;
        if ((slot >> 32) == h32) {
          c = static_cast<uint32_t>(slot);
          break;
        }
      }
      out[i] = c;
    }
    for (size_t i = 0; i < n; ++i) {
      if (i + kAhead < n && out[i + kAhead] != kNone) __builtin_prefetch(&off[out[i + kAhead]]);
      const uint32_t c = out[i];
      begin[i] = c == kNone || off[c + 1] - off[c] != s[i].size() ? ~uint64_t{0} : off[c];
    }
    for (size_t i = 0; i < n; ++i) {
      if (i + kAhead < n && begin[i + kAhead] != ~uint64_t{0}) {
        __builtin_prefetch(arena.data() + begin[i + kAhead]);
      }
      const uint32_t c = out[i];
      if (c == kNone) continue;
      if (begin[i] == ~uint64_t{0} ||
          std::memcmp(arena.data() + begin[i], s[i].data(), s[i].size()) != 0) {
        out[i] = FindHashed(s[i].data(), s[i].size(), hs[i]); // 32-bit hash collision
      }
    }
  }

  uint32_t Insert(const char *p, size_t n) {
    const uint64_t h = Q4HashBytes(p, n);
    const uint32_t existing = FindHashed(p, n, h);
    if (existing != kNone) {
      return existing;
    }
    if (off.empty()) {
      off.push_back(0);
    }
    const size_t size = off.size() - 1;
    if (size + 1 >= kNone) {
      throw std::length_error("sec_edgar q4: string dictionary id space exhausted");
    }
    if ((size + 1) * 2 > table.size()) {
      Rehash(table.empty() ? 64 : table.size() * 2);
    }
    const uint32_t id = static_cast<uint32_t>(size);
    arena.insert(arena.end(), p, p + n);
    off.push_back(arena.size());
    Place(h, id);
    return id;
  }
  uint32_t Insert(std::string_view s) { return Insert(s.data(), s.size()); }

  void Reserve(size_t strings, size_t bytes) {
    arena.reserve(bytes);
    off.reserve(strings + 1);
    size_t cap = 64;
    while (cap < strings * 2) cap *= 2;
    if (cap > table.size()) Rehash(cap);
  }

  std::string_view Get(uint32_t id) const {
    return std::string_view(arena.data() + off[id], off[id + 1] - off[id]);
  }
  uint32_t Size() const { return off.empty() ? 0 : static_cast<uint32_t>(off.size() - 1); }
  uint64_t Bytes() const { return arena.capacity() + off.capacity() * 8 + table.capacity() * 8; }

private:
  static constexpr uint64_t kEmpty = ~uint64_t{0};

  void Place(uint64_t h, uint32_t id) {
    const size_t mask = table.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    while (table[i] != kEmpty) {
      i = (i + 1) & mask;
    }
    table[i] = ((h >> 32) << 32) | id;
  }
  void Rehash(size_t cap) {
    table.assign(cap, kEmpty);
    for (uint32_t id = 0; id < Size(); ++id) {
      Place(Q4HashBytes(arena.data() + off[id], off[id + 1] - off[id]), id);
    }
  }

  std::vector<char> arena;
  std::vector<uint64_t> off; // 64-bit: the string heap may exceed 4 GB
  std::vector<uint64_t> table;
};

// Copy-on-write sharded int64 -> V map.  2^kBits shards, each a linear-probing
// table of {key, value} slots held by shared_ptr; the directory also caches
// each shard's slot array and mask, so a lookup is one directory read (small,
// cache-resident) plus one slot cache line, and can be prefetched
// (FindBatch).  Copying the map copies the directory; the first write to a
// shard in a copy clones only that shard.  Hold it by shared_ptr in the state
// so that versions that do not modify it do not even copy the directory.
// INT64_MIN is reserved (empty slot).
template <typename V, unsigned kBits> class Q4CowMap {
public:
  struct Slot {
    int64_t key;
    V val;
  };
  static constexpr int64_t kEmptyKey = INT64_MIN;
  static constexpr size_t kShards = size_t{1} << kBits;

  Q4CowMap() : dir_(kShards) {}

  static uint64_t Hash(int64_t key) { return Q4Fmix(static_cast<uint64_t>(key) + 1); }
  // Top bits of the hash: the full build lays its key table out in the same
  // order, so the shards are filled from a sequential pass.
  static size_t ShardOf(int64_t key) { return static_cast<size_t>(Hash(key) >> (64 - kBits)); }

  const V *FindHashed(int64_t key, uint64_t h) const {
    const Dir &d = dir_[h >> (64 - kBits)];
    if (!d.slots) return nullptr;
    for (size_t j = h & d.mask;; j = (j + 1) & d.mask) {
      const Slot &s = d.slots[j];
      if (s.key == key) return &s.val;
      if (s.key == kEmptyKey) return nullptr;
    }
  }
  const V *Find(int64_t key) const { return FindHashed(key, Hash(key)); }
  void Prefetch(uint64_t h) const {
    const Dir &d = dir_[h >> (64 - kBits)];
    if (d.slots) __builtin_prefetch(&d.slots[h & d.mask]);
  }
  // out[i] = Find(keys[i]), with the slot lines prefetched ahead.
  void FindBatch(const int64_t *keys, size_t n, const V **out) const {
    constexpr size_t kAhead = 16;
    thread_local std::vector<uint64_t> hs;
    if (hs.size() < n) hs.resize(n);
    for (size_t i = 0; i < n; ++i) hs[i] = Hash(keys[i]);
    for (size_t i = 0; i < n; ++i) {
      if (i + kAhead < n) Prefetch(hs[i + kAhead]);
      out[i] = FindHashed(keys[i], hs[i]);
    }
  }

  V &Upsert(int64_t key, V value) {
    const uint64_t h = Hash(key);
    Dir &d = Mut(h >> (64 - kBits), 1);
    size_t j = h & d.mask;
    while (d.slots[j].key != key && d.slots[j].key != kEmptyKey) j = (j + 1) & d.mask;
    if (d.slots[j].key == kEmptyKey) {
      d.slots[j].key = key;
      ++d.shard->size;
    }
    d.slots[j].val = std::move(value);
    return d.slots[j].val;
  }
  V *FindMut(int64_t key) {
    const uint64_t h = Hash(key);
    if (!FindHashed(key, h)) return nullptr;
    Dir &d = Mut(h >> (64 - kBits), 0);
    size_t j = h & d.mask;
    while (d.slots[j].key != key) j = (j + 1) & d.mask;
    return &d.slots[j].val;
  }
  bool Erase(int64_t key) {
    const uint64_t h = Hash(key);
    if (!FindHashed(key, h)) return false;
    Dir &d = Mut(h >> (64 - kBits), 0);
    size_t hole = h & d.mask;
    while (d.slots[hole].key != key) hole = (hole + 1) & d.mask;
    // backward-shift deletion: no tombstones
    for (size_t k = (hole + 1) & d.mask; d.slots[k].key != kEmptyKey; k = (k + 1) & d.mask) {
      const size_t home = Hash(d.slots[k].key) & d.mask;
      if (((k - home) & d.mask) >= ((k - hole) & d.mask)) {
        d.slots[hole] = std::move(d.slots[k]);
        hole = k;
      }
    }
    d.slots[hole].key = kEmptyKey;
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
  template <typename F> void ForEach(F &&fn) const {
    for (const auto &d : dir_) {
      if (!d.slots) continue;
      for (size_t j = 0; j <= d.mask; ++j) {
        if (d.slots[j].key != kEmptyKey) fn(d.slots[j].key, d.slots[j].val);
      }
    }
  }
  // Loads `entries` (distinct keys, any order) into an empty map.
  void BulkLoad(std::vector<std::pair<int64_t, V>> &entries) {
    std::vector<std::vector<std::pair<int64_t, V>>> parts(kShards);
    const size_t per = entries.size() / kShards + 8;
    for (auto &p : parts) p.reserve(per + per / 4);
    for (auto &e : entries) parts[ShardOf(e.first)].push_back(std::move(e));
    std::vector<std::pair<int64_t, V>>().swap(entries);
    BulkLoadParts(parts);
  }
  // Same, from entries already partitioned by ShardOf (parts.size() == kShards).
  void BulkLoadParts(std::vector<std::vector<std::pair<int64_t, V>>> &parts) {
    for (size_t s = 0; s < kShards; ++s) {
      auto &part = parts[s];
      if (part.empty()) continue;
      size_t cap = 16;
      while (cap * 3 < part.size() * 5) cap *= 2; // load <= 0.6
      auto shard = std::make_shared<Shard>();
      shard->slots.assign(cap, Slot{kEmptyKey, V{}});
      Dir &d = dir_[s];
      d.shard = std::move(shard);
      d.slots = d.shard->slots.data();
      d.mask = cap - 1;
      for (auto &e : part) {
        size_t j = Hash(e.first) & d.mask;
        while (d.slots[j].key != kEmptyKey) j = (j + 1) & d.mask;
        d.slots[j].key = e.first;
        d.slots[j].val = std::move(e.second);
      }
      d.shard->size = part.size();
      std::vector<std::pair<int64_t, V>>().swap(part);
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

  // Private (unshared) shard i with room for `extra` more keys.
  Dir &Mut(size_t i, size_t extra) {
    Dir &d = dir_[i];
    if (!d.shard) {
      d.shard = std::make_shared<Shard>();
      d.shard->slots.assign(16, Slot{kEmptyKey, V{}});
    } else if (d.shard.use_count() > 1) {
      d.shard = std::make_shared<Shard>(*d.shard);
    }
    if ((d.shard->size + extra) * 4 > d.shard->slots.size() * 3) {
      std::vector<Slot> old(std::move(d.shard->slots));
      const size_t cap = old.size() * 2;
      d.shard->slots.assign(cap, Slot{kEmptyKey, V{}});
      for (auto &s : old) {
        if (s.key == kEmptyKey) continue;
        size_t j = Hash(s.key) & (cap - 1);
        while (d.shard->slots[j].key != kEmptyKey) j = (j + 1) & (cap - 1);
        d.shard->slots[j] = std::move(s);
      }
    }
    d.slots = d.shard->slots.data();
    d.mask = d.shard->slots.size() - 1;
    return d;
  }
  std::vector<Dir> dir_;
};

// ------------------------------------------------------------------ limits
// Key packing (see the *Key helpers below).
inline constexpr unsigned kQ4IdBits = 21;
inline constexpr uint32_t kQ4MaxAdsh = 1u << kQ4IdBits;
inline constexpr uint32_t kQ4MaxTag = 1u << kQ4IdBits;
inline constexpr uint32_t kQ4MaxVer = 1u << kQ4IdBits;
inline constexpr uint32_t kQ4MaxStmt = 63;
inline constexpr uint32_t kQ4MaxUom = 1023;
inline constexpr uint32_t kQ4MaxSic = 4096;
inline constexpr uint32_t kQ4MaxTl = 0xFFFFFFu; // 0xFFFFFF encodes NULL
inline constexpr uint32_t kQ4MaxCik = 1u << 24;
inline constexpr uint32_t kQ4MaxSlot = 1u << 15; // keeps pair keys non-negative
inline constexpr uint32_t kQ4MaxLocal = 1u << 24;

// (adsh, tag, version) key of the pre map / of a num row.
inline int64_t Q4PreKey(uint32_t adsh, uint32_t tag, uint32_t ver) {
  return static_cast<int64_t>((static_cast<uint64_t>(adsh) << 42) |
                              (static_cast<uint64_t>(tag) << 21) | ver);
}
inline uint32_t Q4PreKeyAdsh(int64_t k) { return static_cast<uint32_t>(static_cast<uint64_t>(k) >> 42); }
inline uint32_t Q4PreKeyTag(int64_t k) {
  return static_cast<uint32_t>((static_cast<uint64_t>(k) >> 21) & (kQ4MaxTag - 1));
}
inline uint32_t Q4PreKeyVer(int64_t k) { return static_cast<uint32_t>(static_cast<uint64_t>(k) & (kQ4MaxVer - 1)); }
// (tag, version) key of the tag map.
inline int64_t Q4TagKey(uint32_t tag, uint32_t ver) {
  return static_cast<int64_t>((static_cast<uint64_t>(tag) << 21) | ver);
}
inline int64_t Q4GroupKey(uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom) {
  const uint64_t t = tl == kQ4None ? kQ4MaxTl : tl;
  return static_cast<int64_t>((static_cast<uint64_t>(sic_id) << 40) | (t << 16) |
                              (static_cast<uint64_t>(stmt) << 10) | uom);
}
inline uint32_t Q4SuKey(uint32_t stmt, uint32_t uom) { return (stmt << 16) | uom; }
// Group id: (slot of its (stmt, uom)) << 24 | index within the slot.
inline uint64_t Q4Gid(uint32_t slot, uint32_t local) {
  return (static_cast<uint64_t>(slot) << 24) | local;
}
inline uint32_t Q4GidSlot(uint64_t gid) { return static_cast<uint32_t>(gid >> 24); }
inline uint32_t Q4GidLocal(uint64_t gid) { return static_cast<uint32_t>(gid & (kQ4MaxLocal - 1)); }
inline int64_t Q4PairKey(uint64_t gid, uint32_t cik_id) {
  return static_cast<int64_t>((gid << 24) | cik_id);
}

// Pre map value: the tlabel id of the key's tag and up to 4 (stmt, mult) slots
// of 16 bits, (stmt + 1) << 10 | mult with mult in [1, 1023].  Keys whose list
// does not fit have stmts == kQ4PreOverflow and keep it in the overflow map.
inline constexpr uint64_t kQ4PreOverflow = ~uint64_t{0};
struct SecEdgarQ4PreVal {
  uint64_t stmts = 0;
  uint32_t tl = kQ4None;
  uint32_t pad = 0;
};
struct SecEdgarQ4PreOverflow {
  uint32_t n = 0;
  uint8_t stmt[16] = {};
  uint32_t mult[16] = {};
};

// Packs a (stmt, mult) list; returns false when it does not fit 4 slots.
inline bool Q4PackStmts(const uint32_t *stmt, const uint32_t *mult, uint32_t n, uint64_t &out) {
  if (n > 4) return false;
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (stmt[i] >= kQ4MaxStmt || mult[i] == 0 || mult[i] > 1023) return false;
    v |= static_cast<uint64_t>(((stmt[i] + 1) << 10) | mult[i]) << (16 * i);
  }
  out = v;
  return true;
}

using SecEdgarQ4PreMap = Q4CowMap<SecEdgarQ4PreVal, 12>;
using SecEdgarQ4PreOverflowMap = Q4CowMap<SecEdgarQ4PreOverflow, 4>;
using SecEdgarQ4TagMap = Q4CowMap<uint32_t, 10>;    // Q4TagKey -> tlabel id (kQ4None = NULL)
using SecEdgarQ4PairMap = Q4CowMap<int64_t, 13>;    // Q4PairKey -> joined rows
using SecEdgarQ4GroupIndex = Q4CowMap<uint64_t, 10>; // Q4GroupKey -> gid

// fn(stmt, mult) for every pre stmt of key `pk` with packed value `v`.
template <typename F>
inline bool Q4ForEachStmt(int64_t pk, uint64_t v, const SecEdgarQ4PreOverflowMap &over, F &&fn) {
  if (v == kQ4PreOverflow) {
    const auto *o = over.Find(pk);
    if (!o) return false;
    for (uint32_t i = 0; i < o->n; ++i) fn(static_cast<uint32_t>(o->stmt[i]), o->mult[i]);
    return true;
  }
  for (; v != 0; v >>= 16) {
    const uint32_t s = static_cast<uint32_t>(v & 0xFFFF);
    if (s == 0) break;
    fn((s >> 10) - 1, s & 1023u);
  }
  return true;
}

// sub.adsh -> (cik id, sic id) for every filing with a non-NULL sic.
struct SecEdgarQ4Cohort {
  SecEdgarQ4StrDict adsh;
  std::vector<uint32_t> cik_id; // per adsh id, kQ4None when cik IS NULL
  std::vector<uint32_t> sic_id; // per adsh id, kQ4None when the filing does not qualify
  std::vector<int32_t> ciks;    // cik value per cik id
  std::vector<int32_t> sics;    // sic value per sic id
  core::FlatHashMap<uint32_t> cik_index; // cik value -> id
  core::FlatHashMap<uint32_t> sic_index; // sic value -> id
  uint64_t Bytes() const {
    return adsh.Bytes() + cik_id.capacity() * 4 + sic_id.capacity() * 4 + ciks.capacity() * 4 +
           sics.capacity() * 4 + cik_index.memory_bytes() + sic_index.memory_bytes();
  }
};

// tag / version string dictionaries (ids are stable and append-only).
struct SecEdgarQ4TagDicts {
  SecEdgarQ4StrDict tags;
  SecEdgarQ4StrDict versions;
  uint64_t Bytes() const { return tags.Bytes() + versions.Bytes(); }
};

// stmt / uom dictionaries (ids are stable and append-only).
struct SecEdgarQ4Sud {
  SecEdgarQ4StrDict stmts;
  SecEdgarQ4StrDict uoms;
  uint64_t Bytes() const { return stmts.Bytes() + uoms.Bytes(); }
};

struct SecEdgarQ4Labels {
  SecEdgarQ4StrDict tlabels;
  uint64_t Bytes() const { return tlabels.Bytes(); }
};

// Join bridge (only read by maintenance).
struct SecEdgarQ4Bridge {
  std::shared_ptr<const SecEdgarQ4TagDicts> tdict;
  std::shared_ptr<const SecEdgarQ4TagMap> tags;
  std::shared_ptr<const SecEdgarQ4PreMap> pre;
  std::shared_ptr<const SecEdgarQ4PreOverflowMap> pre_over;
  uint64_t Bytes() const {
    uint64_t b = 0;
    if (tdict) b += tdict->Bytes();
    if (tags) b += tags->memory_bytes();
    if (pre) b += pre->memory_bytes();
    if (pre_over) b += pre_over->memory_bytes();
    return b;
  }
};

inline constexpr uint32_t kQ4GroupShardBits = 6;
inline constexpr uint32_t kQ4GroupShardSize = 1u << kQ4GroupShardBits;

// Structure-of-arrays; a write clones one 64-group shard.
struct alignas(32) SecEdgarQ4GroupShard {
  double sum_hi[kQ4GroupShardSize] = {};
  double sum_lo[kQ4GroupShardSize] = {};
  int64_t count[kQ4GroupShardSize] = {};
  uint32_t distinct[kQ4GroupShardSize] = {};
  int32_t sic[kQ4GroupShardSize] = {};
  uint32_t tl[kQ4GroupShardSize] = {};
};

// 32 group shards and their sic ranges; the unit a SuGroups copy shares.
inline constexpr uint32_t kQ4ChunkBits = 5;
inline constexpr uint32_t kQ4ChunkShards = 1u << kQ4ChunkBits;
struct SecEdgarQ4ShardChunk {
  std::shared_ptr<SecEdgarQ4GroupShard> shard[kQ4ChunkShards];
  int32_t min_sic[kQ4ChunkShards];
  int32_t max_sic[kQ4ChunkShards]; // over the used groups of the shard
  SecEdgarQ4ShardChunk() {
    for (uint32_t i = 0; i < kQ4ChunkShards; ++i) {
      min_sic[i] = INT32_MAX;
      max_sic[i] = INT32_MIN;
    }
  }
};

// The groups of one (stmt, uom): group `local` is entry local % 64 of shard
// local / 64; shard k is entry k % 32 of chunk k / 32.  Copying it copies only
// the chunk pointers; a write clones the touched chunk and shard.
struct SecEdgarQ4SuGroups {
  uint32_t stmt = 0;
  uint32_t uom = 0;
  uint32_t n = 0; // groups in use
  std::vector<std::shared_ptr<SecEdgarQ4ShardChunk>> chunks;
  uint32_t Shards() const { return (n + kQ4GroupShardSize - 1) >> kQ4GroupShardBits; }
  const SecEdgarQ4GroupShard &ShardAt(uint32_t k) const {
    return *chunks[k >> kQ4ChunkBits]->shard[k & (kQ4ChunkShards - 1)];
  }
};

// Query parameters resolved against the dictionaries.
struct SecEdgarQ4Params {
  int32_t sic_lo = 4000;
  int32_t sic_hi = 4999;
  std::string stmt = "EQ";
  std::string uom = "USD";
  bool empty = false; // NULL argument / empty range
};

struct SecEdgarQ4ResultRow {
  int32_t sic = 0;
  uint32_t tl = kQ4None;
  int64_t distinct = 0;
  double total = 0.0;
  double avg = 0.0;
};

struct SecEdgarQ4Result {
  std::vector<SecEdgarQ4ResultRow> rows;
  // Pre-built output rows so repeated reads between writes only copy.
  std::vector<core::AuxRow> aux_rows;
};

// One published version.  Readers use labels / sud / su_slot / slots;
// maintenance additionally reads the cohort, bridge, pairs and gindex of the
// same version (so the GroupAggregates image alone is a consistent snapshot).
struct SecEdgarQ4Groups {
  SecEdgarQ4Groups() = default;
  SecEdgarQ4Groups(const SecEdgarQ4Groups &) = delete;
  SecEdgarQ4Groups &operator=(const SecEdgarQ4Groups &) = delete;

  std::shared_ptr<const SecEdgarQ4Labels> labels;
  std::shared_ptr<const SecEdgarQ4Sud> sud;
  std::shared_ptr<const core::FlatHashMap<uint32_t>> su_slot; // Q4SuKey -> slot
  std::vector<std::shared_ptr<SecEdgarQ4SuGroups>> slots;     // const after publication
  std::shared_ptr<const SecEdgarQ4PairMap> pairs;
  std::shared_ptr<const SecEdgarQ4GroupIndex> gindex;
  std::shared_ptr<const SecEdgarQ4Cohort> cohort;
  std::shared_ptr<const SecEdgarQ4Bridge> bridge;

  // Small per-version cache of recent parameter combinations (valid for
  // exactly this immutable object).
  struct CacheKey {
    int32_t lo, hi;
    std::string stmt, uom;
    bool operator==(const CacheKey &o) const {
      return lo == o.lo && hi == o.hi && stmt == o.stmt && uom == o.uom;
    }
  };
  mutable std::mutex cache_mutex;
  mutable std::vector<std::pair<CacheKey, std::shared_ptr<const SecEdgarQ4Result>>> cache;
  mutable size_t cache_next = 0;

  uint64_t Bytes() const {
    uint64_t b = slots.capacity() * sizeof(slots[0]);
    for (const auto &s : slots) {
      if (s) b += sizeof(*s) + s->chunks.size() * sizeof(SecEdgarQ4ShardChunk) + s->Shards() * sizeof(SecEdgarQ4GroupShard);
    }
    if (pairs) b += pairs->memory_bytes();
    if (gindex) b += gindex->memory_bytes();
    if (su_slot) b += su_slot->memory_bytes();
    return b;
  }
};

struct SecEdgarQ4State {
  std::shared_ptr<const SecEdgarQ4Cohort> cohort;
  std::shared_ptr<const SecEdgarQ4Bridge> bridge;
  std::shared_ptr<const SecEdgarQ4Labels> labels;
  std::shared_ptr<const SecEdgarQ4Groups> groups;
};

} // namespace accelerator_codegen::aux_state::query
