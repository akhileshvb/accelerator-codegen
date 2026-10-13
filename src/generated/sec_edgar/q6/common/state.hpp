#pragma once

// SEC-EDGAR Q6 aux state (parameterized by fy, stmt, uom).
//
//   SELECT s.name, p.stmt, n.tag, p.plabel, SUM(n.value), COUNT(*)
//   FROM num n JOIN sub s ON n.adsh = s.adsh
//              JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
//   WHERE n.uom = :uom AND p.stmt = :stmt AND s.fy = :fy AND n.value IS NOT NULL
//   GROUP BY s.name, p.stmt, n.tag, p.plabel
//   ORDER BY total_value DESC, s.name, n.tag, p.plabel LIMIT 200
//
// The state answers every (fy, stmt, uom): groups are keyed by
// (name, tag, plabel, combo) where combo = (fy, stmt, uom) is interned in a
// small table; each combo has its own top cache.
//
// Versions are cheap to derive from each other: copying a State copies
// directories (pointers) and one handle on the aggregate store; a write clones
// only the chunks / hash shards of the structure it touches and records
// O(touched) undo entries for the aggregates, so a num write and a publish are
// O(touched), never O(state).
//
//   Structure
//     - interned string pools (adsh, name, tag, version, plabel, stmt, uom):
//       ChunkedBlobMap id -> bytes plus a ShardedCowHashMap hash -> id (exact:
//       collisions probe and compare).
//     - (tag, version) pairs: tv_index (tag_id, version_id) -> tv_id, tv_tag.
//     - adsh_name / adsh_fy: ChunkedCowArray<uint32_t> columns. An adsh is in
//       the cohort when its sub row has a non-NULL fy.
//     - runs: ChunkedBlobMap adsh_id -> the adsh's bridge run
//         [u32 n][u32 tv * n][u32 stmt * n][u32 plabel * n]
//       one entry per pre row (stmt, tag, version non-NULL), sorted by
//       (tv, stmt, plabel); duplicate rows are repeated entries.
//     - groups: ChunkedCowArray<{name,tag,plabel}> + group_combo (append-only,
//       ids stable) and a hash index. A num contribution (adsh, tag, version,
//       uom) fans out to one group per matching run entry:
//       (adsh name, tag, plabel, combo(adsh fy, entry stmt, uom)).
//   Aggregates: SecEdgarQ6AggStore, a persistent array (one flat array shared
//     by all versions plus per-version undo logs; "rerooting" moves the flat
//     array to the version being read). A sum is a double-double (hi + lo,
//     error-free additions), so adding and later removing a value restores the
//     sum exactly and the order of writes does not change it; the answer is hi.
//   Top caches (one per combo): output-ordered ids (with their sum and count)
//     of every active group of the combo whose sum >= a lower bound, so a
//     200-row answer is a prefix read that never touches the aggregate store.
//
// Id widths: pool ids and group ids are dense 32-bit counters, checked on every
// allocation (a std::length_error, never a silent wrap).

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/prims/chunked_blob_map.hpp"
#include "accelerator_codegen/stateful/prims/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/prims/cow_box.hpp"
#include "accelerator_codegen/stateful/prims/flat_hash_map.hpp"
#include "accelerator_codegen/stateful/prims/sharded_cow_open_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ---------------------------------------------------------------------------
// Copy-on-write building blocks. Copying any of them copies one pointer; the
// first mutation of a shared one clones its (directory-level) contents.
// ---------------------------------------------------------------------------
template <typename T, unsigned kShift> class SecEdgarQ6BoxedArray {
public:
  using Chunk = typename core::ChunkedCowArray<T, kShift>::Chunk;
  T Get(int64_t key) const { return box_->Get(key); }
  T &Mutable(int64_t key) { return box_.Mutable().Mutable(key); }
  bool Set(int64_t key, const T &value) { return box_.Mutable().Set(key, value); }
  size_t NumChunks() const { return box_->NumChunks(); }
  const Chunk *ChunkAt(size_t ci) const { return box_->ChunkAt(ci); }
  size_t MemoryBytes() const { return box_->MemoryBytes(); }

private:
  core::CowBox<core::ChunkedCowArray<T, kShift>> box_;
};

