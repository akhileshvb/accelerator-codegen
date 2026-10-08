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
inline constexpr const char *kSecEdgarQ24Uom = "USD";
inline constexpr int32_t kSecEdgarQ24FirstDate = 20230101;
inline constexpr int32_t kSecEdgarQ24LastDate = 20231231;
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
    return {arena_.data() + offs_[id], offs_[id + 1] - offs_[id]};
  }
  uint32_t size() const { return static_cast<uint32_t>(offs_.size() - 1); }
  std::size_t Bytes() const;
  // Drops growth slack: tight (~70% load) slot table, exact-capacity arena.
  void ShrinkToFit();
  // Serialization-free accessors used by the footprint encoder.
  const std::vector<char> &arena() const { return arena_; }
  const std::vector<uint32_t> &offsets() const { return offs_; }
  const std::vector<uint64_t> &slots() const { return slots_; }

private:
  void Rehash(std::size_t new_slot_count);
  std::size_t SlotOf(uint64_t hash) const {
    return static_cast<std::size_t>(((hash & 0xFFFFFFFFull) * slots_.size()) >> 32);
  }
  std::vector<char> arena_;
  std::vector<uint32_t> offs_{0};
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
  std::vector<uint32_t> off16, off32;       // adsh.size()+1 each
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
  std::unordered_set<std::string> keys; // encoded triples inserted after build
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

class Q24AccMapTxn;

// Sharded copy-on-write map<string, Q24Acc>.
class Q24AccMap {
public:
  using Shard = std::unordered_map<std::string, Q24Acc>;
  Q24AccMap() = default;
  explicit Q24AccMap(std::size_t shard_count);
  const Q24Acc *Find(const std::string &key) const;
  std::size_t size() const { return size_; }
  std::size_t Bytes() const;
  template <typename F> void ForEach(F &&f) const {
    for (const auto &shard : shards_) {
      if (!shard) continue;
      for (const auto &entry : *shard) f(entry.first, entry.second);
    }
  }
  std::size_t shard_count() const { return shards_.size(); }

private:
  friend class Q24AccMapTxn;
  std::size_t ShardOf(const std::string &key) const {
    return static_cast<std::size_t>(Q24HashBytes(key.data(), key.size())) &
           (shards_.size() - 1);
  }
  std::vector<std::shared_ptr<const Shard>> shards_;
  std::size_t size_ = 0;
};

// Mutating view that clones a shard on first write.
class Q24AccMapTxn {
public:
  explicit Q24AccMapTxn(const Q24AccMap &base);
  Q24Acc *Find(const std::string &key);
  Q24Acc &GetOrCreate(const std::string &key);
  void Erase(const std::string &key);
  Q24AccMap Finish();
  bool dirty() const { return dirty_; }

private:
  Q24AccMap::Shard &Mutable(std::size_t shard);
  Q24AccMap map_;
  std::vector<std::shared_ptr<Q24AccMap::Shard>> owned_;
  bool dirty_ = false;
};

struct Q24OutputRow {
  std::string tag, version;
  bool tag_null = false, version_null = false;
  int64_t count = 0;
  double total = 0.0;
};

struct Q24GroupState {
  Q24AccMap orphans; // key = enc(adsh,tag,version)
  Q24AccMap groups;  // key = enc(tag,version)

  // Top-k result, computed lazily once per published state.
  const std::vector<Q24OutputRow> &Result() const;
  std::size_t Bytes() const;

private:
  mutable std::once_flag once_;
  mutable std::vector<Q24OutputRow> result_;
};

// Worker-local accumulation of orphan rows during Build.
class Q24OrphanAccumulator {
public:
  void Add(const Q24Str &a, const Q24Str &t, const Q24Str &v, double value);
  void MergeFrom(Q24OrphanAccumulator &other);
  std::size_t size() const { return orphans_.size(); }
  std::shared_ptr<const Q24GroupState> Finish();
  // For tests / diagnostics.
  const std::unordered_map<std::string, Q24Acc> &orphans() const {
    return orphans_;
  }

private:
  std::unordered_map<std::string, Q24Acc> orphans_;
  std::string scratch_;
};

std::shared_ptr<const Q24GroupState> MakeQ24GroupState(
    const std::unordered_map<std::string, Q24Acc> &orphans);

} // namespace accelerator_codegen::aux_state::query
