#pragma once

// Compact, exact auxiliary state for SEC-EDGAR Q24 (anti-join of num on pre).
//
//   SELECT n.tag, n.version, COUNT(*), SUM(n.value)
//   FROM num n LEFT JOIN pre p USING (adsh, tag, version)
//   WHERE n.uom=$2 AND n.ddate BETWEEN $1*10000+101 AND $1*10000+1231
//     AND n.value IS NOT NULL AND p.adsh IS NULL
//   GROUP BY n.tag, n.version HAVING COUNT(*) > 10
//   ORDER BY cnt DESC LIMIT 100
//
// State layout (engine neutral, no DuckDB types):
//   * PreBase    immutable exact set of pre (adsh,tag,version) triples:
//                three string dictionaries + (tag,version) pair index + CSR of
//                sorted pair ids per adsh. ~8 bytes/triple smaller than the old
//                per-string layout and shared (never copied) between versions.
//   * PreOverlay small copy-on-write set of triples inserted after Build.
//   * GroupState the *orphan* side: qualifying num rows with no pre match,
//                kept as per-triple and per-(tag,version) accumulators in a
//                sharded copy-on-write map so a maintenance step copies only
//                the touched shards. Only these rows can reach the result.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "prims/sharded_cow_map.hpp"
#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::query {

// Query constants.
inline constexpr int64_t kSecEdgarQ24MinimumCount = 11; // HAVING COUNT(*) > 10
inline constexpr std::size_t kSecEdgarQ24OutputLimit = 100;

// Nullable string view (valid only for the duration of the call that got it).
struct Q24Str {
  const char *data = nullptr;
  uint32_t size = 0;
  bool null = true;

  static Q24Str Null() { return {}; }
  static Q24Str Of(const char *d, std::size_t n) {
    return {d, static_cast<uint32_t>(n), false};
  }
  static Q24Str Of(const std::string &s) { return Of(s.data(), s.size()); }
  std::string_view View() const { return {data, size}; }
};

uint64_t Q24HashBytes(const char *data, std::size_t size);

// Test hook: force the scalar (false) or runtime-dispatched SIMD (true) slice
// search. SIMD is only ever used when the CPU reports AVX2.
void Q24SetSimdEnabled(bool enabled);
bool Q24SimdActive();

// Append-only string dictionary with exact lookup (arena + open addressing).
class Q24StringDict {
public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  uint32_t Find(const char *data, uint32_t size) const {
    return Find(data, size, Q24HashBytes(data, size));
  }
  uint32_t Find(const char *data, uint32_t size, uint64_t hash) const;
  // out[i] = Find(data[i], size[i]) for i < n (null data: kNone); hashes and
  // slot prefetches for the whole batch first, so the cache misses overlap.
  void FindBatch(const char *const *data, const uint32_t *size, std::size_t n,
                 uint32_t *out) const;
  uint32_t FindOrAdd(const char *data, uint32_t size) {
    return FindOrAdd(data, size, Q24HashBytes(data, size));
  }
  uint32_t FindOrAdd(const char *data, uint32_t size, uint64_t hash);
  void Prefetch(uint64_t hash) const {
    if (!slots_.empty()) __builtin_prefetch(&slots_[SlotOf(hash)]);
  }
  std::string_view Get(uint32_t id) const {
    return {arena_.data() + offs_[id], static_cast<std::size_t>(offs_[id + 1] - offs_[id])};
  }
  uint32_t size() const { return static_cast<uint32_t>(offs_.size() - 1); }
  std::size_t Bytes() const;
  // Drops growth slack: tight (~70% load) slot table, exact-capacity arena.
  void ShrinkToFit();
  // Serialization-free accessors used by the footprint encoder.
  const std::vector<char> &arena() const { return arena_; }
  const std::vector<uint64_t> &offsets() const { return offs_; }
  const std::vector<uint64_t> &slots() const { return slots_; }

private:
  void Rehash(std::size_t new_slot_count);
  std::size_t SlotOf(uint64_t hash) const {
    return static_cast<std::size_t>(((hash & 0xFFFFFFFFull) * slots_.size()) >> 32);
  }
  std::vector<char> arena_;
  std::vector<uint64_t> offs_{0};
  std::vector<uint64_t> slots_; // (hash high 32 bits << 32) | (id + 1)
};

// (tag id, version id) -> dense pair index.
class Q24PairIndex {
public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  uint32_t Find(uint64_t key) const;
  uint32_t FindOrAdd(uint64_t key);
  void Prefetch(uint64_t key) const {
    if (!slots_.empty()) __builtin_prefetch(&slots_[SlotOf(key)]);
  }
  std::size_t size() const { return pairs_.size(); }
  std::size_t Bytes() const;
  void ShrinkToFit();
  // Renumbers entries: entry `old` becomes `new_id_of_old[old]`.
  void Reorder(const std::vector<uint32_t> &new_id_of_old);
  const std::vector<uint64_t> &pairs() const { return pairs_; }
  const std::vector<uint32_t> &slots() const { return slots_; }

private:
  void Rehash(std::size_t new_slot_count);
  std::size_t SlotOf(uint64_t key) const;
  std::vector<uint64_t> pairs_;
  std::vector<uint32_t> slots_; // pair index + 1
};