class SecEdgarQ6BoxedBlobMap {
public:
  bool Get(int64_t key, std::string_view &out) const { return box_->Get(key, out); }
  bool Set(int64_t key, std::string_view bytes) { return box_.Mutable().Set(key, bytes); }
  void Erase(int64_t key) { box_.Mutable().Erase(key); }
  void Shrink() { box_.Mutable().Shrink(); }
  size_t size() const { return box_->size(); }
  size_t MemoryBytes() const { return box_->MemoryBytes(); }

private:
  core::CowBox<core::ChunkedBlobMap> box_;
};

inline uint64_t SecEdgarQ6Mix64(uint64_t x) noexcept {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDULL;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ULL;
  x ^= x >> 33;
  return x;
}

// Key -> u32 indexes: key stored in the slot, prefetchable lookups. The empty
// marker is a key value that is never inserted (ids stay below 0xFFFFFFFE).
struct SecEdgarQ6U64IndexTraits {
  static uint64_t Hash(const uint64_t &key) noexcept { return SecEdgarQ6Mix64(key); }
  static uint64_t Empty() noexcept { return ~uint64_t{0}; }
};
// Keys that already are well-mixed hashes (string pool index).
struct SecEdgarQ6HashKeyTraits {
  static uint64_t Hash(const uint64_t &key) noexcept { return key; }
  static uint64_t Empty() noexcept { return ~uint64_t{0}; }
};
using SecEdgarQ6U64Index = core::ShardedCowOpenMap<uint64_t, uint32_t, 8, SecEdgarQ6U64IndexTraits>;

// ---------------------------------------------------------------------------
// Interned string pool (ids start at 1, 0 is reserved for NULL / "absent").
// ---------------------------------------------------------------------------
class SecEdgarQ6StringPool {
public:
  static uint64_t Hash(const char *data, size_t size) noexcept;
  static uint64_t Hash(std::string_view s) noexcept {
    return Hash(s.data(), s.size());
  }

  uint32_t Find(std::string_view s) const { return FindHashed(Hash(s), s); }
  uint32_t FindHashed(uint64_t hash, std::string_view s) const;
  uint32_t Intern(std::string_view s);

  // Valid until the next Intern on this pool object (a published pool never
  // changes).
  std::string_view View(uint32_t id) const {
    std::string_view out;
    data_->strings.Get(id, out);
    return out;
  }
  uint32_t Count() const noexcept { return data_->count; }
  size_t Bytes() const;
  void Shrink() { data_.Mutable().strings.Shrink(); }

private:
  struct Data {
    core::ChunkedBlobMap strings;
    // hash(+probe) -> id. A key holds at most one id; equal hashes probe to
    // the next key and compare the strings, so lookups are exact.
    core::ShardedCowOpenMap<uint64_t, uint32_t, 6, SecEdgarQ6HashKeyTraits> index;
    uint32_t count = 0;
  };
  core::CowBox<Data> data_;
};

struct SecEdgarQ6Group {
  uint32_t name = 0;   // 0 = NULL
  uint32_t tag = 0;
  uint32_t plabel = 0; // 0 = NULL
};

// (fy, stmt, uom) of a group; stmt / uom are pool ids.
struct SecEdgarQ6ComboKey {
  int32_t fy = 0;
  uint32_t stmt = 0;
  uint32_t uom = 0;
  bool operator<(const SecEdgarQ6ComboKey &o) const {
    return std::tie(fy, stmt, uom) < std::tie(o.fy, o.stmt, o.uom);
  }
};
struct SecEdgarQ6Combos {
  std::vector<SecEdgarQ6ComboKey> keys;
  std::map<SecEdgarQ6ComboKey, uint32_t> index;
};

// A bridge hit: the adsh's run (see the layout above) and the entries of one tv.
struct SecEdgarQ6Hit {
  std::string_view run;
  uint32_t adsh_id = 0;
  uint32_t tag_id = 0;
  uint32_t first = 0;
  uint32_t count = 0;
  bool Found() const noexcept { return count != 0; }
  uint32_t Stmt(uint32_t i) const noexcept;
  uint32_t Plabel(uint32_t i) const noexcept;
};

