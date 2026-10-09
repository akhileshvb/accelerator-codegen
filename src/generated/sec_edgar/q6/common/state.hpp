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
// Everything below is copy-on-write: copying a State copies directories
// (pointers); a write clones only the 1024-key chunks / hash shards it
// touches, so a num write, a sub/pre write of a few adsh and a publish are all
// O(touched), never O(state).
//
//   Structure
//     - seven interned string pools (adsh, name, tag, (tag,version), plabel,
//       stmt, uom): ChunkedBlobMap id -> bytes plus a ShardedCowHashMap
//       hash -> id (exact: collisions probe and compare).
//     - adsh_name / adsh_fy / tv_tag: ChunkedCowArray<uint32_t> columns. An
//       adsh is in the cohort when its sub row has a non-NULL fy.
//     - runs: ChunkedBlobMap adsh_id -> the adsh's bridge run
//         [u32 n][u32 tv * n][u32 stmt * n][u32 plabel * n]
//       one entry per pre row (stmt, tag, version non-NULL), sorted by
//       (tv, stmt, plabel); duplicate rows are repeated entries.
//     - groups: ChunkedCowArray<{name,tag,plabel}> + group_combo (append-only,
//       ids stable) and a hash index. A num contribution (adsh, tag, version,
//       uom) fans out to one group per matching run entry:
//       (adsh name, tag, plabel, combo(adsh fy, entry stmt, uom)).
//   Aggregates: ChunkedCowArray columns sum_hi, sum_lo, cnt. A sum is a
//     double-double (hi + lo, error-free additions), so adding and later
//     removing a value restores the sum exactly and the order of writes does
//     not change it; the answer is hi.
//   Top caches (one per combo): output-ordered ids of every active group of the
//     combo whose sum >= a lower bound, so a 200-row answer is a prefix read.
//
// Id widths: pool ids and group ids are dense 32-bit counters, checked on every
// allocation (a std::length_error, never a silent wrap).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <string_view>
#include <tuple>
#include <vector>

#include "accelerator_codegen/stateful/core/chunked_blob_map.hpp"
#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

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
    strings_.Get(id, out);
    return out;
  }
  uint32_t Count() const noexcept { return count_; }
  size_t Bytes() const;
  void Shrink() { strings_.Shrink(); }

private:
  core::ChunkedBlobMap strings_;
  // hash(+probe) -> id. A key holds at most one id; equal hashes probe to the
  // next key and compare the strings, so lookups are exact.
  core::ShardedCowHashMap<uint32_t, 8> index_;
  uint32_t count_ = 0;
};

// (tag, version) composite key: stored as [u32 len(tag)] tag version.
class SecEdgarQ6TagVersionKey {
public:
  SecEdgarQ6TagVersionKey(std::string_view tag, std::string_view version);
  std::string_view View() const noexcept { return {ptr_, size_}; }
  uint64_t Hash() const noexcept {
    return SecEdgarQ6StringPool::Hash(ptr_, size_);
  }

private:
  char stack_[320];
  std::string heap_;
  const char *ptr_ = nullptr;
  size_t size_ = 0;
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

// ---------------------------------------------------------------------------
// Join structure (copy-on-write value type).
// ---------------------------------------------------------------------------
struct SecEdgarQ6Structure {
  static constexpr uint32_t kNoPayload = 0xFFFFFFFFu;
  static constexpr uint32_t kMaxGroups = 0x7FFFFFFFu;
  static constexpr uint32_t kNotCohort = 0xFFFFFFFFu; // adsh_name of an exited adsh

  SecEdgarQ6StringPool adshs;
  SecEdgarQ6StringPool names;
  SecEdgarQ6StringPool tags;
  SecEdgarQ6StringPool tvs;
  SecEdgarQ6StringPool plabels;
  SecEdgarQ6StringPool stmts;
  SecEdgarQ6StringPool uoms;

