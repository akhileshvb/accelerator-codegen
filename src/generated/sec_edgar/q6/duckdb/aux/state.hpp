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
// Layout (all ids are dense 32-bit integers into interned string pools):
//
//   Structure  (immutable, shared between versions; rebuilt only when sub/pre
//               change the join shape)
//     - string pools: adsh (fy=2023 cohort), name, tag, (tag,version), plabel
//     - CSR "bridge": adsh_id -> sorted (tv_id -> group payload) runs. A payload
//       is the group id itself (single plabel, multiplicity 1) or an offset
//       into a small side table for the (rare) multi-label / duplicate case.
//     - group table: SoA (name_id, tag_id, plabel_id), sorted lexicographically.
//   Aggregates (copy-on-write): 1024-group chunks of SoA (sum, count). A write
//     clones only the touched chunks + the chunk directory.
//   Top cache (copy-on-write): output-ordered ids of every active group whose
//     sum >= a lower bound, so the 200-row answer is a prefix read.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
  uint32_t FindHashed(uint64_t hash, std::string_view s) const noexcept;
  void Prefetch(uint64_t hash) const noexcept {
    if (!table_.empty()) {
      __builtin_prefetch(&table_[hash & mask_]);
    }
  }
  uint32_t Intern(std::string_view s);

  std::string_view View(uint32_t id) const noexcept {
    return {data_.data() + offs_[id - 1],
            static_cast<size_t>(offs_[id] - offs_[id - 1])};
  }
  uint32_t Count() const noexcept {
    return static_cast<uint32_t>(offs_.size() - 1);
  }
  size_t Bytes() const noexcept;
  void ShrinkToFit();

private:
  void Grow();

  std::vector<char> data_;
  std::vector<uint32_t> offs_{0};
  std::vector<uint64_t> table_; // (hash_hi32 << 32) | id ; 0 == empty
  uint64_t mask_ = 0;
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

// ---------------------------------------------------------------------------
// Immutable join structure.
// ---------------------------------------------------------------------------
struct SecEdgarQ6Structure {
  static constexpr uint32_t kNoPayload = 0xFFFFFFFFu;
  static constexpr uint32_t kMultiFlag = 0x80000000u;
  static constexpr uint32_t kNotCohort = 0xFFFFFFFFu; // adsh_name of an exited adsh

  SecEdgarQ6StringPool adshs; // cohort only (fy = 2023)
  SecEdgarQ6StringPool names;
  SecEdgarQ6StringPool tags;
  SecEdgarQ6StringPool tvs;
  SecEdgarQ6StringPool plabels;

  std::vector<uint32_t> adsh_name; // [adsh_id] -> name_id (0 = NULL)
  std::vector<uint32_t> tv_tag;    // [tv_id]   -> tag_id
  std::vector<uint32_t> csr_off;   // [adsh_id] run start (size adsh_count + 2)
  std::vector<uint32_t> csr_tv;    // sorted within each run
  std::vector<uint32_t> csr_pay;
  std::vector<uint32_t> multi;     // [k, gid0, mult0, gid1, mult1, ...]*

  // Groups: sorted by (name,tag,plabel) at build time, append-only afterwards
  // (gids are stable across structural edits; unused groups just stay inactive).
  std::vector<uint32_t> g_name;
  std::vector<uint32_t> g_tag;
  std::vector<uint32_t> g_plabel;
  std::vector<uint32_t> group_index; // open addressing, gid + 1 (0 = empty)
  uint32_t base_group_count = 0;     // groups at the last full build
  uint32_t base_multi_size = 0;

  uint32_t GroupCount() const noexcept {
    return static_cast<uint32_t>(g_name.size());
  }
  uint32_t AdshCount() const noexcept { return adshs.Count(); }

  // Payload for (adsh_id, tv_id) or kNoPayload.
  uint32_t Lookup(uint32_t adsh_id, uint32_t tv_id) const noexcept;
  // Group id for (name, tag, plabel) ids or kNoPayload.
  uint32_t FindGroup(uint32_t name_id, uint32_t tag_id,
                     uint32_t plabel_id) const noexcept;
  uint32_t FindOrAddGroup(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id);
  void RebuildGroupIndex();
  bool IsCohort(std::string_view adsh) const noexcept {
    const uint32_t id = adshs.Find(adsh);
    return id != 0 && adsh_name[id] != kNotCohort;
  }
  size_t Bytes() const noexcept;
};