// Rows of pre as dictionary ids; accumulated per worker then merged.
struct Q24RawPre {
  Q24StringDict adsh, tag, version;
  std::vector<uint32_t> rows; // adsh_id, tag_id, version_id triples
  // Last-adsh cache: pre is stored clustered by adsh.
  uint32_t last_adsh_id = Q24StringDict::kNone;
  std::string last_adsh;
  uint32_t last_ver_id = Q24StringDict::kNone;
  std::string last_ver;

  void Add(const Q24Str &a, const Q24Str &t, const Q24Str &v);
  // Batched ingest: hashes + slot prefetches for the whole batch first, then
  // the (dependent) dictionary inserts, to overlap cache misses.
  void AddBatch(const Q24Str *a, const Q24Str *t, const Q24Str *v, std::size_t n);
  std::size_t RowCount() const { return rows.size() / 3; }

private:
  void AddHashed(const Q24Str &a, const Q24Str &t, const Q24Str &v, uint64_t ht,
                 uint64_t hv);
  std::vector<uint64_t> hash_scratch_;

public:
};

struct Q24PreBase {
  Q24StringDict adsh, tag, version;
  // (tag id, version id) pairs numbered by descending frequency, so the most
  // common pairs get the smallest ids and the per-adsh id lists below fit in
  // narrow integer types (40% of triples use ids < 256, 88% ids < 65536).
  Q24PairIndex pairs;
  // Membership by pair-id width:
  //   id < 256         256-bit bitmap per adsh (one bit test)
  //   256 <= id < 64Ki CSR of sorted unique u16 ids per adsh
  //   id >= 64Ki       CSR of sorted unique u32 ids per adsh
  std::vector<uint64_t> bits256;            // 4 words per adsh
  std::vector<uint64_t> off16, off32;       // adsh.size()+1 each (64-bit)
  std::vector<uint16_t> ids16;
  std::vector<uint32_t> ids32;
  uint64_t triple_count = 0;

  bool Contains(const Q24Str &a, const Q24Str &t, const Q24Str &v) const;
  // Software-pipelined membership test for a batch: out[i] = Contains(i).
  // Stages (hash+prefetch, dictionary ids, pair id, CSR search) run over the
  // whole batch so independent cache misses overlap.
  void ContainsBatch(const Q24Str *a, const Q24Str *t, const Q24Str *v,
                     std::size_t n, uint8_t *out) const;
  // The same test on dictionary ids already resolved against adsh / tag /
  // version (Q24StringDict::kNone = the string is not in pre: no match).
  void ContainsIdsBatch(const uint32_t *aid, const uint32_t *tid,
                        const uint32_t *vid, std::size_t n, uint8_t *out) const;
  std::size_t Bytes() const;
};

// Merge worker-local raw parts into the immutable base. Consumes the parts.
// The per-adsh sorting runs on `services`' workers (serial if null).
std::shared_ptr<const Q24PreBase>
BuildQ24PreBase(std::vector<Q24RawPre> &parts,
                const core::AuxEngineServices *services = nullptr);

struct Q24PreOverlay {
  std::unordered_set<std::string> keys;    // encoded triples inserted after build (not in base)
  std::unordered_set<std::string> removed; // encoded base triples deleted after build
  std::size_t Bytes() const;
};

struct Q24PreState {
  std::shared_ptr<const Q24PreBase> base;
  std::shared_ptr<const Q24PreOverlay> overlay; // may be null
  // `key` is the encoded (adsh,tag,version) of the same row.
  bool Contains(const Q24Str &a, const Q24Str &t, const Q24Str &v,
                const std::string &key) const;
  std::size_t Bytes() const;
};

// ── key encoding ───────────────────────────────────────────────────────────
// component = 0x00 (null) | 0x01 u32 len bytes
void Q24AppendComponent(std::string &key, const Q24Str &s);
std::size_t Q24ComponentSize(const std::string &key, std::size_t offset);
Q24Str Q24ReadComponent(const std::string &key, std::size_t &offset);

// ── accumulators ───────────────────────────────────────────────────────────
// Neumaier-compensated running sum so add/remove cycles stay accurate.
struct Q24Acc {
  int64_t cnt = 0;
  double sum = 0.0;
  double comp = 0.0;

