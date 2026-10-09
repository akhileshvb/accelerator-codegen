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
// Layout (everything a write touches is copy-on-write, per unit of measure):
//   * one `Q2Slice` per unit of measure (shared between versions; a write
//     clones only the slices it touches).  A slice is
//       - an immutable `Q2Base` (SoA) of its groups sorted by value desc with a
//         hash index on (adsh, tag), and
//       - an overlay of the groups changed since that base was built, split
//         into hash shards (each shared between versions and cloned on first
//         write), plus a copy-on-write bitmap of shadowed base positions.  The
//         overlay is folded into a new base (a linear merge, O(slice)) when it
//         exceeds a fraction of the slice, so the compaction is amortised over
//         many written groups;
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

// Hash of an (adsh, tag) pair inside one unit of measure.  Tables index with
// the high bits (`hash >> shift`).
inline uint64_t Q2PairHash(uint32_t adsh, uint32_t tag) {
  uint64_t h = ((static_cast<uint64_t>(adsh) << 32) | tag) * 0x9E3779B97F4A7C15ull;
  h ^= h >> 31;
  h *= 0xD6E8FEB86659FD93ull;
  h ^= h >> 29;
  return h;
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
// Invariant: rows not covered by (k0,c0)/(k1,c1) exist only if `trunc`, and
// all such rows have a value strictly below the smallest listed value.
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

// Open-addressing table of groups keyed by (adsh, tag) (one unit of measure).
struct Q2GroupTable {
  std::vector<Q2Group> ents;
  std::vector<uint32_t> slots; // entry index + 1, 0 = empty; power of two

  int32_t Find(uint32_t adsh_id, uint32_t tag_id) const;
  // Upsert.
  void Put(const Q2Group &g);
  // Insert a group known to be absent (one probe instead of two).
  void Append(const Q2Group &g);
  size_t HeapBytes() const { return ents.capacity() * sizeof(Q2Group) + slots.capacity() * 4; }
};

// Immutable base of one unit of measure: its groups sorted by k0 desc with a
// linear-probe hash index on (adsh, tag) (power-of-two, addressed by the high
// bits of Q2PairHash).
//
// Compact layout (about 26 bytes per group plus the index): counts are one
// byte each -- `c0b` (255 = escape) and `c1b` (low 7 bits = count, 127 =
// escape, bit 7 = trunc) -- with the exact 32-bit counts of the rare escaped
// groups in a position-sorted side table.
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
  std::vector<uint32_t> slots; // position + 1, 0 = empty
  unsigned shift = 64;         // slot = hash >> shift

  // Exact row count of the top value at position i.
  uint32_t C0(uint32_t i) const {
    const uint8_t b = c0b[i];
    return b != kC0Esc ? b : EscapedC0(i);
  }
  uint32_t EscapedC0(uint32_t i) const;
  int32_t Find(uint32_t adsh_id, uint32_t tag_id) const;
  Q2Group Get(uint32_t i, uint32_t uom) const;
  // Sequential variant: positions must be passed in ascending order, `ec` is
  // the caller's cursor into the escape table (start at 0).
  Q2Group GetSeq(uint32_t i, uint32_t uom, size_t &ec) const;
  size_t HeapBytes() const;

  // Appends groups in final (k0 desc) order; Finish() builds the index.
  void Reserve(size_t count);
  void PushBack(const Q2Group &g);
  void Finish();
  // Index slots for `count` groups.
  static unsigned ShiftFor(size_t count);
  // Throws std::length_error beyond 2^32-2 groups.  Any input order.
  static std::shared_ptr<const Q2Base> Build(std::vector<Q2Group> &&groups);

private:
  void Fill(Q2Group &g, uint32_t i, size_t e) const;
};

// The groups of one unit of measure: an immutable base plus an overlay of the
// groups changed since it was built.  Overlay shards and dead-bitmap chunks are
// shared between versions and cloned on first write (see Q2SliceWriter).
struct Q2Slice {
  uint32_t uom = 0;
  std::shared_ptr<const Q2Base> base;
  std::vector<std::shared_ptr<Q2GroupTable>> shards; // power-of-two count; immutable once published
  core::ChunkedCowArray<uint64_t, 6> dead{0}; // bit per base position that is shadowed
  uint64_t dead_count = 0;
  uint64_t ov_count = 0; // overlay entries (live + tombstones)
  unsigned shard_shift = 64;

  bool IsDead(uint32_t pos) const {
    const uint64_t *w = dead.Find(static_cast<int64_t>(pos >> 6));
    return w && ((*w >> (pos & 63)) & 1ull);
  }
  size_t ShardOf(uint64_t hash) const {
    return shard_shift >= 64 ? 0 : static_cast<size_t>(hash >> shard_shift);
  }
  // Current group (overlay first, then base).  False if the group is empty.
  bool Get(uint32_t adsh_id, uint32_t tag_id, Q2Group &out) const;
  std::vector<Q2Group> LiveGroups() const;
  uint64_t LiveGroupCount() const;
  // Overlay size beyond which the slice is compacted (`limit` 0 = automatic).
  bool NeedsCompaction(uint64_t limit) const;
  size_t HeapBytes() const;

  // New slice over `base` with an empty overlay.
  static std::shared_ptr<Q2Slice> Make(uint32_t uom, std::shared_ptr<const Q2Base> base);
  // Base with the overlay folded in (linear merge).
  std::shared_ptr<const Q2Base> Compacted() const;
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
  // By uom id; null = not materialised (excluded) or no group yet.
  std::vector<std::shared_ptr<const Q2Slice>> slices;

  const Q2Slice *Slice(uint32_t uom) const {
    return uom < slices.size() ? slices[uom].get() : nullptr;
  }
  // Current group.  False if the group is empty.
  bool Get(uint32_t uom, uint32_t adsh_id, uint32_t tag_id, Q2Group &out) const;
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

// Copy-on-write writer of one version's slices: clones a slice, an overlay
// shard or a dead-bitmap chunk the first time this version writes it, so a
// write batch costs O(touched shards), never O(state).
class Q2SliceWriter {
public:
  explicit Q2SliceWriter(SecEdgarQ2State &state) : state_(state) {}
  // Upsert of the group `g` (g.uom must be materialised).  `base_pos` is the
  // group's base position when the caller already knows it (-2 = unknown).
  void Put(const Q2Group &g, int32_t base_pos = -2);
  // Compacts every written slice whose overlay is too large.  Returns the
  // number of compactions.
  unsigned CompactIfNeeded(uint64_t limit);

private:
  Q2Slice &Writable(uint32_t uom);

  SecEdgarQ2State &state_;
  // Per written slice: which of its shards are private to this version.
  struct Owned {
    std::shared_ptr<Q2Slice> slice;
    std::vector<uint8_t> shard_owned;
  };
  std::vector<int32_t> owned_index_; // uom -> index into owned_ (-1 = shared)
  std::vector<Owned> owned_;
};

} // namespace accelerator_codegen::aux_state::query
