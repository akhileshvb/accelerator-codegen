#pragma once

// Compact aux state for SEC-EDGAR Q2.
//
//   SELECT s.name, n.tag, n.value FROM num n JOIN sub s ON n.adsh = s.adsh
//   JOIN (SELECT adsh, tag, MAX(value) FROM num WHERE uom = U AND value IS NOT NULL
//         GROUP BY adsh, tag) m ON (adsh, tag, value = max)
//   WHERE n.uom = U AND s.fy = F AND n.value IS NOT NULL
//   ORDER BY n.value DESC, s.name, n.tag LIMIT 100
//
// The state is independent of the fiscal-year parameter F (the fy filter is
// applied at read time through a per-adsh fy array) and covers the uom
// parameter U for every "covered" unit of measure (all of them by default; see
// SecEdgarQ2Config::max_uom_rows for the opt-out that leaves big units to the
// module's exact SQL path).
//
// Per (uom, adsh, tag) group we keep the top-2 distinct values with exact row
// counts plus a truncation flag.  That is enough to maintain MAX under
// inserts, deletes and updates without touching the base table, except when
// every listed value of a group is deleted (the group is then re-read).
//
// Layout (everything a write touches is copy-on-write):
//   * one immutable `Q2Base` (SoA, shared between versions) holds every group of
//     every materialised unit of measure, sorted by (uom, value desc);
//   * a small `Q2Overlay` holds the groups changed since the base was built
//     (copied per version: O(overlay), compacted into a new base when large);
//   * adsh / tag dictionaries are an immutable shared `core::StringDict` plus a
//     small private delta (`Q2Dict`), so a new string copies O(delta), not the
//     dictionary;
//   * the per-adsh fiscal year is a `core::ChunkedCowArray<int64_t>` and the
//     per-adsh filer name a `core::ChunkedBlobMap`: a sub write clones one chunk.
// Keys are order-preserving int64 images of the DOUBLE value (exact for NaN,
// +-inf, -0.0).  Counts are exact 32-bit row counts of one (group, value);
// ids are dense 32-bit dictionary ids (not packed keys).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "accelerator_codegen/stateful/core/chunked_blob_map.hpp"
#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/string_blob.hpp"

