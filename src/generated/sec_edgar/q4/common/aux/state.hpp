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
//   * pre map  (adsh, tv, stmt)       -> mult  = number of pre rows (stmt IS NOT NULL)
//                                        whose adsh is a sub row with sic IS NOT NULL
//                                        and whose (tag, version) is a tag with abstract=0
//   * num map  (adsh, tv, stmt, uom)  -> running SUM / COUNT of the non-NULL num
//                                        values (double-double) and the group id
//   * groups   (sic, tl, stmt, uom)   -> sum, count, COUNT(DISTINCT cik) (backed by a
//                                        (group, cik) -> joined-row refcount map)
// A num write touches the stmts that have a pre record for its key (<= a few)
// and is O(1) per stmt; sub/tag/pre writes remove and re-add the affected
// records from a restricted re-read of the base tables.  No filter on sic, stmt
// or uom is baked into the state; a read scans the compact group columns
// (AVX2 compare of distinct/sic/stmt-uom) and takes the top 500.
//
// All big containers are shared between published versions (shared_ptr /
// copy-on-write shards); a write clones only the shards it touches.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

inline constexpr uint32_t kQ4None = 0xFFFFFFFFu;

inline uint64_t Q4Mix(uint64_t x) {
  x ^= x >> 32;
  x *= 0x9E3779B97F4A7C15ull;
  x ^= x >> 29;
  return x;
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

// Double-double accumulator: hi+lo carries ~106 bits so that add/subtract of
// arbitrary doubles stays exact enough that deletes cancel inserts.
struct SecEdgarQ4DD {
  double hi = 0.0;
  double lo = 0.0;
};

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

// Append-only interned string table with an open-addressing index.
class SecEdgarQ4StrDict {
public:
  static constexpr uint32_t kNone = kQ4None;

  uint32_t Find(const char *p, size_t n) const {
    if (table.empty()) {
      return kNone;
    }
    const uint64_t h = Q4HashBytes(p, n);
    const uint32_t h32 = static_cast<uint32_t>(h >> 32);
    const size_t mask = table.size() - 1;
    for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
      const uint32_t id = table[i];
      if (id == kNone) {
        return kNone;
      }
      if (hash32[id] == h32 && off[id + 1] - off[id] == n &&
          std::memcmp(arena.data() + off[id], p, n) == 0) {
        return id;
      }
    }
  }
  uint32_t Find(std::string_view s) const { return Find(s.data(), s.size()); }

  uint32_t Insert(const char *p, size_t n) {
    const uint32_t existing = Find(p, n);
    if (existing != kNone) {
      return existing;
    }
    if (off.empty()) {
      off.push_back(0);
    }
    if (hash32.size() + 1 >= kNone) {
      throw std::length_error("sec_edgar q4: string dictionary id space exhausted");
    }
    if ((hash32.size() + 1) * 2 > table.size()) {
      Rehash(table.empty() ? 64 : table.size() * 2);
    }
    const uint32_t id = static_cast<uint32_t>(hash32.size());
    const uint64_t h = Q4HashBytes(p, n);
    arena.insert(arena.end(), p, p + n);
    off.push_back(arena.size());
    hash32.push_back(static_cast<uint32_t>(h >> 32));
    const size_t mask = table.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    while (table[i] != kNone) {
      i = (i + 1) & mask;
    }
    table[i] = id;
    return id;
  }
  uint32_t Insert(std::string_view s) { return Insert(s.data(), s.size()); }

  std::string_view Get(uint32_t id) const {
    return std::string_view(arena.data() + off[id], off[id + 1] - off[id]);
  }
  uint32_t Size() const { return static_cast<uint32_t>(hash32.size()); }
  uint64_t Bytes() const {
    return arena.size() + off.size() * 8 + hash32.size() * 4 + table.size() * 4;
  }
  // Payload bytes only (strings + lengths), used by the compact encoding.
  uint64_t PayloadBytes() const { return arena.size() + hash32.size() * 4; }

private:
  void Rehash(size_t cap) {
    table.assign(cap, kNone);
    const size_t mask = cap - 1;
    for (uint32_t id = 0; id < hash32.size(); ++id) {
      const uint64_t h = Q4HashBytes(arena.data() + off[id], off[id + 1] - off[id]);
      size_t i = static_cast<size_t>(h) & mask;
      while (table[i] != kNone) {
        i = (i + 1) & mask;
      }
      table[i] = id;
    }
  }

  std::vector<char> arena;
  std::vector<uint64_t> off; // 64-bit: the string heap may exceed 4 GB
  std::vector<uint32_t> hash32;
  std::vector<uint32_t> table;
};

// sub.adsh -> (cik id, sic id) for every filing the state has seen with a
// non-NULL sic.  Shared between versions; replaced (copied) by structural writes.
struct SecEdgarQ4Cohort {
  SecEdgarQ4StrDict adsh;
  std::vector<uint32_t> cik_id; // per adsh id, kQ4None when cik IS NULL
  std::vector<uint32_t> sic_id; // per adsh id
  std::vector<int32_t> ciks;    // cik value per cik id
  std::vector<int32_t> sics;    // sic value per sic id
  core::FlatHashMap<uint32_t> cik_index; // cik value -> id
  core::FlatHashMap<uint32_t> sic_index; // sic value -> id
  uint64_t Bytes() const {
    return adsh.Bytes() + cik_id.size() * 8 + ciks.size() * 4 + sics.size() * 4 +
           cik_index.memory_bytes() + sic_index.memory_bytes();
  }
};