  core::ChunkedCowArray<uint32_t, 12> adsh_name; // [adsh_id] -> name_id (0 = NULL)
  core::ChunkedCowArray<uint32_t, 12> adsh_fy;   // [adsh_id] -> fy (int32 bits)
  core::ChunkedCowArray<uint32_t, 12> tv_tag;    // [tv_id]   -> tag_id
  core::ChunkedBlobMap runs;                      // [adsh_id] -> bridge run

  core::ChunkedCowArray<SecEdgarQ6Group, 10> groups; // append-only
  core::ChunkedCowArray<uint32_t, 10> group_combo;
  core::ShardedCowHashMap<uint32_t, 8> group_index;  // hash(+probe) -> gid
  std::shared_ptr<const SecEdgarQ6Combos> combos =
      std::make_shared<const SecEdgarQ6Combos>();
  uint32_t group_count = 0;
  uint32_t base_group_count = 0; // groups at the last full build

  uint32_t GroupCount() const noexcept { return group_count; }
  uint32_t ComboCount() const noexcept {
    return static_cast<uint32_t>(combos->keys.size());
  }
  uint32_t AdshCount() const noexcept { return adshs.Count(); }

  // The bridge entries for (adsh_id, tv_id), or an empty Hit.
  SecEdgarQ6Hit Lookup(uint32_t adsh_id, uint32_t tv_id) const;
  uint32_t FindCombo(int32_t fy, uint32_t stmt, uint32_t uom) const;
  uint32_t FindOrAddCombo(int32_t fy, uint32_t stmt, uint32_t uom);
  // Group id for the key or kNoPayload.
  uint32_t FindGroup(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id,
                     uint32_t combo) const;
  uint32_t FindOrAddGroup(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id,
                          uint32_t combo);
  bool IsCohort(std::string_view adsh) const {
    const uint32_t id = adshs.Find(adsh);
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

// ---------------------------------------------------------------------------
// Double-double sum: value = hi + lo, |lo| <= ulp(hi) / 2 after normalizing.
// ---------------------------------------------------------------------------
struct SecEdgarQ6Sum {
  double hi = 0.0;
  double lo = 0.0;
};
SecEdgarQ6Sum SecEdgarQ6SumAdd(SecEdgarQ6Sum a, SecEdgarQ6Sum b) noexcept;

// ---------------------------------------------------------------------------
// Copy-on-write aggregates + per-combo top caches.
// ---------------------------------------------------------------------------
constexpr uint32_t kQ6ChunkShift = 10;
constexpr uint32_t kQ6ChunkSize = 1u << kQ6ChunkShift;
constexpr uint32_t kQ6ChunkMask = kQ6ChunkSize - 1;
constexpr size_t kQ6OutputLimit = 200;

struct SecEdgarQ6TopCache {
  // Output-ordered ids of exactly { active groups g of the combo :
  // !(sum[g] < lower_bound) }.
  std::vector<uint32_t> gids;
  double lower_bound = 0.0;
  bool complete = true; // lower_bound == -inf: gids holds every active group
  uint32_t base_size = 0; // size right after the last rescan
};

struct SecEdgarQ6State {
  SecEdgarQ6Structure structure;
  core::ChunkedCowArray<double, kQ6ChunkShift> sum_hi;
  core::ChunkedCowArray<double, kQ6ChunkShift> sum_lo;
  core::ChunkedCowArray<int64_t, kQ6ChunkShift> cnt;
  uint64_t active_groups = 0;
  std::vector<std::shared_ptr<const SecEdgarQ6TopCache>> tops; // by combo id

  double Sum(uint32_t gid) const { return sum_hi.Get(gid); }
  int64_t Count(uint32_t gid) const { return cnt.Get(gid); }
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
};
// Resolves every ref against `s`, creating missing groups.
void SecEdgarQ6ResolveBatch(SecEdgarQ6Structure &s, const SecEdgarQ6NumRef *refs,
                            size_t n, std::vector<SecEdgarQ6GroupDelta> &out);

// Merge deltas by gid, apply them copy-on-write and maintain the top caches.
// Returns nullptr when `deltas` is empty after merging (nothing changes).
// `overflow` is set when a finite sum would leave the double range (the
// caller then rebuilds: removing such a contribution again is not exact).
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