namespace accelerator_codegen::aux_state::query {

constexpr uint32_t kQ2None = 0xFFFFFFFFu;
constexpr int64_t kQ2NoFy = std::numeric_limits<int64_t>::min();

// Order-preserving map double -> int64.  NaN is the greatest value and equal
// to itself, -0.0 equals +0.0 (DuckDB semantics for MAX / = / ORDER BY).
inline int64_t Q2ValueKey(double v) {
  if (v != v) {
    return std::numeric_limits<int64_t>::max();
  }
  if (v == 0.0) {
    v = 0.0;
  }
  int64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits ^ ((bits >> 63) & std::numeric_limits<int64_t>::max());
}

inline double Q2KeyValue(int64_t key) {
  if (key == std::numeric_limits<int64_t>::max()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const int64_t bits = key ^ ((key >> 63) & std::numeric_limits<int64_t>::max());
  double v;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

inline uint64_t Q2GroupHash(uint32_t uom, uint32_t adsh, uint32_t tag) {
  uint64_t h = (static_cast<uint64_t>(adsh) << 32) | tag;
  h ^= static_cast<uint64_t>(uom) * 0xC2B2AE3D27D4EB4Full;
  h *= 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}

// String -> dense id dictionary, append-only.  Immutable shared base plus a
// small private delta (ids >= base size).  Copying a Q2Dict shares both
// pointers; the first GetOrAdd of a copy clones only the delta.
class Q2Dict {
public:
  static constexpr uint32_t kDeltaLimit = 1024;

  Q2Dict();
  uint32_t Size() const { return base_n_ + (delta_ ? static_cast<uint32_t>(delta_->size()) : 0); }
  uint32_t Find(std::string_view s) const;
  uint32_t GetOrAdd(std::string_view s);
  std::string_view Get(uint32_t id) const {
    return id < base_n_ ? base_->Get(id) : delta_->Get(id - base_n_);
  }
  bool NeedsCompaction() const { return delta_ && delta_->size() > kDeltaLimit; }
  // Folds the delta into a fresh base (ids unchanged).  Only on a private copy.
  void Compact();
  size_t HeapBytes() const;

private:
  std::shared_ptr<const core::StringDict> base_;
  std::shared_ptr<core::StringDict> delta_; // null when empty
  uint32_t base_n_ = 0;
};

// One (uom, adsh, tag) group: top-2 distinct values with exact counts.
// Invariant: rows not covered by (k0,c0)/(k1,c1) exist iff `trunc`, and all
// such rows have a value strictly below the smallest listed value.
struct Q2Group {
  int64_t k0 = 0;
  int64_t k1 = 0;
  uint32_t uom = 0;
  uint32_t adsh = 0;
  uint32_t tag = 0;
  uint32_t c0 = 0; // 0 => group has no rows (deleted / tombstone)
  uint32_t c1 = 0; // 0 => no second listed value
  bool trunc = false;
};

// Adds `n` rows with `key`.  False when a count would exceed 2^32-1 (the
// group is left unchanged).
bool Q2GroupInsert(Q2Group &g, int64_t key, uint64_t n = 1);
enum class Q2Removal : uint8_t { Ok, Lost, Inconsistent };
Q2Removal Q2GroupRemove(Q2Group &g, int64_t key);

// Open-addressing table of groups keyed by (uom, adsh, tag).
struct Q2GroupTable {
  std::vector<Q2Group> ents;
  std::vector<uint32_t> slots; // entry index + 1, 0 = empty

  int32_t Find(uint32_t uom, uint32_t adsh_id, uint32_t tag_id) const;
  // Upsert.
  void Put(const Q2Group &g);
  // Insert a group known to be absent (one probe instead of two).
  void Append(const Q2Group &g);
  size_t HeapBytes() const { return ents.capacity() * sizeof(Q2Group) + slots.capacity() * 4; }
};

// Immutable base: all groups sorted by (uom, k0 desc, adsh, tag) with a hash
// index; `uom_start[u] .. uom_start[u + 1]` is the slice of unit of measure u.
//
// Compact layout (about 31 bytes per group instead of 49): counts are one byte
// each -- `c0b` (255 = escape) and `c1b` (low 7 bits = count, 127 = escape,
// bit 7 = trunc) -- with the exact 32-bit counts of the rare escaped groups in
// a position-sorted side table.  The hash index is a linear-probe table at
// load factor ~0.7 addressed by fast range reduction (not a power of two).
struct Q2Base {
  static constexpr uint8_t kC0Esc = 255;
  static constexpr uint8_t kC1Esc = 127;
  static constexpr uint8_t kTruncBit = 128;

  uint32_t n = 0;
  std::vector<int64_t> k0, k1;
  std::vector<uint32_t> adsh, tag;
  std::vector<uint8_t> c0b, c1b;
  // Escaped counts, ascending by position.
  std::vector<uint32_t> e_pos, e_c0, e_c1;
  std::vector<uint32_t> uom_start{0};
  std::vector<uint32_t> slots; // position + 1, 0 = empty

  uint32_t UomCount() const { return static_cast<uint32_t>(uom_start.size() - 1); }
  uint32_t SliceBegin(uint32_t uom) const { return uom < UomCount() ? uom_start[uom] : n; }
  uint32_t SliceEnd(uint32_t uom) const { return uom < UomCount() ? uom_start[uom + 1] : n; }
  size_t SlotOf(uint64_t hash) const {
    return static_cast<size_t>(((hash >> 32) * static_cast<uint64_t>(slots.size())) >> 32);
  }
  // Exact row count of the top value at position i.
  uint32_t C0(uint32_t i) const {
    const uint8_t b = c0b[i];
    return b != kC0Esc ? b : EscapedC0(i);
  }
  uint32_t EscapedC0(uint32_t i) const;
  int32_t Find(uint32_t uom, uint32_t adsh_id, uint32_t tag_id) const;
  Q2Group Get(uint32_t i, uint32_t uom) const;
  // Sequential variant: positions must be passed in ascending order, `ec` is
  // the caller's cursor into the escape table (start at 0).
  Q2Group GetSeq(uint32_t i, uint32_t uom, size_t &ec) const;
  size_t HeapBytes() const;
  // Throws std::length_error beyond 2^32-2 groups.
  static std::shared_ptr<const Q2Base> Build(std::vector<Q2Group> &&groups,
                                             uint32_t uom_count);

private:
  void Fill(Q2Group &g, uint32_t i, size_t e) const;
};

// Groups changed since the base was built.  Copies share the dead bitmap's
// chunks (copy-on-write); the entry table is copied (it is small).
struct Q2Overlay {
  Q2GroupTable table;
  core::ChunkedCowArray<uint64_t, 6> dead{0}; // bit per base position that is shadowed
  uint64_t dead_count = 0;

  bool IsDead(uint32_t pos) const {
    const uint64_t *w = dead.Find(static_cast<int64_t>(pos >> 6));
    return w && ((*w >> (pos & 63)) & 1ull);
  }
  void MarkDead(uint32_t pos);
  size_t HeapBytes() const { return table.HeapBytes() + dead.MemoryBytes(); }
};

// Directory of units of measure.  Index = dictionary id.
struct Q2UomDir {
  core::StringDict names;
  std::vector<uint8_t> excluded; // 1 = not materialised
  size_t HeapBytes() const { return names.MemoryBytes() + excluded.capacity(); }
};

struct SecEdgarQ2State {
  Q2Dict adsh;
  Q2Dict tag;
  core::ChunkedCowArray<int64_t, 12> fy{kQ2NoFy}; // by adsh id
  core::ChunkedBlobMap names;                     // by adsh id; absent = NULL / no sub row
  std::shared_ptr<const Q2UomDir> uoms;
  std::shared_ptr<const Q2Base> base;
  Q2Overlay ov;

  // Current group (overlay first, then base).  False if the group is empty.
  bool Get(uint32_t uom, uint32_t adsh_id, uint32_t tag_id, Q2Group &out) const;
  void Put(const Q2Group &g);
  // `limit` 0 = automatic (grows with the base).
  bool NeedsCompaction(size_t limit) const;
  void Compact();
  std::vector<Q2Group> LiveGroups(uint32_t uom) const;
  uint64_t LiveGroupCount() const;

  int64_t FyOf(uint32_t adsh_id) const { return fy.Get(adsh_id); }
  void SetSub(uint32_t adsh_id, bool fy_is_null, int64_t fy_value, bool name_is_null,
              std::string_view name);
  void ClearSub(uint32_t adsh_id);
  bool HasName(uint32_t adsh_id) const { return names.Contains(adsh_id); }
  std::string_view Name(uint32_t adsh_id) const {
    std::string_view v;
    names.Get(adsh_id, v);
    return v;
  }
  size_t HeapBytes() const;
};

} // namespace accelerator_codegen::aux_state::query