  void Add(double x) {
    const double t = sum + x;
    if (!std::isfinite(t)) { // inf / nan propagate like a plain double sum
      sum = t;
      return;
    }
    const double ax = x < 0 ? -x : x;
    const double as = sum < 0 ? -sum : sum;
    comp += (as >= ax) ? (sum - t) + x : (x - t) + sum;
    sum = t;
  }
  double Total() const { return sum + comp; }
};

// Per-(tag, version) result of one (year, uom) bucket.
struct Q24OutputRow {
  std::string tag, version;
  bool tag_null = false, version_null = false;
  int64_t count = 0;
  double total = 0.0;
};

// ── buckets ────────────────────────────────────────────────────────────────
// ddate is an INTEGER YYYYMMDD. `ddate BETWEEN Y*10000+101 AND Y*10000+1231`
// holds exactly when floor(ddate/10000) = Y and ddate mod 10000 is in
// [101, 1231]; every other value (month/day out of range, NULL) can never
// satisfy the predicate for any Y and is dropped from the state.
inline bool Q24DdateYear(int64_t ddate, int32_t &year) {
  if (ddate < INT32_MIN || ddate > INT32_MAX) return false;
  int64_t y = ddate / 10000;
  int64_t rem = ddate - y * 10000;
  if (rem < 0) {
    rem += 10000;
    --y;
  }
  if (rem < 101 || rem > 1231) return false;
  year = static_cast<int32_t>(y);
  return true;
}

// Bucket key = year (4 bytes) + enc(uom). Group key = bucket key + enc(tag)
// enc(version). Orphan key = enc(adsh) enc(tag) enc(version).
std::string Q24BucketKey(int32_t year, const Q24Str &uom);

// Shard of a key: its byte hash, so a maintenance step copies only the shards it
// touches (see core::ShardedCowMap).
struct Q24KeyHash {
  std::size_t operator()(const std::string &key) const {
    return static_cast<std::size_t>(Q24HashBytes(key.data(), key.size()));
  }
};

// One (year, uom) slice of an orphan triple's accumulators.
struct Q24OrphanBucket {
  int32_t year = 0;
  std::string uom;
  Q24Acc acc;
};
using Q24OrphanEntry = std::vector<Q24OrphanBucket>;
inline std::size_t Q24ExtraBytes(const Q24OrphanEntry &e) {
  std::size_t n = e.capacity() * sizeof(Q24OrphanBucket);
  for (const auto &b : e) n += b.uom.capacity();
  return n;
}

using Q24AccMap = core::ShardedCowMap<std::string, Q24Acc, Q24KeyHash>;
using Q24AccMapTxn = core::ShardedCowMapTxn<std::string, Q24Acc, Q24KeyHash>;
using Q24OrphanMap = core::ShardedCowMap<std::string, Q24OrphanEntry, Q24KeyHash>;
using Q24OrphanMapTxn = core::ShardedCowMapTxn<std::string, Q24OrphanEntry, Q24KeyHash>;

// bucket key -> group keys (bucket key + enc(tag) enc(version)) whose count
// passes HAVING COUNT(*) > 10. Per-bucket sets are shared between states.
using Q24HeavySet = std::unordered_set<std::string>;
using Q24HeavyMap = std::unordered_map<std::string, std::shared_ptr<const Q24HeavySet>>;

struct Q24GroupState {
  Q24OrphanMap orphans; // key = enc(adsh,tag,version): per (year, uom) accumulators
  Q24AccMap groups;     // key = group key (see above)
  Q24HeavyMap heavy;

  // Top-100 of one (year, uom) bucket (HAVING + ORDER BY cnt DESC), computed
  // lazily from the bucket's heavy set and cached per published state.
  std::shared_ptr<const std::vector<Q24OutputRow>>
  Result(const std::string &bucket_key) const;
  // Carries cached results of every bucket not in `touched` over from the
  // previous state (the maintenance step did not change them).
  void InheritResults(const Q24GroupState &prev,
                      const std::unordered_set<std::string> &touched);
  std::size_t Bytes() const;

private:
  mutable std::mutex mu_;
  mutable std::unordered_map<std::string,
                             std::shared_ptr<const std::vector<Q24OutputRow>>>
      cache_;
};

// Worker-local accumulation of orphan rows during Build.
class Q24OrphanAccumulator {
public:
  void Add(const Q24Str &a, const Q24Str &t, const Q24Str &v, int32_t year,
           const Q24Str &uom, double value);
  void MergeFrom(Q24OrphanAccumulator &other);
  std::size_t size() const { return orphans_.size(); }
  std::shared_ptr<const Q24GroupState> Finish();

private:
  std::unordered_map<std::string, Q24OrphanEntry> orphans_;
  std::string scratch_;
};

std::shared_ptr<const Q24GroupState>
MakeQ24GroupState(const std::unordered_map<std::string, Q24OrphanEntry> &orphans);

} // namespace accelerator_codegen::aux_state::query