struct SecEdgarQ6GroupKey {
  uint32_t name = 0, tag = 0, plabel = 0, combo = 0;
  bool operator==(const SecEdgarQ6GroupKey &o) const noexcept {
    return name == o.name && tag == o.tag && plabel == o.plabel && combo == o.combo;
  }
};
uint64_t SecEdgarQ6GroupKeyHash(const SecEdgarQ6GroupKey &k) noexcept;
struct SecEdgarQ6GroupKeyTraits {
  static uint64_t Hash(const SecEdgarQ6GroupKey &k) noexcept { return SecEdgarQ6GroupKeyHash(k); }
  static SecEdgarQ6GroupKey Empty() noexcept {
    return {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  }
};
using SecEdgarQ6GroupIndex =
    core::ShardedCowOpenMap<SecEdgarQ6GroupKey, uint32_t, 8, SecEdgarQ6GroupKeyTraits>;

// ---------------------------------------------------------------------------
// Join structure (copy-on-write value type).
// ---------------------------------------------------------------------------
constexpr unsigned kQ6GroupShift = 14; // groups are append-only: big chunks

struct SecEdgarQ6Structure {
  static constexpr uint32_t kNoPayload = 0xFFFFFFFFu;
  static constexpr uint32_t kMaxGroups = 0x7FFFFFFFu;
  static constexpr uint32_t kNotCohort = 0xFFFFFFFFu; // adsh_name of an exited adsh

  SecEdgarQ6StringPool adshs;
  SecEdgarQ6StringPool names;
  SecEdgarQ6StringPool tags;
  SecEdgarQ6StringPool versions;
  SecEdgarQ6StringPool plabels;
  SecEdgarQ6StringPool stmts;
  SecEdgarQ6StringPool uoms;

  // canonical adsh ("dddddddddd-dd-dddddd" as an integer) -> adsh_id
  core::CowBox<SecEdgarQ6U64Index> adsh_keys;
  SecEdgarQ6BoxedArray<uint32_t, 12> adsh_name; // [adsh_id] -> name_id (0 = NULL)
  SecEdgarQ6BoxedArray<uint32_t, 12> adsh_fy;   // [adsh_id] -> fy (int32 bits)
  core::CowBox<SecEdgarQ6U64Index> tv_index;    // (tag_id << 32 | version_id) -> tv_id
  SecEdgarQ6BoxedArray<uint32_t, 12> tv_tag;    // [tv_id]   -> tag_id
  uint32_t tv_count = 0;
  SecEdgarQ6BoxedBlobMap runs;                  // [adsh_id] -> bridge run

  SecEdgarQ6BoxedArray<SecEdgarQ6Group, kQ6GroupShift> groups; // append-only
  SecEdgarQ6BoxedArray<uint32_t, kQ6GroupShift> group_combo;
  core::CowBox<SecEdgarQ6GroupIndex> group_index;
  std::shared_ptr<const SecEdgarQ6Combos> combos =
      std::make_shared<const SecEdgarQ6Combos>();
  uint32_t group_count = 0;
  uint32_t base_group_count = 0; // groups at the last full build

  uint32_t GroupCount() const noexcept { return group_count; }
  uint32_t ComboCount() const noexcept {
    return static_cast<uint32_t>(combos->keys.size());
  }
  uint32_t AdshCount() const noexcept { return adshs.Count(); }
  uint32_t TvCount() const noexcept { return tv_count; }

  static int64_t TvKey(uint32_t tag_id, uint32_t version_id) noexcept {
    return static_cast<int64_t>((static_cast<uint64_t>(tag_id) << 32) | version_id);
  }
  // tv id of (tag, version) or 0.
  uint32_t FindTv(std::string_view tag, std::string_view version) const;
  uint32_t FindTvIds(uint32_t tag_id, uint32_t version_id) const {
    const uint64_t key = static_cast<uint64_t>(TvKey(tag_id, version_id));
    const uint32_t *tv = tv_index->Find(key);
    return tv ? *tv : 0;
  }
  // adsh id (0 = unknown); canonical adsh strings use adsh_keys.
  uint32_t FindAdsh(std::string_view adsh) const;
  uint32_t FindAdshHashed(uint64_t pool_hash, std::string_view adsh) const;
  uint32_t InternAdsh(std::string_view adsh);
  uint32_t InternTv(uint32_t tag_id, uint32_t version_id);

