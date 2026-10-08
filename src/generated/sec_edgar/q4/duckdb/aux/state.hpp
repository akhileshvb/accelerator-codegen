#pragma once

// Compact, copy-on-write state for the SEC-EDGAR Q4 auxiliary accelerator.
//
// Query semantics (see benchmarks/sec_edgar/queries/q4.sql):
//   SELECT s.sic, t.tlabel, p.stmt, COUNT(DISTINCT s.cik), SUM(n.value), AVG(n.value)
//   FROM num n JOIN sub s JOIN tag t JOIN pre p
//   WHERE n.uom='USD' AND p.stmt='EQ' AND s.sic BETWEEN 4000 AND 4999
//     AND n.value IS NOT NULL AND t.abstract=0
//   GROUP BY s.sic, t.tlabel, p.stmt HAVING COUNT(DISTINCT s.cik) >= 2
//   ORDER BY total_value DESC LIMIT 500
//
// State model.  A "bridge entry" is one distinct (adsh, tag, version) key that
//   * has >= 1 pre row with stmt='EQ'                (mult = number of such rows)
//   * whose adsh is a sub row with sic in [4000,4999]
//   * whose (tag,version) is a tag row with abstract=0
// and carries the running aggregate of the qualifying num rows for the key
// (ns = SUM(value), nc = COUNT(value) over uom='USD' AND value IS NOT NULL).
// Every joined result row is one (num row x pre row), so the contribution of an
// entry to its group (sic, tlabel) is: sum += mult*ns, count += mult*nc, and
// every joined row increments the (group, cik) reference count that backs
// COUNT(DISTINCT cik).  A num-row insert/delete/update is then O(1): look up
// the entry and adjust ns/nc, the group aggregate and one refcount.
//
// Everything that a num write touches lives in fixed-size shards that are
// shared between published versions via shared_ptr; a write clones only the
// shards it touches (tens of KB) instead of the whole state.

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

// One row per distinct sub.adsh that owns at least one bridge entry.
struct SecEdgarQ4Cohort {
  SecEdgarQ4StrDict adsh;
  std::vector<uint32_t> cik_id; // per adsh id, kQ4None when cik IS NULL
  std::vector<int32_t> ciks;    // cik value per cik id
  uint64_t Bytes() const { return adsh.Bytes() + cik_id.size() * 4 + ciks.size() * 4; }
};

struct SecEdgarQ4Entry {
  double ns_hi = 0.0;
  double ns_lo = 0.0;
  uint32_t adsh_id = 0;
  uint32_t tv_id = 0;
  uint32_t group = 0;
  uint32_t mult = 0;  // 0 => dead slot (only exists transiently while rebuilding)
  int64_t nc = 0;     // qualifying num rows for this key
};

inline constexpr unsigned kQ4EntryShardBits = 10;
inline constexpr uint32_t kQ4EntryShardSize = 1u << kQ4EntryShardBits;
using SecEdgarQ4EntryArray = core::ChunkedCowArray<SecEdgarQ4Entry, kQ4EntryShardBits>;

struct SecEdgarQ4Bridge {
  SecEdgarQ4Bridge() = default;
  std::shared_ptr<const SecEdgarQ4StrDict> tv_dict;       // "tag\0version"
  std::shared_ptr<const std::vector<uint32_t>> tv_tl;     // tlabel id per tv (kQ4None = NULL)
  std::shared_ptr<const std::vector<uint32_t>> index;     // open addressing: entry id
  SecEdgarQ4EntryArray entries;  // copy-on-write chunks of 1024 entries
  uint32_t entry_count = 0;
  uint64_t generation = 0;

  const SecEdgarQ4Entry &At(uint32_t id) const {
    return *entries.Find(id);
  }
  static uint64_t KeyHash(uint32_t adsh_id, uint32_t tv_id) {
    return Q4Mix((static_cast<uint64_t>(adsh_id) << 32) | tv_id);
  }
  uint32_t Lookup(uint32_t adsh_id, uint32_t tv_id) const {
    if (!index || index->empty()) {
      return kQ4None;
    }
    const auto &idx = *index;
    const size_t mask = idx.size() - 1;
    for (size_t i = static_cast<size_t>(KeyHash(adsh_id, tv_id)) & mask;;
         i = (i + 1) & mask) {
      const uint32_t id = idx[i];
      if (id == kQ4None) {
        return kQ4None;
      }
      const auto &en = At(id);
      if (en.adsh_id == adsh_id && en.tv_id == tv_id) {
        return id;
      }
    }
  }
  uint64_t Bytes() const {
    uint64_t b = entry_count * sizeof(SecEdgarQ4Entry);
    if (tv_dict) b += tv_dict->Bytes();
    if (tv_tl) b += tv_tl->size() * 4;
    if (index) b += index->size() * 4;
    return b;
  }
};

struct SecEdgarQ4Labels {
  SecEdgarQ4StrDict tlabels;
  uint64_t Bytes() const { return tlabels.Bytes(); }
};

inline constexpr uint32_t kQ4GroupShardSize = 256;

// Structure-of-arrays so the top-500 candidate filter (distinct >= 2) runs as
// a contiguous (AVX2) scan over `distinct`; a write clones one 256-group shard.
struct alignas(32) SecEdgarQ4GroupShard {
  double sum_hi[kQ4GroupShardSize] = {};
  double sum_lo[kQ4GroupShardSize] = {};
  int64_t count[kQ4GroupShardSize] = {};
  uint32_t distinct[kQ4GroupShardSize] = {};
  int32_t sic[kQ4GroupShardSize] = {};
  uint32_t tl[kQ4GroupShardSize] = {};
};

// (group, cik id) -> number of joined rows, as one copy-on-write sharded hash
// map (4096 shards: a write clones only the shards it touches).  The key is
// the exact pair (group id << 32 | cik id); zero counts are inert.
inline constexpr unsigned kQ4PairShardBits = 12;
using SecEdgarQ4PairMap = core::ShardedCowHashMap<int64_t, kQ4PairShardBits>;
inline int64_t Q4PairKey(uint32_t group, uint32_t cik_id) {
  return static_cast<int64_t>((static_cast<uint64_t>(group) << 32) | cik_id);
}

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
      : labels(other.labels), gshards(other.gshards), group_count(other.group_count),
        pairs(other.pairs), generation(other.generation) {}
  SecEdgarQ4Groups &operator=(const SecEdgarQ4Groups &) = delete;

  std::shared_ptr<const SecEdgarQ4Labels> labels;
  std::vector<std::shared_ptr<const SecEdgarQ4GroupShard>> gshards;
  uint32_t group_count = 0;
  SecEdgarQ4PairMap pairs;
  uint64_t generation = 0;

  // Lazily computed top-500 (valid for exactly this immutable object).
  mutable std::mutex cache_mutex;
  mutable std::shared_ptr<const SecEdgarQ4Result> cache;

  uint64_t Bytes() const {
    uint64_t b = gshards.size() * sizeof(SecEdgarQ4GroupShard);
    b += pairs.memory_bytes();
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