class SecEdgarQ6StructureBuilder {
public:
  // Rows are expected pre-filtered (fy = 2023 / stmt = 'IS'); duplicate adsh
  // keep the first name.
  void AddCohort(std::string_view adsh, std::string_view name, bool name_is_null);
  // Ignored when the adsh is not in the cohort.
  void AddPre(std::string_view adsh, std::string_view tag, std::string_view version,
              std::string_view plabel, bool plabel_is_null);
  std::shared_ptr<const SecEdgarQ6Structure> Finish();

private:
  struct Entry {
    uint32_t adsh;
    uint32_t tv;
    uint32_t plabel;
  };
  std::unique_ptr<SecEdgarQ6Structure> s_ = std::make_unique<SecEdgarQ6Structure>();
  std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// Copy-on-write aggregates + top cache.
// ---------------------------------------------------------------------------
constexpr uint32_t kQ6ChunkShift = 10;
constexpr uint32_t kQ6ChunkSize = 1u << kQ6ChunkShift;
constexpr uint32_t kQ6ChunkMask = kQ6ChunkSize - 1;
constexpr size_t kQ6OutputLimit = 200;

struct SecEdgarQ6AggChunk {
  alignas(64) double sum[kQ6ChunkSize];
  alignas(64) int64_t cnt[kQ6ChunkSize];
};

struct SecEdgarQ6TopCache {
  // Output-ordered ids of exactly { active groups g : !(sum[g] < lower_bound) }.
  std::vector<uint32_t> gids;
  double lower_bound = 0.0;
  bool complete = true; // lower_bound == -inf: gids holds every active group
};

struct SecEdgarQ6State {
  std::shared_ptr<const SecEdgarQ6Structure> structure;
  std::vector<std::shared_ptr<const SecEdgarQ6AggChunk>> chunks;
  uint64_t active_groups = 0;
  SecEdgarQ6TopCache top;

  double Sum(uint32_t gid) const noexcept {
    return chunks[gid >> kQ6ChunkShift]->sum[gid & kQ6ChunkMask];
  }
  int64_t Count(uint32_t gid) const noexcept {
    return chunks[gid >> kQ6ChunkShift]->cnt[gid & kQ6ChunkMask];
  }
  size_t Bytes() const noexcept; // heap bytes owned by this version (+structure)
  bool TopUsable() const noexcept; // top answers LIMIT 200 without a rescan
};

struct SecEdgarQ6GroupDelta {
  uint32_t gid;
  double sum;
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
SecEdgarQ6State SecEdgarQ6MakeEmptyState(
    std::shared_ptr<const SecEdgarQ6Structure> structure);

// Writes absolute (sum, count) for a group while building. Returns false on a
// bad group id. Call SecEdgarQ6Rescan afterwards.
void SecEdgarQ6SetGroup(SecEdgarQ6State &state, uint32_t gid, double sum,
                        int64_t cnt);

// Recompute active_groups and the top cache from scratch.
void SecEdgarQ6Rescan(SecEdgarQ6State &state);

// Output-order comparator used for ordering and tie-breaks.
bool SecEdgarQ6GroupBefore(const SecEdgarQ6State &state, uint32_t a, uint32_t b);

// Resolve one num contribution (adsh, tag, version) -> payload or kNoPayload.
uint32_t SecEdgarQ6Resolve(const SecEdgarQ6Structure &s, std::string_view adsh,
                           std::string_view tag, std::string_view version);

// Expand payloads for one (adsh, tag, version) into group deltas.
void SecEdgarQ6ExpandPayload(const SecEdgarQ6Structure &s, uint32_t payload,
                             double value, int64_t sign,
                             std::vector<SecEdgarQ6GroupDelta> &out);

// Batched resolve with software pipelining (prefetches pool/CSR cache lines).
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
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyDeltas(const SecEdgarQ6State &old_state,
                      std::vector<SecEdgarQ6GroupDelta> &deltas);

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

// Edits a copy of the structure (bridge runs, cohort, groups), moves the
// affected adsh's contributions and applies `extra` group deltas, returning the
// next state. Returns nullptr when the edit cannot be applied safely (growth
// limits); the caller then rebuilds.
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyEdits(const SecEdgarQ6State &old_state,
                     const std::vector<SecEdgarQ6AdshEdit> &edits,
                     std::vector<SecEdgarQ6GroupDelta> &extra);

// Dispatch introspection for tests/benchmarks.
bool SecEdgarQ6UsesAvx2();
void SecEdgarQ6ForceScalar(bool force);

// Test hook: all ids of active groups with !(sum < lb), unsorted, using the
// SIMD path when available (or scalar when forced).
void SecEdgarQ6CollectAtLeast(const SecEdgarQ6State &state, double lb,
                              std::vector<uint32_t> &out);

} // namespace accelerator_codegen::aux_state::query
