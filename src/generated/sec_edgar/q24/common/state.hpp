#pragma once

// Compact, exact auxiliary state for SEC-EDGAR Q24 (anti-join of num on pre).
//
//   SELECT n.tag, n.version, COUNT(*), SUM(n.value)
//   FROM num n LEFT JOIN pre p USING (adsh, tag, version)
//   WHERE n.uom='USD' AND n.ddate BETWEEN 20230101 AND 20231231
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

namespace accelerator_codegen::aux_state::query {

// Query constants (the query has no template parameters).
inline constexpr int64_t kSecEdgarQ24DefaultYear = 2023;
inline constexpr const char *kSecEdgarQ24DefaultUom = "USD";
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
  std::size_t Bytes() const;
};

// Merge worker-local raw parts into the immutable base. Consumes the parts.
std::shared_ptr<const Q24PreBase> BuildQ24PreBase(std::vector<Q24RawPre> &parts);

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

// Sharded copy-on-write hash map: a maintenance step copies only the shards
// it touches.
template <typename V> class Q24ShardedMapTxn;

template <typename V> class Q24ShardedMap {
public:
  using Shard = std::unordered_map<std::string, V>;
  Q24ShardedMap() = default;
  explicit Q24ShardedMap(std::size_t shard_count) {
    std::size_t n = 1;
    while (n < std::max<std::size_t>(1, shard_count)) n <<= 1;
    shards_.resize(n);
  }
  const V *Find(const std::string &key) const {
    if (shards_.empty()) return nullptr;
    const auto &shard = shards_[ShardOf(key)];
    if (!shard) return nullptr;
    const auto it = shard->find(key);
    return it == shard->end() ? nullptr : &it->second;
  }
  std::size_t size() const { return size_; }
  std::size_t Bytes() const {
    std::size_t total = shards_.capacity() * sizeof(shards_[0]);
    for (const auto &shard : shards_) {
      if (!shard) continue;
      total += sizeof(Shard) + shard->bucket_count() * sizeof(void *);
      for (const auto &e : *shard)
        total += e.first.capacity() + 32 + sizeof(V) + Q24ExtraBytes(e.second);
    }
    return total;
  }
  template <typename F> void ForEach(F &&f) const {
    for (const auto &shard : shards_) {
      if (!shard) continue;
      for (const auto &entry : *shard) f(entry.first, entry.second);
    }
  }
  std::size_t shard_count() const { return shards_.size(); }

private:
  friend class Q24ShardedMapTxn<V>;
  std::size_t ShardOf(const std::string &key) const {
    return static_cast<std::size_t>(Q24HashBytes(key.data(), key.size())) &
           (shards_.size() - 1);
  }
  std::vector<std::shared_ptr<const Shard>> shards_;
  std::size_t size_ = 0;
};

// Mutating view that clones a shard on first write.
template <typename V> class Q24ShardedMapTxn {
public:
  explicit Q24ShardedMapTxn(const Q24ShardedMap<V> &base)
      : map_(base), owned_(base.shards_.size()) {}
  // Read-only lookup (does not clone the shard or mark the txn dirty).
  const V *FindConst(const std::string &key) const { return map_.Find(key); }
  V *Find(const std::string &key) {
    const std::size_t s = map_.ShardOf(key);
    const auto &shard = map_.shards_[s];
    if (!shard || shard->find(key) == shard->end()) return nullptr;
    auto &mutable_shard = Mutable(s);
    dirty_ = true;
    return &mutable_shard.find(key)->second;
  }
  V &GetOrCreate(const std::string &key) {
    auto &shard = Mutable(map_.ShardOf(key));
    dirty_ = true;
    const auto result = shard.emplace(key, V());
    if (result.second) map_.size_++;
    return result.first->second;
  }
  void Erase(const std::string &key) {
    const std::size_t s = map_.ShardOf(key);
    if (!map_.shards_[s] || map_.shards_[s]->count(key) == 0) return;
    auto &shard = Mutable(s);
    dirty_ = true;
    map_.size_ -= shard.erase(key);
  }
  Q24ShardedMap<V> Finish() {
    owned_.clear();
    return std::move(map_);
  }
  bool dirty() const { return dirty_; }

private:
  typename Q24ShardedMap<V>::Shard &Mutable(std::size_t shard) {
    if (!owned_[shard]) {
      owned_[shard] =
          map_.shards_[shard]
              ? std::make_shared<typename Q24ShardedMap<V>::Shard>(*map_.shards_[shard])
              : std::make_shared<typename Q24ShardedMap<V>::Shard>();
      map_.shards_[shard] = owned_[shard];
    }
    return *owned_[shard];
  }
  Q24ShardedMap<V> map_;
  std::vector<std::shared_ptr<typename Q24ShardedMap<V>::Shard>> owned_;
  bool dirty_ = false;
};

inline std::size_t Q24ExtraBytes(const Q24Acc &) { return 0; }

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

using Q24AccMap = Q24ShardedMap<Q24Acc>;
using Q24AccMapTxn = Q24ShardedMapTxn<Q24Acc>;
using Q24OrphanMap = Q24ShardedMap<Q24OrphanEntry>;
using Q24OrphanMapTxn = Q24ShardedMapTxn<Q24OrphanEntry>;

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
