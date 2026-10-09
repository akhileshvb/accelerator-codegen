#pragma once

// SEC-EDGAR Q6 aux state.
//
//   SELECT s.name, p.stmt, n.tag, p.plabel, SUM(n.value), COUNT(*)
//   FROM num n JOIN sub s ON n.adsh = s.adsh
//              JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
//   WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 AND n.value IS NOT NULL
//   GROUP BY s.name, p.stmt, n.tag, p.plabel
//   ORDER BY total_value DESC, s.name, n.tag, p.plabel LIMIT 200
//
// Everything below is copy-on-write: copying a State copies directories
// (pointers); a write clones only the 1024-key chunks / hash shards it
// touches, so a num write, a sub/pre write of a few adsh and a publish are all
// O(touched), never O(state).
//
//   Structure (core primitives)
//     - five interned string pools (adsh = fy 2023 cohort, name, tag,
//       (tag,version), plabel): ChunkedBlobMap id -> bytes plus a
//       ShardedCowHashMap hash -> id (exact: collisions probe and compare).
//     - adsh_name / tv_tag: ChunkedCowArray<uint32_t> columns.
//     - runs: ChunkedBlobMap adsh_id -> the adsh's bridge run
//         [u32 n][u32 tv * n (sorted)][u32 payload * n][u32 multi trailer...]
//       (8 B per pre entry). A payload is the group id, or kMultiFlag | word
//       offset of a [k, gid0, mult0, ...] block in the run's own trailer (the
//       rare multi-label / duplicate-row case), so a run is self-contained and
//       rewritten whole when its adsh changes.
//     - groups: ChunkedCowArray<{name,tag,plabel}> (append-only, ids stable)
//       and a hash index.
//   Aggregates: ChunkedCowArray columns sum_hi, sum_lo, cnt (1024 groups per
//     chunk). A sum is a double-double (hi + lo, error-free additions), so
//     adding and later removing a value restores the sum exactly and the order
//     of writes does not change it; the answer is hi.
//   Top cache: output-ordered ids of every active group whose sum >= a lower
//     bound, so the 200-row answer is a prefix read.
//
// Id widths: pool ids and group ids are dense 32-bit counters (a pool holds up
// to 2^32-2 strings, groups up to 2^31-1), checked on every allocation (a
// std::length_error, never a silent wrap). Offsets inside blob chunks are
// per-chunk; counts and sums are 64-bit.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

// A bridge hit: the adsh's run (see the layout above) and the payload found in
// it. `run` stays valid while the Structure it came from is not mutated.
struct SecEdgarQ6Hit {
  std::string_view run;
  uint32_t payload = 0xFFFFFFFFu;
  bool Found() const noexcept { return payload != 0xFFFFFFFFu; }
};

// ---------------------------------------------------------------------------
// Join structure (copy-on-write value type).
// ---------------------------------------------------------------------------
struct SecEdgarQ6Structure {
  static constexpr uint32_t kNoPayload = 0xFFFFFFFFu;
  static constexpr uint32_t kMultiFlag = 0x80000000u;
  static constexpr uint32_t kMaxGroups = 0x7FFFFFFFu;
  static constexpr uint32_t kNotCohort = 0xFFFFFFFFu; // adsh_name of an exited adsh

  SecEdgarQ6StringPool adshs; // cohort only (fy = 2023), plus exited ids
  SecEdgarQ6StringPool names;
  SecEdgarQ6StringPool tags;
  SecEdgarQ6StringPool tvs;
  SecEdgarQ6StringPool plabels;

  core::ChunkedCowArray<uint32_t, 12> adsh_name; // [adsh_id] -> name_id (0 = NULL)
  core::ChunkedCowArray<uint32_t, 12> tv_tag;    // [tv_id]   -> tag_id
  core::ChunkedBlobMap runs;                      // [adsh_id] -> bridge run