  // The bridge entries for (adsh_id, tv_id), or an empty Hit.
  SecEdgarQ6Hit Lookup(uint32_t adsh_id, uint32_t tv_id) const;
  uint32_t FindCombo(int32_t fy, uint32_t stmt, uint32_t uom) const;
  uint32_t FindOrAddCombo(int32_t fy, uint32_t stmt, uint32_t uom);
  // Group id for the key or kNoPayload.
  uint32_t FindGroup(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id,
                     uint32_t combo) const;
  uint32_t FindOrAddGroup(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id,
                          uint32_t combo);
  // Appends a group known to be absent (bulk build).
  uint32_t AddNewGroup(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id,
                       uint32_t combo);
  bool IsCohort(std::string_view adsh) const {
    const uint32_t id = FindAdsh(adsh);
    return id != 0 && adsh_name.Get(id) != kNotCohort;
  }
  size_t Bytes() const;
};

class SecEdgarQ6StructureBuilder {
public:
  // Duplicate adsh keep the first row. Rows with a NULL fy are not passed.
  void AddCohort(std::string_view adsh, int32_t fy, std::string_view name,
                 bool name_is_null);
  // Ignored when the adsh is not in the cohort.
  void AddPre(std::string_view adsh, std::string_view tag, std::string_view version,
              std::string_view stmt, std::string_view plabel, bool plabel_is_null);
  SecEdgarQ6Structure Finish();

private:
  struct Entry {
    uint32_t adsh;
    uint32_t tv;
    uint32_t stmt;
    uint32_t plabel;
  };
  SecEdgarQ6Structure s_;
  std::vector<Entry> entries_;
};

// Writes the bridge runs of `entries` (sorted by adsh, tv, stmt, plabel) into
// s.runs. Entry layout: {adsh, tv, stmt, plabel}.
void SecEdgarQ6WriteRuns(SecEdgarQ6Structure &s, const uint32_t (*entries)[4],
                         size_t n);

// ---------------------------------------------------------------------------
// Double-double sum: value = hi + lo, |lo| <= ulp(hi) / 2 after normalizing.
// ---------------------------------------------------------------------------
struct SecEdgarQ6Sum {
  double hi = 0.0;
  double lo = 0.0;
};
SecEdgarQ6Sum SecEdgarQ6SumAdd(SecEdgarQ6Sum a, SecEdgarQ6Sum b) noexcept;

// ---------------------------------------------------------------------------
// Persistent aggregate array.
//
// All versions of one family share a single flat array that holds the values
// of the "root" version; every other version is a node with an undo log
// (gid, value) relative to the next node towards the root. Reading a version
// first reroots the family to it (applies the logs on the path, reversing
// them), so the latest version - the one maintenance keeps writing - reads
// and writes the flat array directly, and a version derived from it costs one
// log entry per written slot. Every access takes the family mutex.
// ---------------------------------------------------------------------------
struct SecEdgarQ6Agg {
  double hi = 0.0;
  double lo = 0.0;
  int64_t cnt = 0;
};

class SecEdgarQ6AggStore {
  struct Family;
  struct Node;

public:
  SecEdgarQ6AggStore();

  // Value of this version (zero for a never-written slot).
  SecEdgarQ6Agg Get(uint32_t gid) const;
  // fn(const SecEdgarQ6Agg *data, size_t size) on this version's values, under
  // the family lock (slots >= size read as zero). Do not touch any store of
  // the same family from inside fn.
  template <typename F> void Read(F &&fn) const {
    std::lock_guard<std::mutex> guard(Mutex());
    const auto view = RootView();
    fn(view.first, view.second);
  }

  // Exclusive write session on this version: the first write forks a new
  // version unless this handle is the only reference to its node.
  class Writer {
  public:
    explicit Writer(SecEdgarQ6AggStore &store);
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    SecEdgarQ6Agg Get(uint32_t gid) const {
      return gid < data_->size() ? (*data_)[gid] : SecEdgarQ6Agg{};
    }
    void Set(uint32_t gid, const SecEdgarQ6Agg &value);
    // Grows the array to n slots (no logging needed: new slots read as zero).
    void Reserve(size_t n);
    // Direct access for bulk builds on a private store (no undo log).
    SecEdgarQ6Agg *MutableData(size_t n);