// stmt / uom dictionaries (ids are stable and append-only).
struct SecEdgarQ4Sud {
  SecEdgarQ4StrDict stmts;
  SecEdgarQ4StrDict uoms;
  uint64_t Bytes() const { return stmts.Bytes() + uoms.Bytes(); }
};

// Record key limits (see Q4RecKey / Q4GroupKey).
inline constexpr uint32_t kQ4MaxAdsh = 1u << 22;
inline constexpr uint32_t kQ4MaxTv = 1u << 24;
inline constexpr uint32_t kQ4MaxStmt = 64;
inline constexpr uint32_t kQ4MaxUom = 1023;
inline constexpr uint32_t kQ4MaxSic = 4096;
inline constexpr uint32_t kQ4MaxTl = 0xFFFFFFu; // 0xFFFFFF encodes NULL

inline int64_t Q4RecKey(uint32_t adsh, uint32_t tv, uint32_t stmt, uint32_t uom) {
  return static_cast<int64_t>((static_cast<uint64_t>(adsh) << 42) |
                              (static_cast<uint64_t>(tv) << 18) |
                              (static_cast<uint64_t>(stmt) << 10) | uom);
}
inline int64_t Q4GroupKey(uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom) {
  const uint64_t t = tl == kQ4None ? kQ4MaxTl : tl;
  return static_cast<int64_t>((static_cast<uint64_t>(sic_id) << 40) | (t << 16) |
                              (static_cast<uint64_t>(stmt) << 10) | uom);
}

struct SecEdgarQ4NumRec {
  double hi = 0.0;
  double lo = 0.0;
  uint32_t nc = 0;    // non-NULL num values of this (adsh, tv, uom); 0 => absent
  uint32_t group = 0; // group id of (sic, tl, stmt, uom)
};

inline constexpr unsigned kQ4RecShardBits = 12;
using SecEdgarQ4PreMap = core::ShardedCowHashMap<uint32_t, kQ4RecShardBits>;
using SecEdgarQ4NumMap = core::ShardedCowHashMap<SecEdgarQ4NumRec, kQ4RecShardBits>;

struct SecEdgarQ4Bridge {
  std::shared_ptr<const SecEdgarQ4StrDict> tv_dict;   // "tag\0version"
  std::shared_ptr<const std::vector<uint32_t>> tv_tl; // tlabel id per tv (kQ4None = NULL)
  std::shared_ptr<const SecEdgarQ4Sud> sud;
  SecEdgarQ4PreMap pre;
  SecEdgarQ4NumMap num;
  uint64_t generation = 0;

  uint64_t Bytes() const {
    uint64_t b = pre.memory_bytes() + num.memory_bytes();
    if (tv_dict) b += tv_dict->Bytes();
    if (tv_tl) b += tv_tl->size() * 4;
    if (sud) b += sud->Bytes();
    return b;
  }
};

struct SecEdgarQ4Labels {
  SecEdgarQ4StrDict tlabels;
  uint64_t Bytes() const { return tlabels.Bytes(); }
};

inline constexpr uint32_t kQ4GroupShardSize = 256;

// Structure-of-arrays so the read-time candidate filter runs as a contiguous
// (AVX2) scan; a write clones one 256-group shard.
struct alignas(32) SecEdgarQ4GroupShard {
  double sum_hi[kQ4GroupShardSize] = {};
  double sum_lo[kQ4GroupShardSize] = {};
  int64_t count[kQ4GroupShardSize] = {};
  uint32_t distinct[kQ4GroupShardSize] = {};
  int32_t sic[kQ4GroupShardSize] = {};
  uint32_t tl[kQ4GroupShardSize] = {};
  uint32_t su[kQ4GroupShardSize] = {}; // stmt id << 16 | uom id; 0xFFFFFFFF = unused slot
};

inline constexpr unsigned kQ4PairShardBits = 12;
using SecEdgarQ4PairMap = core::ShardedCowHashMap<int64_t, kQ4PairShardBits>;
inline int64_t Q4PairKey(uint32_t group, uint32_t cik_id) {
  return static_cast<int64_t>((static_cast<uint64_t>(group) << 32) | cik_id);
}
using SecEdgarQ4GroupIndex = core::ShardedCowHashMap<uint32_t, 10>;

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

struct SecEdgarQ4Groups {
  SecEdgarQ4Groups() = default;
  SecEdgarQ4Groups(const SecEdgarQ4Groups &other)
      : labels(other.labels), sud(other.sud), gshards(other.gshards),
        group_count(other.group_count), gindex(other.gindex), pairs(other.pairs),
        generation(other.generation) {}
  SecEdgarQ4Groups &operator=(const SecEdgarQ4Groups &) = delete;

  std::shared_ptr<const SecEdgarQ4Labels> labels;
  std::shared_ptr<const SecEdgarQ4Sud> sud;
  std::vector<std::shared_ptr<const SecEdgarQ4GroupShard>> gshards;
  uint32_t group_count = 0;
  SecEdgarQ4GroupIndex gindex; // Q4GroupKey -> group id
  SecEdgarQ4PairMap pairs;     // Q4PairKey(group, cik id) -> joined rows
  uint64_t generation = 0;

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
    uint64_t b = gshards.size() * sizeof(SecEdgarQ4GroupShard);
    b += pairs.memory_bytes() + gindex.memory_bytes();
    if (labels) b += labels->Bytes();
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