  core::ChunkedCowArray<SecEdgarQ6Group, 10> groups; // append-only
  core::ShardedCowHashMap<uint32_t, 8> group_index;  // hash(+probe) -> gid
  uint32_t group_count = 0;
  uint32_t base_group_count = 0; // groups at the last full build

  uint32_t GroupCount() const noexcept { return group_count; }
  uint32_t AdshCount() const noexcept { return adshs.Count(); }

  // The bridge entry for (adsh_id, tv_id), or an empty Hit.
  SecEdgarQ6Hit Lookup(uint32_t adsh_id, uint32_t tv_id) const;
  // Group id for (name, tag, plabel) ids or kNoPayload.
  uint32_t FindGroup(uint32_t name_id, uint32_t tag_id,
                     uint32_t plabel_id) const;
  uint32_t FindOrAddGroup(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id);
  bool IsCohort(std::string_view adsh) const {
    const uint32_t id = adshs.Find(adsh);
    return id != 0 && adsh_name.Get(id) != kNotCohort;
  }
  size_t Bytes() const;
};

class SecEdgarQ6StructureBuilder {
public:
  // Rows are expected pre-filtered (fy = 2023 / stmt = 'IS'); duplicate adsh
  // keep the first name.
  void AddCohort(std::string_view adsh, std::string_view name, bool name_is_null);
  // Ignored when the adsh is not in the cohort.
  void AddPre(std::string_view adsh, std::string_view tag, std::string_view version,
              std::string_view plabel, bool plabel_is_null);
  SecEdgarQ6Structure Finish();

private:
  struct Entry {
    uint32_t adsh;
    uint32_t tv;
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
// Copy-on-write aggregates + top cache.
// ---------------------------------------------------------------------------
constexpr uint32_t kQ6ChunkShift = 10;
constexpr uint32_t kQ6ChunkSize = 1u << kQ6ChunkShift;
constexpr uint32_t kQ6ChunkMask = kQ6ChunkSize - 1;
constexpr size_t kQ6OutputLimit = 200;

struct SecEdgarQ6TopCache {
  // Output-ordered ids of exactly { active groups g : !(sum[g] < lower_bound) }.
  std::vector<uint32_t> gids;
  double lower_bound = 0.0;
  bool complete = true; // lower_bound == -inf: gids holds every active group
};

struct SecEdgarQ6State {
  SecEdgarQ6Structure structure;
  core::ChunkedCowArray<double, kQ6ChunkShift> sum_hi;
  core::ChunkedCowArray<double, kQ6ChunkShift> sum_lo;
  core::ChunkedCowArray<int64_t, kQ6ChunkShift> cnt;
  uint64_t active_groups = 0;
  SecEdgarQ6TopCache top;