  private:
    std::unique_lock<std::mutex> lock_;
    std::shared_ptr<Node> keep_; // the forked-from version (owns the log)
    std::vector<SecEdgarQ6Agg> *data_ = nullptr;
    std::vector<std::pair<uint32_t, SecEdgarQ6Agg>> *log_ = nullptr;
  };

  size_t Bytes() const; // flat array (shared) + this version's log

private:
  std::mutex &Mutex() const;
  std::pair<const SecEdgarQ6Agg *, size_t> RootView() const; // lock held
  static void Reroot(const std::shared_ptr<Node> &node);      // lock held

  std::shared_ptr<Node> node_;
};

// ---------------------------------------------------------------------------
// Aggregates + per-combo top caches.
// ---------------------------------------------------------------------------
constexpr size_t kQ6OutputLimit = 200;

struct SecEdgarQ6TopCache {
  // Output-ordered ids of exactly { active groups g of the combo :
  // !(sum[g] < lower_bound) }, with their sums and counts.
  std::vector<uint32_t> gids;
  std::vector<double> sums;
  std::vector<int64_t> cnts;
  double lower_bound = 0.0;
  bool complete = true; // lower_bound == -inf: gids holds every active group
  uint32_t base_size = 0; // size right after the last rescan / trim
};

// Top caches by combo id (chunked copy-on-write: a version that changes a few
// combos clones only their chunks).
class SecEdgarQ6Tops {
public:
  using Ptr = std::shared_ptr<const SecEdgarQ6TopCache>;
  size_t size() const noexcept { return size_; }
  const Ptr &operator[](size_t combo) const { return *arr_.Find(static_cast<int64_t>(combo)); }
  void Set(size_t combo, Ptr top) { arr_.Mutable(static_cast<int64_t>(combo)) = std::move(top); }
  void push_back(Ptr top) { Set(size_++, std::move(top)); }

private:
  core::ChunkedCowArray<Ptr, 6> arr_;
  size_t size_ = 0;
};

struct SecEdgarQ6State {
  SecEdgarQ6Structure structure;
  SecEdgarQ6AggStore aggs;
  uint64_t active_groups = 0;
  SecEdgarQ6Tops tops; // by combo id