  double Sum(uint32_t gid) const { return sum_hi.Get(gid); }
  int64_t Count(uint32_t gid) const { return cnt.Get(gid); }
  size_t Bytes() const; // heap bytes owned by this version (+structure)
  bool TopUsable() const noexcept; // top answers LIMIT 200 without a rescan
};

struct SecEdgarQ6GroupDelta {
  uint32_t gid;
  SecEdgarQ6Sum sum;
  int64_t cnt;
};

struct SecEdgarQ6OutputRow {
  std::string name;
  bool name_is_null = false;
  std::string stmt = "IS";
  std::string tag;
  std::string plabel;
  bool plabel_is_null = false;
  double total_value = 0.0;
  int64_t cnt = 0;
};

// Zeroed aggregates over `structure` (no top cache yet; call SecEdgarQ6Rescan).
SecEdgarQ6State SecEdgarQ6MakeEmptyState(SecEdgarQ6Structure structure);

// Writes absolute (sum, count) for a group while building. Call
// SecEdgarQ6Rescan afterwards.
void SecEdgarQ6SetGroup(SecEdgarQ6State &state, uint32_t gid, double sum,
                        int64_t cnt);

// Recompute active_groups and the top cache from scratch.
void SecEdgarQ6Rescan(SecEdgarQ6State &state);

// Output-order comparator used for ordering and tie-breaks.
bool SecEdgarQ6GroupBefore(const SecEdgarQ6State &state, uint32_t a, uint32_t b);

// Resolve one num contribution (adsh, tag, version) -> its bridge hit.
SecEdgarQ6Hit SecEdgarQ6Resolve(const SecEdgarQ6Structure &s, std::string_view adsh,
                                std::string_view tag, std::string_view version);

// Expand a hit into group deltas for `value` x `sign`.
void SecEdgarQ6ExpandPayload(const SecEdgarQ6Hit &hit, double value, int64_t sign,
                             std::vector<SecEdgarQ6GroupDelta> &out);

// Batched resolve (hashes first, then lookups).
struct SecEdgarQ6NumRef {
  std::string_view adsh, tag, version;
  double value;
  int64_t sign;
};
void SecEdgarQ6ResolveBatch(const SecEdgarQ6Structure &s,
                            const SecEdgarQ6NumRef *refs, size_t n,
                            std::vector<SecEdgarQ6GroupDelta> &out);

// Merge deltas by gid, apply them copy-on-write and maintain the top cache.
// Returns nullptr when `deltas` is empty after merging (nothing changes).
// `overflow` is set when a finite sum would leave the double range (the
// caller then rebuilds: removing such a contribution again is not exact).
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyDeltas(const SecEdgarQ6State &old_state,
                      std::vector<SecEdgarQ6GroupDelta> &deltas,
                      bool *overflow = nullptr);

// First `limit` rows of the answer (uses the top cache when usable).
std::vector<SecEdgarQ6OutputRow> SecEdgarQ6TopRows(const SecEdgarQ6State &state,
                                                   size_t limit = kQ6OutputLimit);

// ---------------------------------------------------------------------------
// Incremental structural edits (sub cohort/name changes, pre IS rows).
// ---------------------------------------------------------------------------
struct SecEdgarQ6NumSum {
  std::string tag, version;
  double sum = 0.0;
  int64_t cnt = 0;
  double sum_lo = 0.0; // the sum is sum + sum_lo as a double-double
};
struct SecEdgarQ6PreEntry {
  std::string tag, version, plabel;
  bool plabel_is_null = false;
  uint32_t mult = 1;
};
// Post-transaction truth for one adsh plus its pre-transaction num sums.
struct SecEdgarQ6AdshEdit {
  std::string adsh;
  bool new_cohort = false;
  std::string name;
  bool name_is_null = false;
  std::vector<SecEdgarQ6PreEntry> pre;       // IS rows (new state)
  std::vector<SecEdgarQ6NumSum> old_nums;    // USD sums before the txn
  std::vector<SecEdgarQ6NumSum> new_nums;    // USD sums after the txn
};

// Edits a copy of the structure (only the touched chunks / shards are cloned:
// the affected adsh's bridge runs, pool tails, new groups), moves the affected
// adsh's contributions and applies `extra` group deltas, returning the next
// state. Returns nullptr when the edit cannot be applied safely (group
// garbage limit); the caller then rebuilds.
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyEdits(const SecEdgarQ6State &old_state,
                     const std::vector<SecEdgarQ6AdshEdit> &edits,
                     std::vector<SecEdgarQ6GroupDelta> &extra,
                     bool *overflow = nullptr);

// Dispatch introspection for tests/benchmarks.
bool SecEdgarQ6UsesAvx2();
void SecEdgarQ6ForceScalar(bool force);

// Test hook: all ids of active groups with !(sum < lb), unsorted, using the
// SIMD path when available (or scalar when forced).
void SecEdgarQ6CollectAtLeast(const SecEdgarQ6State &state, double lb,
                              std::vector<uint32_t> &out);

} // namespace accelerator_codegen::aux_state::query