  double Sum(uint32_t gid) const { return aggs.Get(gid).hi; }
  int64_t Count(uint32_t gid) const { return aggs.Get(gid).cnt; }
  size_t Bytes() const; // heap bytes owned by this version (+structure)
  bool TopUsable(uint32_t combo) const noexcept;
  const SecEdgarQ6TopCache &Top(uint32_t combo) const { return *tops[combo]; }
};

struct SecEdgarQ6GroupDelta {
  uint32_t gid;
  SecEdgarQ6Sum sum;
  int64_t cnt;
};

struct SecEdgarQ6OutputRow {
  std::string name;
  bool name_is_null = false;
  std::string stmt;
  std::string tag;
  std::string plabel;
  bool plabel_is_null = false;
  double total_value = 0.0;
  int64_t cnt = 0;
};

// Zeroed aggregates over `structure` (call SecEdgarQ6Rescan afterwards).
SecEdgarQ6State SecEdgarQ6MakeEmptyState(SecEdgarQ6Structure structure);

// Writes absolute (sum, count) for a group while building. Call
// SecEdgarQ6Rescan afterwards.
void SecEdgarQ6SetGroup(SecEdgarQ6State &state, uint32_t gid, double sum,
                        int64_t cnt);

// Recompute active_groups and every combo's top cache from scratch.
void SecEdgarQ6Rescan(SecEdgarQ6State &state);
// Recompute one combo's top cache (one pass over the groups).
void SecEdgarQ6RescanCombo(SecEdgarQ6State &state, uint32_t combo);

// Output-order comparator used for ordering and tie-breaks (same combo).
bool SecEdgarQ6GroupBefore(const SecEdgarQ6State &state, uint32_t a, uint32_t b);

// Resolve one num contribution (adsh, tag, version) -> its bridge hit.
SecEdgarQ6Hit SecEdgarQ6Resolve(const SecEdgarQ6Structure &s, std::string_view adsh,
                                std::string_view tag, std::string_view version);

// Expand a hit for one uom into group deltas for sum x sign (creates the
// missing groups / combos in `s`).
void SecEdgarQ6ExpandHit(SecEdgarQ6Structure &s, const SecEdgarQ6Hit &hit,
                         uint32_t uom_id, SecEdgarQ6Sum sum, int64_t cnt,
                         std::vector<SecEdgarQ6GroupDelta> &out);

struct SecEdgarQ6NumRef {
  std::string_view adsh, tag, version, uom;
  double value;
  int64_t sign;
  // pair: a value update of one row (+value, -old_value, count unchanged);
  // `sign` is ignored.
  double old_value = 0.0;
  bool pair = false;
};
// Resolves every ref against `s`, creating missing groups.
void SecEdgarQ6ResolveBatch(SecEdgarQ6Structure &s, const SecEdgarQ6NumRef *refs,
                            size_t n, std::vector<SecEdgarQ6GroupDelta> &out);

// Merge deltas by gid, apply them (new aggregate version) and maintain the top
// caches. Returns nullptr when `deltas` is empty after merging (nothing
// changes). `overflow` is set when a finite sum would leave the double range
// (the caller then rebuilds: removing such a contribution again is not exact).
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyDeltas(const SecEdgarQ6State &old_state,
                      std::vector<SecEdgarQ6GroupDelta> &deltas,
                      bool *overflow = nullptr);

// num inserts / deletes: resolves the refs (creating groups) and applies them.
// `overflow` is also set when too many unused groups accumulated (rebuild).
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyNums(const SecEdgarQ6State &old_state, const SecEdgarQ6NumRef *refs,
                    size_t n, bool *overflow = nullptr);

// Rows of the answer for one combo, or {} when the (fy, stmt, uom) is not in the
// state. `stmt` fills the stmt column.
std::vector<SecEdgarQ6OutputRow>
SecEdgarQ6TopRows(const SecEdgarQ6State &state, int64_t fy, std::string_view stmt,
                  std::string_view uom, size_t limit = kQ6OutputLimit);
std::vector<SecEdgarQ6OutputRow>
SecEdgarQ6TopRowsOfCombo(const SecEdgarQ6State &state, uint32_t combo,
                         std::string_view stmt, size_t limit = kQ6OutputLimit);
// Combo id for the parameter values or kNoPayload.
uint32_t SecEdgarQ6FindComboFor(const SecEdgarQ6Structure &s, int64_t fy,
                                std::string_view stmt, std::string_view uom);

// ---------------------------------------------------------------------------
// Incremental structural edits (sub cohort/name/fy changes, pre rows).
// ---------------------------------------------------------------------------
struct SecEdgarQ6NumSum {
  std::string tag, version, uom;
  double sum = 0.0;
  int64_t cnt = 0;
  double sum_lo = 0.0; // the sum is sum + sum_lo as a double-double
};
struct SecEdgarQ6PreEntry {
  std::string tag, version, stmt, plabel;
  bool plabel_is_null = false;
  uint32_t mult = 1;
};
// Post-transaction truth for one adsh plus its pre-transaction num sums.
struct SecEdgarQ6AdshEdit {
  std::string adsh;
  bool new_cohort = false;
  int32_t fy = 0;
  std::string name;
  bool name_is_null = false;
  std::vector<SecEdgarQ6PreEntry> pre;       // all stmt rows (new state)
  std::vector<SecEdgarQ6NumSum> old_nums;    // sums before the txn
  std::vector<SecEdgarQ6NumSum> new_nums;    // sums after the txn
};

// Edits a copy of the structure (only the touched chunks / shards are cloned),
// moves the affected adsh's contributions and applies the num refs of the other
// adsh, returning the next state. Returns nullptr when the edit cannot be
// applied safely (group garbage limit, inconsistent old state); the caller
// then rebuilds.
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyEdits(const SecEdgarQ6State &old_state,
                     const std::vector<SecEdgarQ6AdshEdit> &edits,
                     const std::vector<SecEdgarQ6NumRef> &other_refs,
                     bool *overflow = nullptr);

// Dispatch introspection for tests/benchmarks.
bool SecEdgarQ6UsesAvx2();
void SecEdgarQ6ForceScalar(bool force);

// Test hook: all ids of active groups of `combo` with !(sum < lb), unsorted,
// using the SIMD path when available (or scalar when forced).
void SecEdgarQ6CollectAtLeast(const SecEdgarQ6State &state, uint32_t combo,
                              double lb, std::vector<uint32_t> &out);

} // namespace accelerator_codegen::aux_state::query
