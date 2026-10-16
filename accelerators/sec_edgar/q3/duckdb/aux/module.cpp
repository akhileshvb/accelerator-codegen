#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
#include <functional>
#include <variant>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define ACCEL_Q3_X86 1
#endif

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteWriter;

constexpr uint32_t kFormatVersion = 2;
constexpr uint32_t kStateMagic = 0x53303351; // Q30S
constexpr uint32_t kIndexMagic = 0x49303351; // Q30I
constexpr std::size_t kOutputLimit = 100;

// ---------------------------------------------------------------------------
// Row decoding
// ---------------------------------------------------------------------------

bool ReadDoubleCell(const core::AuxValue &cell, double &value) {
  if (std::holds_alternative<double>(cell.payload)) {
    value = std::get<double>(cell.payload);
    return true;
  }
  if (std::holds_alternative<int64_t>(cell.payload)) {
    value = static_cast<double>(std::get<int64_t>(cell.payload));
    return true;
  }
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
    value = static_cast<double>(decimal.value);
    for (int32_t scale = 0; scale < decimal.scale; ++scale) {
      value /= 10.0;
    }
    return true;
  }
  return false;
}

bool ReadIntCell(const core::AuxValue &cell, int32_t &value) {
  if (!std::holds_alternative<int64_t>(cell.payload)) {
    return false;
  }
  const auto raw = std::get<int64_t>(cell.payload);
  if (raw < std::numeric_limits<int32_t>::min() ||
      raw > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  value = static_cast<int32_t>(raw);
  return true;
}

struct NumRow {
  const std::string *adsh = nullptr;
  const std::string *uom = nullptr;
  double value = 0.0;
  bool relevant = false; // adsh, uom and value non-null
};

// num layout (see catalog MakeNumTableAuxRow): 0 adsh, 5 uom, 7 value
bool ReadNumRow(const core::AuxRow &row, const std::vector<core::ColumnId> &pos,
                NumRow &out) {
  const auto width = *std::max_element(pos.begin(), pos.end()) + 1;
  if (row.values.size() < static_cast<std::size_t>(width)) {
    return false;
  }
  const auto &adsh = row.values[pos[0]];
  const auto &uom = row.values[pos[1]];
  const auto &value = row.values[pos[2]];
  out.relevant = false;
  if (adsh.is_null || uom.is_null || value.is_null) {
    // Cells that can never qualify; still require sane payload types for the
    // non-null ones so a layout mismatch is detected.
    if (!adsh.is_null && !std::holds_alternative<std::string>(adsh.payload)) {
      return false;
    }
    return true;
  }
  if (!std::holds_alternative<std::string>(adsh.payload) ||
      !std::holds_alternative<std::string>(uom.payload)) {
    return false;
  }
  if (!ReadDoubleCell(value, out.value)) {
    return false;
  }
  out.adsh = &std::get<std::string>(adsh.payload);
  out.uom = &std::get<std::string>(uom.payload);
  out.relevant = true;
  return true;
}

struct SubRow {
  const std::string *adsh = nullptr;
  const std::string *name = nullptr; // null when NULL
  int32_t cik = 0;
  bool cik_null = false;
  bool tracked = false; // fy non-NULL: the filing can match some fy
  int32_t fy = 0;
};

// sub layout: 0 adsh, 1 name, 2 cik, 3 fy
bool ReadSubRow(const core::AuxRow &row, const std::vector<core::ColumnId> &pos,
                SubRow &out) {
  const auto width = *std::max_element(pos.begin(), pos.end()) + 1;
  if (row.values.size() < static_cast<std::size_t>(width)) {
    return false;
  }
  const auto &adsh = row.values[pos[0]];
  const auto &name = row.values[pos[1]];
  const auto &cik = row.values[pos[2]];
  const auto &fy = row.values[pos[3]];
  if (adsh.is_null || !std::holds_alternative<std::string>(adsh.payload)) {
    return false;
  }
  out.adsh = &std::get<std::string>(adsh.payload);
  if (name.is_null) {
    out.name = nullptr;
  } else if (std::holds_alternative<std::string>(name.payload)) {
    out.name = &std::get<std::string>(name.payload);
  } else {
    return false;
  }
  out.cik_null = cik.is_null;
  if (!cik.is_null && !ReadIntCell(cik, out.cik)) {
    return false;
  }
  if (fy.is_null) {
    out.tracked = false;
  } else {
    if (!ReadIntCell(fy, out.fy)) {
      return false;
    }
    out.tracked = true;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Copy-on-write working set
// ---------------------------------------------------------------------------

// Exact accumulation of doubles as sum + comp (double-double). Adding and
// removing the same contribution restores the value, so incremental
// maintenance does not drift from the from-scratch sum.
struct TwoD {
  double hi = 0.0;
  double lo = 0.0;
};

inline TwoD TwoSum(double a, double b) {
  const double s = a + b;
  const double bb = s - a;
  return {s, (a - (s - bb)) + (b - bb)};
}

// (s, c) += (hi, lo)
inline void DdAdd(double &s, double &c, double hi, double lo) {
  if (!std::isfinite(s) || !std::isfinite(hi)) {
    s += hi; // NaN / inf propagate exactly like a plain double sum
    c = 0.0;
    return;
  }
  const TwoD t = TwoSum(s, hi);
  if (!std::isfinite(t.hi)) {
    s = t.hi; // overflow
    c = 0.0;
    return;
  }
  const TwoD n = TwoSum(t.hi, c + t.lo + lo);
  s = n.hi;
  c = n.lo;
}

inline uint64_t Mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

inline uint64_t HashBytes(const char *data, std::size_t size, uint64_t seed) {
  uint64_t h = seed ^ (size * 0x9E3779B97F4A7C15ULL);
  std::size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t w;
    std::memcpy(&w, data + i, 8);
    h = Mix64(h ^ w) + 0x9E3779B97F4A7C15ULL;
  }
  uint64_t tail = 0;
  if (i < size) {
    std::memcpy(&tail, data + i, size - i);
  }
  return Mix64(h ^ tail);
}

constexpr std::size_t kLeafTarget = 128; // entries per index leaf
constexpr uint32_t kMinIndexBits = 4;
constexpr uint32_t kMaxIndexBits = 30;

uint32_t IndexBitsFor(uint64_t entries) {
  uint32_t bits = kMinIndexBits;
  while (bits < kMaxIndexBits && (entries >> bits) > kLeafTarget) {
    ++bits;
  }
  return bits;
}

class OverflowWork {
public:
  explicit OverflowWork(const SecEdgarQ3OverflowDict *base) {
    if (base) {
      dict = *base;
    }
  }

  bool Find(const char *data, std::size_t size, uint32_t &id) const {
    const int64_t h = static_cast<int64_t>(HashBytes(data, size, 0x51ed270b));
    const uint32_t *head = dict.by_hash.Find(h);
    for (uint32_t at = head ? *head : 0; at != 0;) {
      std::string_view stored;
      dict.strings.Get(at - 1, stored);
      if (stored.size() == size && std::memcmp(stored.data(), data, size) == 0) {
        id = at - 1;
        return true;
      }
      at = dict.next.Get(at - 1);
    }
    return false;
  }

  uint32_t Intern(const std::string &s) {
    uint32_t id;
    if (Find(s.data(), s.size(), id)) {
      return id;
    }
    if (dict.count == std::numeric_limits<uint32_t>::max() - 1) {
      throw std::overflow_error("SEC EDGAR Q3: overflow dictionary full");
    }
    id = dict.count++;
    const int64_t h = static_cast<int64_t>(HashBytes(s.data(), s.size(), 0x51ed270b));
    dict.strings.Set(id, s);
    auto &head = dict.by_hash[h];
    dict.next.Set(id, head);
    head = id + 1;
    return id;
  }

  const SecEdgarQ3OverflowDict &Dict() const { return dict; }

private:
  SecEdgarQ3OverflowDict dict;
};

class IndexWork {
public:
  explicit IndexWork(const SecEdgarQ3AdshIndex *base, bool bulk = false)
      : bulk(bulk), overflow(base ? &base->overflow : nullptr) {
    if (base && !base->tops.empty()) {
      top_bits = base->top_bits;
      mid_bits = base->mid_bits;
      tops = base->tops;
      entry_count = base->entry_count;
    } else {
      Init(kMinIndexBits);
    }
    owned_tops.resize(tops.size());
  }

  // Resolves an adsh string to its key. Non-conforming strings are interned
  // in the overflow dictionary when `create` is set.
  bool ResolveKey(const std::string &adsh, bool create, uint64_t &key) {
    if (SecEdgarQ3PackAdsh(adsh.data(), adsh.size(), key)) {
      return true;
    }
    uint32_t id;
    if (overflow.Find(adsh.data(), adsh.size(), id)) {
      key = kSecEdgarQ3OverflowBit | id;
      return true;
    }
    if (!create) {
      return false;
    }
    key = kSecEdgarQ3OverflowBit | overflow.Intern(adsh);
    return true;
  }

  const SecEdgarQ3Entry *Peek(uint64_t key) const {
    const auto leaf = LeafOf(key);
    const auto &mid = *(tops[leaf >> mid_bits]);
    return mid.leaves[leaf & ((1u << mid_bits) - 1)]->Find(static_cast<int64_t>(key));
  }

  // Mutable access; the leaf is cloned on its first write in this working set.
  SecEdgarQ3Entry *Find(uint64_t key) {
    if (!Peek(key)) {
      return nullptr;
    }
    return OwnLeaf(LeafOf(key))->Find(static_cast<int64_t>(key));
  }

  SecEdgarQ3Entry *FindOrCreate(uint64_t key) {
    if (auto *e = Find(key)) {
      return e;
    }
    MaybeGrow(bulk ? kLeafTarget : 4 * kLeafTarget);
    auto *leaf = OwnLeaf(LeafOf(key));
    ++entry_count;
    return leaf->Insert(static_cast<int64_t>(key), SecEdgarQ3Entry{}).first;
  }

  void Erase(uint64_t key) {
    if (Peek(key)) {
      OwnLeaf(LeafOf(key))->Erase(static_cast<int64_t>(key));
      --entry_count;
    }
  }

  // Bulk loading on a fresh index (grows the leaf count as entries arrive).
  void BulkAdd(uint64_t key, double sum, double comp, int64_t cnt) {
    MaybeGrow(kLeafTarget);
    SecEdgarQ3Entry entry;
    entry.sum = sum;
    entry.comp = comp;
    entry.cnt = cnt;
    auto *leaf = OwnLeaf(LeafOf(key));
    const auto inserted = leaf->Insert(static_cast<int64_t>(key), entry);
    if (inserted.second) {
      ++entry_count;
    } else {
      DdAdd(inserted.first->sum, inserted.first->comp, sum, comp);
      inserted.first->cnt += cnt;
    }
  }

  std::shared_ptr<const SecEdgarQ3AdshIndex> Finish() {
    auto index = std::make_shared<SecEdgarQ3AdshIndex>();
    index->top_bits = top_bits;
    index->mid_bits = mid_bits;
    index->tops = tops;
    index->overflow = overflow.Dict();
    index->entry_count = entry_count;
    return index;
  }

private:
  // Doubles the leaf count once the average leaf exceeds `target` entries.
  void MaybeGrow(std::size_t target) {
    if (entry_count >= (static_cast<uint64_t>(target) << top_bits << mid_bits) &&
        top_bits + mid_bits < kMaxIndexBits) {
      Repartition(top_bits + mid_bits + 1);
    }
  }

  void Init(uint32_t bits) {
    mid_bits = bits / 2;
    top_bits = bits - mid_bits;
    const std::size_t mids = std::size_t{1} << top_bits;
    const std::size_t leaves = std::size_t{1} << mid_bits;
    tops.assign(mids, nullptr);
    owned_tops.assign(mids, nullptr);
    owned_leaves.clear();
    for (std::size_t t = 0; t < mids; ++t) {
      auto mid = std::make_shared<SecEdgarQ3IndexMid>();
      mid->leaves.resize(leaves);
      for (auto &leaf : mid->leaves) {
        leaf = std::make_shared<SecEdgarQ3Leaf>();
      }
      tops[t] = mid;
      owned_tops[t] = mid;
      owned_leaves[t].assign(leaves, true);
    }
    entry_count = 0;
  }

  void Repartition(uint32_t bits) {
    std::vector<std::pair<int64_t, SecEdgarQ3Entry>> all;
    all.reserve(entry_count);
    for (const auto &mid : tops) {
      for (const auto &leaf : mid->leaves) {
        for (auto e : static_cast<const SecEdgarQ3Leaf &>(*leaf)) {
          all.emplace_back(e.first, e.second);
        }
      }
    }
    Init(bits);
    for (auto &[key, entry] : all) {
      OwnLeaf(LeafOf(static_cast<uint64_t>(key)))->Insert(key, entry);
    }
    entry_count = all.size();
  }

  std::size_t LeafOf(uint64_t key) const {
    return static_cast<std::size_t>(Mix64(key + 0x2545F4914F6CDD1DULL) >>
                                    (64 - (top_bits + mid_bits)));
  }

  SecEdgarQ3Leaf *OwnLeaf(std::size_t leaf) {
    const std::size_t t = leaf >> mid_bits;
    const std::size_t m = leaf & ((std::size_t{1} << mid_bits) - 1);
    if (!owned_tops[t]) {
      owned_tops[t] = std::make_shared<SecEdgarQ3IndexMid>(*tops[t]);
      tops[t] = owned_tops[t];
      owned_leaves[t].assign(owned_tops[t]->leaves.size(), false);
    }
    auto &flags = owned_leaves[t];
    if (flags.empty()) {
      flags.assign(owned_tops[t]->leaves.size(), false);
    }
    auto &slot = owned_tops[t]->leaves[m];
    if (!flags[m]) {
      slot = std::make_shared<SecEdgarQ3Leaf>(*slot);
      flags[m] = true;
    }
    return const_cast<SecEdgarQ3Leaf *>(slot.get());
  }

  uint32_t top_bits = 0;
  uint32_t mid_bits = 0;
  std::vector<std::shared_ptr<const SecEdgarQ3IndexMid>> tops;
  std::vector<std::shared_ptr<SecEdgarQ3IndexMid>> owned_tops;
  std::unordered_map<std::size_t, std::vector<bool>> owned_leaves;
  bool bulk = false;
  OverflowWork overflow;
  uint64_t entry_count = 0;
};

// Read-only lookups on a published index.
const SecEdgarQ3Entry *IndexFind(const SecEdgarQ3AdshIndex &index, uint64_t key) {
  const std::size_t bits = index.top_bits + index.mid_bits;
  const auto leaf = static_cast<std::size_t>(Mix64(key + 0x2545F4914F6CDD1DULL) >> (64 - bits));
  const auto &mid = *index.tops[leaf >> index.mid_bits];
  return mid.leaves[leaf & ((std::size_t{1} << index.mid_bits) - 1)]->Find(
      static_cast<int64_t>(key));
}

bool OverflowFind(const SecEdgarQ3OverflowDict &dict, const char *data,
                  std::size_t size, uint32_t &id) {
  OverflowWork work(&dict);
  return work.Find(data, size, id);
}

// Key of `adsh` in `index` (false when an overflow string is unknown there).
bool ResolveConst(const SecEdgarQ3AdshIndex &index, const std::string &adsh,
                  uint64_t &key) {
  if (SecEdgarQ3PackAdsh(adsh.data(), adsh.size(), key)) {
    return true;
  }
  uint32_t id;
  if (!OverflowFind(index.overflow, adsh.data(), adsh.size(), id)) {
    return false;
  }
  key = kSecEdgarQ3OverflowBit | id;
  return true;
}

// Key of an entry key of `from` in `to`.
bool TranslateKey(const SecEdgarQ3AdshIndex &from, uint64_t key,
                  const SecEdgarQ3AdshIndex &to, uint64_t &out) {
  if (!(key & kSecEdgarQ3OverflowBit)) {
    out = key;
    return true;
  }
  std::string_view s;
  from.overflow.strings.Get(static_cast<uint32_t>(key & ~kSecEdgarQ3OverflowBit), s);
  return ResolveConst(to, std::string(s), out);
}

template <typename Fn>
void ForEachEntry(const SecEdgarQ3AdshIndex &index, Fn &&fn) {
  for (const auto &mid : index.tops) {
    for (const auto &leaf : mid->leaves) {
      for (auto e : static_cast<const SecEdgarQ3Leaf &>(*leaf)) {
        fn(static_cast<uint64_t>(e.first), e.second);
      }
    }
  }
}

class AggWork {
public:
  explicit AggWork(const SecEdgarQ3AggArray *base) {
    if (base) {
      shared = base->chunks;
      size = base->size;
    }
    owned.resize(shared.size());
  }

  void Grow(uint32_t new_size) {
    if (new_size <= size) {
      return;
    }
    const std::size_t chunks_needed =
        (new_size + kSecEdgarQ3AggChunk - 1) / kSecEdgarQ3AggChunk;
    while (shared.size() < chunks_needed) {
      auto chunk = std::make_shared<SecEdgarQ3AggChunkData>();
      std::memset(chunk.get(), 0, sizeof(*chunk));
      shared.push_back(chunk);
      owned.push_back(std::move(chunk));
    }
    size = new_size;
  }

  // Adds the double-double (hi + lo) to the sum and dcnt to the row count.
  void Add(uint32_t idx, TwoD d, int64_t dcnt) {
    Grow(idx + 1);
    const auto c = idx / kSecEdgarQ3AggChunk;
    const auto i = idx % kSecEdgarQ3AggChunk;
    if (!owned[c]) {
      owned[c] = std::make_shared<SecEdgarQ3AggChunkData>(*shared[c]);
      shared[c] = owned[c];
    }
    auto &chunk = *owned[c];
    chunk.cnt[i] += dcnt;
    DdAdd(chunk.sum[i], chunk.comp[i], d.hi, d.lo);
    if (chunk.cnt[i] == 0) {
      chunk.sum[i] = 0.0;
      chunk.comp[i] = 0.0;
    } else if (chunk.cnt[i] < 0) {
      corrupt = true;
    }
  }

  SecEdgarQ3AggArray Finish() const {
    SecEdgarQ3AggArray array;
    array.chunks = shared;
    array.size = size;
    return array;
  }

  bool corrupt = false;
  uint32_t size = 0;

private:
  std::vector<std::shared_ptr<const SecEdgarQ3AggChunkData>> shared;
  std::vector<std::shared_ptr<SecEdgarQ3AggChunkData>> owned;
};

constexpr int64_t kNullCikKey = std::numeric_limits<int64_t>::min();

uint64_t GroupHash(const std::string *name, bool cik_null, int32_t cik) {
  uint64_t h = Mix64((name ? 1u : 0u) | (cik_null ? 2u : 0u) |
                     (static_cast<uint64_t>(static_cast<uint32_t>(cik_null ? 0 : cik)) << 8));
  if (name) {
    h = HashBytes(name->data(), name->size(), h);
  }
  return h;
}

class DirWork {
public:
  explicit DirWork(std::shared_ptr<const SecEdgarQ3GroupDir> base)
      : base(std::move(base)) {}

  const SecEdgarQ3GroupDir &View() const {
    if (cloned) {
      return *cloned;
    }
    if (base) {
      return *base;
    }
    static const SecEdgarQ3GroupDir empty;
    return empty;
  }

  uint32_t CikIdOfGroup(uint32_t group) const { return View().meta.Get(group).cik_id; }

  bool Lookup(const std::string *name, bool cik_null, int32_t cik, uint32_t &group) const {
    const auto &dir = View();
    const uint32_t *head =
        dir.by_hash.Find(static_cast<int64_t>(GroupHash(name, cik_null, cik)));
    const uint8_t flags = static_cast<uint8_t>((name == nullptr ? 1 : 0) | (cik_null ? 2 : 0));
    for (uint32_t at = head ? *head : 0; at != 0;) {
      const auto meta = dir.meta.Get(at - 1);
      if (meta.flags == flags && (cik_null || meta.cik == cik)) {
        bool same = true;
        if (name) {
          const auto stored = dir.Name(at - 1);
          same = stored.size() == name->size() &&
                 std::memcmp(stored.data(), name->data(), name->size()) == 0;
        }
        if (same) {
          group = at - 1;
          return true;
        }
      }
      at = meta.next;
    }
    return false;
  }

  // Finds or creates the (name, cik) group; grows the aggregate arrays.
  uint32_t FindOrAddGroup(const std::string *name, bool cik_null, int32_t cik) {
    uint32_t group;
    if (Lookup(name, cik_null, cik, group)) {
      return group;
    }
    if (!cloned) {
      cloned = base ? std::make_shared<SecEdgarQ3GroupDir>(*base)
                    : std::make_shared<SecEdgarQ3GroupDir>();
    }
    auto &dir = *cloned;
    if (dir.group_count == std::numeric_limits<uint32_t>::max() - 1) {
      throw std::overflow_error("SEC EDGAR Q3: group id space exhausted");
    }
    const int64_t cik_key = cik_null ? kNullCikKey : static_cast<int64_t>(cik);
    uint32_t cik_id;
    if (const uint32_t *found = dir.cik_ids.Find(cik_key)) {
      cik_id = *found - 1;
    } else {
      cik_id = dir.cik_count++;
      dir.cik_meta.Set(cik_id, {cik_null ? 0 : cik, static_cast<uint8_t>(cik_null ? 1 : 0)});
      dir.cik_ids.Upsert(cik_key, cik_id + 1);
    }
    group = dir.group_count++;
    if (name) {
      dir.names.Set(group, std::string_view(*name));
    }
    auto &head = dir.by_hash[static_cast<int64_t>(GroupHash(name, cik_null, cik))];
    SecEdgarQ3GroupMeta meta;
    meta.cik = cik_null ? 0 : cik;
    meta.cik_id = cik_id;
    meta.next = head;
    meta.flags = static_cast<uint8_t>((name == nullptr ? 1 : 0) | (cik_null ? 2 : 0));
    dir.meta.Set(group, meta);
    head = group + 1;
    return group;
  }

  std::shared_ptr<const SecEdgarQ3GroupDir> Finish() {
    if (cloned) {
      return cloned;
    }
    if (base) {
      return base;
    }
    return std::make_shared<SecEdgarQ3GroupDir>();
  }

private:
  std::shared_ptr<const SecEdgarQ3GroupDir> base;
  std::shared_ptr<SecEdgarQ3GroupDir> cloned;
};

// The adsh of an op / scan row: a packed key, or (overflow) the string.
struct KeyRef {
  uint64_t key = 0;
  const std::string *str = nullptr; // non-null iff the adsh does not pack
};

KeyRef MakeRef(const std::string &adsh) {
  KeyRef ref;
  if (!SecEdgarQ3PackAdsh(adsh.data(), adsh.size(), ref.key)) {
    ref.str = &adsh;
  }
  return ref;
}

bool ResolveIn(IndexWork &index, const KeyRef &ref, bool create, uint64_t &key) {
  if (!ref.str) {
    key = ref.key;
    return true;
  }
  return index.ResolveKey(*ref.str, create, key);
}

// Working copy of one cohort's aggregates.
struct CohortWork {
  explicit CohortWork(const SecEdgarQ3Totals *base)
      : groups(base ? &base->groups : nullptr), ciks(base ? &base->ciks : nullptr) {}
  AggWork groups;
  AggWork ciks;
};

class Work {
public:
  // `bulk`: a from-scratch build (indexes are sized for bulk loading).
  explicit Work(const SecEdgarQ3State *base, bool bulk = false)
      : subs(base ? base->subs.get() : nullptr, bulk),
        dir(base ? base->dir : std::shared_ptr<const SecEdgarQ3GroupDir>()),
        bulk(bulk) {
    if (base) {
      dict = base->uom_dict;
      base_uoms = base->uom_index;
      if (base->cohorts) {
        std::lock_guard<std::mutex> lock(base->cohorts->mutex);
        base_cohorts = base->cohorts->items;
      }
    }
    uoms.resize(base_uoms.size());
    for (const auto &item : base_cohorts) {
      base_cohort_map.emplace(item.first, item.second);
    }
  }

  IndexWork subs;
  DirWork dir;

  bool UomId(const std::string &name, bool create, uint32_t &id) {
    if (dict) {
      const auto it = dict->ids.find(name);
      if (it != dict->ids.end()) {
        id = it->second;
        return true;
      }
    }
    if (!create) {
      return false;
    }
    if (!own_dict) {
      own_dict = dict ? std::make_shared<SecEdgarQ3UomDict>(*dict)
                      : std::make_shared<SecEdgarQ3UomDict>();
      dict = own_dict;
    }
    id = static_cast<uint32_t>(own_dict->names.size());
    own_dict->names.push_back(name);
    own_dict->ids.emplace(name, id);
    base_uoms.push_back(nullptr);
    uoms.emplace_back();
    Uom(id);
    return true;
  }

  IndexWork &Uom(uint32_t id) {
    if (!uoms[id]) {
      uoms[id] = std::make_unique<IndexWork>(base_uoms[id].get(), bulk);
    }
    return *uoms[id];
  }

  void ForEachCohortOfFy(int32_t fy, const std::function<bool(uint64_t)> &fn) {
    for (const auto &item : base_cohorts) {
      if (static_cast<int32_t>(item.first >> 32) == fy && !fn(item.first)) {
        return;
      }
    }
  }

  CohortWork *Cohort(uint64_t ckey) {
    const auto live_it = live.find(ckey);
    if (live_it != live.end()) {
      return live_it->second.get();
    }
    const auto base_it = base_cohort_map.find(ckey);
    if (base_it == base_cohort_map.end()) {
      return nullptr;
    }
    return live.emplace(ckey, std::make_unique<CohortWork>(base_it->second.get()))
        .first->second.get();
  }

  void Propagate(CohortWork &cohort, uint32_t group, TwoD d, int64_t dcnt) {
    cohort.groups.Add(group, d, dcnt);
    cohort.ciks.Add(dir.CikIdOfGroup(group), d, dcnt);
  }

  // d / dcnt: change of the (filing, uom) aggregate.
  bool ApplyNum(const KeyRef &ref, const std::string &uom, TwoD d, int64_t dcnt) {
    uint32_t uid;
    if (!UomId(uom, dcnt > 0, uid)) {
      return false; // delete / value update of an untracked unit
    }
    IndexWork &ui = Uom(uid);
    uint64_t ukey;
    if (!ResolveIn(ui, ref, dcnt > 0, ukey)) {
      return false;
    }
    auto *entry = ui.Find(ukey);
    if (!entry) {
      if (dcnt <= 0) {
        return false; // delete / value update of an untracked filing
      }
      entry = ui.FindOrCreate(ukey);
    } else if (dcnt == 0 && entry->cnt == 0) {
      return false;
    }
    const double old_sum = entry->sum;
    const double old_comp = entry->comp;
    entry->cnt += dcnt;
    if (entry->cnt < 0) {
      return false;
    }
    DdAdd(entry->sum, entry->comp, d.hi, d.lo);
    if (entry->cnt == 0) {
      entry->sum = 0.0;
      entry->comp = 0.0;
    }
    const double new_sum = entry->sum;
    const double new_comp = entry->comp;
    const bool drop = entry->cnt == 0;
    uint64_t skey;
    if (ResolveIn(subs, ref, false, skey)) {
      const auto *sub = subs.Peek(skey);
      if (sub && sub->group >= 0) {
        if (CohortWork *cohort = Cohort(SecEdgarQ3CohortKey(sub->fy, uid))) {
          // NaN / inf contributions cannot be subtracted back out of the
          // group and cik sums: let a rebuild recompute them.
          if (!std::isfinite(old_sum) || !std::isfinite(new_sum)) {
            return false;
          }
          // effective change = new - old as a double-double
          const TwoD diff = TwoSum(new_sum, -old_sum);
          Propagate(*cohort, static_cast<uint32_t>(sub->group),
                    {diff.hi, diff.lo + (new_comp - old_comp)}, dcnt);
        }
      }
    }
    if (drop) {
      ui.Erase(ukey);
    }
    return Healthy();
  }

  bool AddSub(const KeyRef &ref, const std::string *name, bool cik_null,
              int32_t cik, int32_t fy) {
    uint64_t skey;
    ResolveIn(subs, ref, true, skey);
    auto *entry = subs.FindOrCreate(skey);
    if (entry->group >= 0) {
      return false; // duplicate sub.adsh
    }
    const uint32_t group = dir.FindOrAddGroup(name, cik_null, cik);
    entry->group = static_cast<int32_t>(group);
    entry->fy = fy;
    bool ok = true;
    ForEachCohortOfFy(fy, [&](uint64_t ckey) {
      IndexWork &ui = Uom(static_cast<uint32_t>(ckey));
      uint64_t ukey;
      if (!ResolveIn(ui, ref, false, ukey)) {
        return true;
      }
      const auto *ue = ui.Peek(ukey);
      if (!ue || ue->cnt == 0) {
        return true;
      }
      Propagate(*Cohort(ckey), group, {ue->sum, ue->comp}, ue->cnt);
      return true;
    });
    return ok && Healthy();
  }

  bool RemoveSub(const KeyRef &ref) {
    uint64_t skey;
    if (!ResolveIn(subs, ref, false, skey)) {
      return false;
    }
    const auto *entry = subs.Peek(skey);
    if (!entry || entry->group < 0) {
      return false;
    }
    const auto group = static_cast<uint32_t>(entry->group);
    const int32_t fy = entry->fy;
    bool ok = true;
    ForEachCohortOfFy(fy, [&](uint64_t ckey) {
      IndexWork &ui = Uom(static_cast<uint32_t>(ckey));
      uint64_t ukey;
      if (!ResolveIn(ui, ref, false, ukey)) {
        return true;
      }
      const auto *ue = ui.Peek(ukey);
      if (!ue || ue->cnt == 0) {
        return true;
      }
      if (!std::isfinite(ue->sum)) {
        ok = false; // cannot subtract NaN / inf from the group sums
        return false;
      }
      Propagate(*Cohort(ckey), group, {-ue->sum, -ue->comp}, -ue->cnt);
      return true;
    });
    if (!ok) {
      return false;
    }
    subs.Erase(skey);
    return Healthy();
  }

  bool Healthy() const {
    for (const auto &[key, cohort] : live) {
      if (cohort->groups.corrupt || cohort->ciks.corrupt) {
        return false;
      }
    }
    return true;
  }

  std::shared_ptr<const SecEdgarQ3State> Finish() {
    auto state = std::make_shared<SecEdgarQ3State>();
    state->subs = subs.Finish();
    state->uom_dict = dict ? std::shared_ptr<const SecEdgarQ3UomDict>(dict)
                           : std::make_shared<SecEdgarQ3UomDict>();
    state->uom_index.resize(uoms.size());
    for (std::size_t i = 0; i < uoms.size(); ++i) {
      state->uom_index[i] = uoms[i] ? uoms[i]->Finish() : base_uoms[i];
    }
    state->dir = dir.Finish();
    state->cohorts = std::make_shared<SecEdgarQ3CohortCache>();
    for (const auto &item : base_cohorts) {
      const auto live_it = live.find(item.first);
      if (live_it == live.end()) {
        state->cohorts->items.push_back(item);
        continue;
      }
      auto totals = std::make_shared<SecEdgarQ3Totals>();
      totals->dir = state->dir;
      totals->groups = live_it->second->groups.Finish();
      totals->ciks = live_it->second->ciks.Finish();
      state->cohorts->items.emplace_back(item.first, std::move(totals));
    }
    return state;
  }

private:
  bool bulk = false;
  std::shared_ptr<const SecEdgarQ3UomDict> dict;
  std::shared_ptr<SecEdgarQ3UomDict> own_dict;
  std::vector<std::shared_ptr<const SecEdgarQ3AdshIndex>> base_uoms;
  std::vector<std::unique_ptr<IndexWork>> uoms;
  std::vector<std::pair<uint64_t, std::shared_ptr<const SecEdgarQ3Totals>>> base_cohorts;
  std::unordered_map<uint64_t, std::shared_ptr<const SecEdgarQ3Totals>> base_cohort_map;
  std::unordered_map<uint64_t, std::unique_ptr<CohortWork>> live;
};

// An entry of a unit's num index that belongs to the cohort.
struct CohortHit {
  uint32_t group;
  uint32_t cik_id;
  TwoD sum;
  int64_t cnt;
};

// Leaves per slice of a cohort build: ~128 entries each, two hash lookups
// apiece, so ~3 ms at 256.
constexpr std::size_t kCohortLeafGrain = 256;

// Builds the cohort of (fy, uom id) from the indexes. The sub lookups run in
// slices; the hits are added in index order, so the sums are the serial ones.
std::shared_ptr<const SecEdgarQ3Totals>
MaterializeCohort(const core::AuxEngineServices &services, const SecEdgarQ3State &state,
                  int32_t fy, uint32_t uid) {
  AggWork groups(nullptr);
  AggWork ciks(nullptr);
  const auto &dir = *state.dir;
  groups.Grow(static_cast<uint32_t>(dir.GroupCount()));
  ciks.Grow(static_cast<uint32_t>(dir.CikCount()));
  if (uid < state.uom_index.size() && state.uom_index[uid]) {
    const SecEdgarQ3AdshIndex &ui = *state.uom_index[uid];
    std::vector<const SecEdgarQ3Leaf *> leaves;
    for (const auto &mid : ui.tops) {
      for (const auto &leaf : mid->leaves) {
        leaves.push_back(leaf.get());
      }
    }
    std::vector<CohortHit> hits;
    core::ParallelReduce(
        services, leaves.size(), kCohortLeafGrain, hits,
        [&](std::vector<CohortHit> &out, std::size_t begin, std::size_t end) {
          for (std::size_t l = begin; l < end; ++l) {
            for (auto e : *leaves[l]) {
              const auto key = static_cast<uint64_t>(e.first);
              if (e.second.cnt <= 0) {
                continue;
              }
              uint64_t skey;
              if (!TranslateKey(ui, key, *state.subs, skey)) {
                continue;
              }
              const auto *sub = IndexFind(*state.subs, skey);
              if (!sub || sub->group < 0 || sub->fy != fy) {
                continue;
              }
              const auto group = static_cast<uint32_t>(sub->group);
              out.push_back({group, dir.meta.Get(group).cik_id,
                             {e.second.sum, e.second.comp}, e.second.cnt});
            }
          }
        },
        [](std::vector<CohortHit> &into, std::vector<CohortHit> &&from) {
          into.insert(into.end(), from.begin(), from.end());
        });
    for (const auto &hit : hits) {
      groups.Add(hit.group, hit.sum, hit.cnt);
      ciks.Add(hit.cik_id, hit.sum, hit.cnt);
    }
  }
  auto totals = std::make_shared<SecEdgarQ3Totals>();
  totals->dir = state.dir;
  totals->groups = groups.Finish();
  totals->ciks = ciks.Finish();
  return totals;
}

// ---------------------------------------------------------------------------
// Images / plans
// ---------------------------------------------------------------------------

std::shared_ptr<const SecEdgarQ3StateImage>
MakeStateImage(const std::shared_ptr<const SecEdgarQ3State> &state) {
  auto image = std::make_shared<SecEdgarQ3StateImage>();
  image->kind = SecEdgarQ3ImageKind::CikTotals;
  image->state = state;
  return image;
}

const SecEdgarQ3StateImage *AsQ3Image(const core::StateImage &image) {
  const auto *payload = image.As<SecEdgarQ3StateImage>();
  return payload && payload->kind == SecEdgarQ3ImageKind::CikTotals &&
                 payload->state && payload->state->subs &&
                 payload->state->dir && payload->state->uom_dict &&
                 payload->state->cohorts
             ? payload
             : nullptr;
}

core::InvalidationPlan AllPartitions() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::StagedMaintenanceOutcome RebuildOutcome(core::MaintenanceAction action) {
  core::StagedMaintenanceOutcome outcome;
  outcome.action = action;
  outcome.invalidations = AllPartitions();
  outcome.pending_rebuild = outcome.invalidations;
  return outcome;
}

bool UpdateTouches(const core::MutationBatch &mutation,
                   const std::vector<core::ColumnId> &columns) {
  if (mutation.kind != core::MutationKind::Update ||
      mutation.updated_columns.empty()) {
    return true;
  }
  for (const auto updated : mutation.updated_columns) {
    for (const auto column : columns) {
      if (updated == column) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

bool SecEdgarQ3PackAdsh(const char *data, std::size_t size, uint64_t &key) {
  if (size != 20 || data[10] != '-' || data[13] != '-') {
    return false;
  }
  uint64_t value = 0;
  for (std::size_t i = 0; i < 20; ++i) {
    if (i == 10 || i == 13) {
      continue;
    }
    const unsigned digit = static_cast<unsigned char>(data[i]) - '0';
    if (digit > 9) {
      return false;
    }
    value = value * 10 + digit;
  }
  key = value;
  return true;
}

// ---------------------------------------------------------------------------
// SIMD kernels
// ---------------------------------------------------------------------------

bool SecEdgarQ3Kernels::HasAvx2() {
#ifdef ACCEL_Q3_X86
  static const bool has = [] {
    if (const char *force = std::getenv("ACCEL_Q3_FORCE_SCALAR");
        force && *force && *force != '0') {
      return false;
    }
    return __builtin_cpu_supports("avx2") != 0;
  }();
  return has;
#else
  return false;
#endif
}

void SecEdgarQ3Kernels::AvgInputsScalar(const SecEdgarQ3AggArray &array,
                                        double &sum, uint64_t &count) {
  double total = 0.0;
  uint64_t n = 0;
  for (uint32_t idx = 0; idx < array.size; ++idx) {
    const auto &chunk = *array.chunks[idx / kSecEdgarQ3AggChunk];
    const auto i = idx % kSecEdgarQ3AggChunk;
    if (chunk.cnt[i] > 0) {
      total += chunk.sum[i];
      ++n;
    }
  }
  sum = total;
  count = n;
}

#ifdef ACCEL_Q3_X86
__attribute__((target("avx2"))) void
SecEdgarQ3Kernels::AvgInputsAvx2(const SecEdgarQ3AggArray &array, double &sum,
                                 uint64_t &count) {
  // Chunks are zero padded past `size`, so whole chunks can be processed.
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  uint64_t n = 0;
  const __m256i zero = _mm256_setzero_si256();
  for (const auto &chunk_ptr : array.chunks) {
    const auto &chunk = *chunk_ptr;
    for (std::size_t i = 0; i < kSecEdgarQ3AggChunk; i += 8) {
      const __m256i c0 =
          _mm256_load_si256(reinterpret_cast<const __m256i *>(chunk.cnt + i));
      const __m256i c1 =
          _mm256_load_si256(reinterpret_cast<const __m256i *>(chunk.cnt + i + 4));
      const __m256d m0 = _mm256_castsi256_pd(_mm256_cmpgt_epi64(c0, zero));
      const __m256d m1 = _mm256_castsi256_pd(_mm256_cmpgt_epi64(c1, zero));
      n += static_cast<unsigned>(__builtin_popcount(
          _mm256_movemask_pd(m0) | (_mm256_movemask_pd(m1) << 4)));
      acc0 = _mm256_add_pd(acc0, _mm256_and_pd(_mm256_load_pd(chunk.sum + i), m0));
      acc1 = _mm256_add_pd(acc1, _mm256_and_pd(_mm256_load_pd(chunk.sum + i + 4), m1));
    }
  }
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, _mm256_add_pd(acc0, acc1));
  sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
  count = n;
}
#else
void SecEdgarQ3Kernels::AvgInputsAvx2(const SecEdgarQ3AggArray &array,
                                      double &sum, uint64_t &count) {
  AvgInputsScalar(array, sum, count);
}
#endif

void SecEdgarQ3Kernels::AvgInputs(const SecEdgarQ3AggArray &array, double &sum,
                                  uint64_t &count) {
  if (HasAvx2()) {
    AvgInputsAvx2(array, sum, count);
  } else {
    AvgInputsScalar(array, sum, count);
  }
}

void SecEdgarQ3Kernels::CollectScalar(const SecEdgarQ3AggArray &array,
                                      double threshold,
                                      std::vector<SecEdgarQ3Result::Row> &out) {
  for (uint32_t idx = 0; idx < array.size; ++idx) {
    const auto &chunk = *array.chunks[idx / kSecEdgarQ3AggChunk];
    const auto i = idx % kSecEdgarQ3AggChunk;
    if (chunk.cnt[i] > 0 && chunk.sum[i] > threshold) {
      out.push_back({idx, chunk.sum[i]});
    }
  }
}

#ifdef ACCEL_Q3_X86
__attribute__((target("avx2"))) void
SecEdgarQ3Kernels::CollectAvx2(const SecEdgarQ3AggArray &array,
                               double threshold,
                               std::vector<SecEdgarQ3Result::Row> &out) {
  const __m256d thr = _mm256_set1_pd(threshold);
  const __m256i zero = _mm256_setzero_si256();
  uint32_t base = 0;
  for (const auto &chunk_ptr : array.chunks) {
    const auto &chunk = *chunk_ptr;
    for (std::size_t i = 0; i < kSecEdgarQ3AggChunk; i += 4) {
      const __m256d s = _mm256_load_pd(chunk.sum + i);
      const __m256i c =
          _mm256_load_si256(reinterpret_cast<const __m256i *>(chunk.cnt + i));
      const __m256d gt = _mm256_cmp_pd(s, thr, _CMP_GT_OQ);
      const __m256d live = _mm256_castsi256_pd(_mm256_cmpgt_epi64(c, zero));
      unsigned bits = static_cast<unsigned>(
          _mm256_movemask_pd(_mm256_and_pd(gt, live)));
      while (bits) {
        const unsigned lane = static_cast<unsigned>(__builtin_ctz(bits));
        bits &= bits - 1;
        out.push_back({base + static_cast<uint32_t>(i) + lane,
                       chunk.sum[i + lane]});
      }
    }
    base += static_cast<uint32_t>(kSecEdgarQ3AggChunk);
  }
}
#else
void SecEdgarQ3Kernels::CollectAvx2(const SecEdgarQ3AggArray &array,
                                    double threshold,
                                    std::vector<SecEdgarQ3Result::Row> &out) {
  CollectScalar(array, threshold, out);
}
#endif

void SecEdgarQ3Kernels::Collect(const SecEdgarQ3AggArray &array,
                                double threshold,
                                std::vector<SecEdgarQ3Result::Row> &out) {
  if (HasAvx2()) {
    CollectAvx2(array, threshold, out);
  } else {
    CollectScalar(array, threshold, out);
  }
}

// ---------------------------------------------------------------------------
// Result computation
// ---------------------------------------------------------------------------

namespace {

// ORDER BY total DESC, then a deterministic order of the groups (NULL name
// first, bytewise name, cik).
bool TieLess(const SecEdgarQ3GroupDir &dir, uint32_t a, uint32_t b) {
  const bool a_null = dir.NameIsNull(a);
  const bool b_null = dir.NameIsNull(b);
  if (a_null != b_null) {
    return a_null;
  }
  if (!a_null) {
    const auto na = dir.Name(a);
    const auto nb = dir.Name(b);
    const int cmp = std::memcmp(na.data(), nb.data(), std::min(na.size(), nb.size()));
    if (cmp != 0) {
      return cmp < 0;
    }
    if (na.size() != nb.size()) {
      return na.size() < nb.size();
    }
  }
  return dir.Cik(a) < dir.Cik(b);
}

std::shared_ptr<const SecEdgarQ3Result>
ComputeResult(const SecEdgarQ3Totals &totals) {
  auto result = std::make_shared<SecEdgarQ3Result>();
  double sum = 0.0;
  uint64_t n = 0;
  SecEdgarQ3Kernels::AvgInputs(totals.ciks, sum, n);
  if (n == 0) {
    return result; // AVG over no rows is NULL: nothing passes HAVING
  }
  const double threshold = sum / static_cast<double>(n);
  auto &candidates = result->rows;
  candidates.reserve(totals.groups.size / 2 + 8);
  SecEdgarQ3Kernels::Collect(totals.groups, threshold, candidates);
  const auto &dir = *totals.dir;
  const auto better = [&dir](const SecEdgarQ3Result::Row &a,
                             const SecEdgarQ3Result::Row &b) {
    if (a.total != b.total) {
      return a.total > b.total;
    }
    return TieLess(dir, a.group, b.group);
  };
  if (candidates.size() > kOutputLimit) {
    std::nth_element(candidates.begin(), candidates.begin() + kOutputLimit,
                     candidates.end(), better);
    candidates.resize(kOutputLimit);
  }
  std::sort(candidates.begin(), candidates.end(), better);
  candidates.shrink_to_fit();
  return result;
}

} // namespace

std::shared_ptr<const SecEdgarQ3Result> SecEdgarQ3Totals::Result() const {
  std::lock_guard<std::mutex> lock(result_mutex);
  if (!result) {
    result = ComputeResult(*this);
  }
  return result;
}

std::shared_ptr<const std::vector<core::AuxRow>>
SecEdgarQ3Totals::OutputRows() const {
  std::lock_guard<std::mutex> lock(result_mutex);
  if (output_rows) {
    return output_rows;
  }
  if (!result) {
    result = ComputeResult(*this);
  }
  const auto &dir = *this->dir;
  auto rows = std::make_shared<std::vector<core::AuxRow>>();
  rows->reserve(result->rows.size());
  for (const auto &row : result->rows) {
    const auto g = row.group;
    core::AuxRow out;
    out.values.reserve(3);
    if (dir.NameIsNull(g)) {
      out.values.push_back(core::AuxValue::Null(core::AuxType::String));
    } else {
      out.values.push_back(
          core::AuxValue::Of(core::AuxType::String, std::string(dir.Name(g))));
    }
    if (dir.CikIsNull(g)) {
      out.values.push_back(core::AuxValue::Null(core::AuxType::Int64));
    } else {
      out.values.push_back(core::AuxValue::Of(core::AuxType::Int64,
                                              static_cast<int64_t>(dir.Cik(g))));
    }
    out.values.push_back(core::AuxValue::Of(core::AuxType::Double, row.total));
    rows->push_back(std::move(out));
  }
  output_rows = std::move(rows);
  return output_rows;
}

std::shared_ptr<const SecEdgarQ3Totals>
SecEdgarQ3CohortFor(const core::AuxEngineServices &services, const SecEdgarQ3State &state,
                    int64_t fy, const std::string &uom) {
  if (fy < std::numeric_limits<int32_t>::min() ||
      fy > std::numeric_limits<int32_t>::max() || !state.uom_dict ||
      !state.cohorts) {
    return nullptr; // no INTEGER fiscal year can equal it
  }
  const auto it = state.uom_dict->ids.find(uom);
  if (it == state.uom_dict->ids.end()) {
    return nullptr; // no num row has this unit
  }
  const uint64_t key = SecEdgarQ3CohortKey(static_cast<int32_t>(fy), it->second);
  auto &cache = *state.cohorts;
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    for (std::size_t i = 0; i < cache.items.size(); ++i) {
      if (cache.items[i].first == key) {
        auto hit = cache.items[i];
        cache.items.erase(cache.items.begin() + static_cast<std::ptrdiff_t>(i));
        cache.items.push_back(hit);
        return hit.second;
      }
    }
  }
  auto built = MaterializeCohort(services, state, static_cast<int32_t>(fy), it->second);
  std::lock_guard<std::mutex> lock(cache.mutex);
  for (const auto &item : cache.items) {
    if (item.first == key) {
      return item.second; // another reader was faster
    }
  }
  cache.items.emplace_back(key, built);
  if (cache.items.size() > kSecEdgarQ3MaxCohorts) {
    cache.items.erase(cache.items.begin());
  }
  return built;
}

std::vector<SecEdgarQ3OutputRow>
BuildSecEdgarQ3Output(const core::AuxEngineServices &services, const SecEdgarQ3State &state,
                      int64_t fy, const std::string &uom) {
  std::vector<SecEdgarQ3OutputRow> rows;
  const auto totals = SecEdgarQ3CohortFor(services, state, fy, uom);
  if (!totals) {
    return rows;
  }
  const auto result = totals->Result();
  const auto &dir = *totals->dir;
  rows.reserve(result->rows.size());
  for (const auto &row : result->rows) {
    SecEdgarQ3OutputRow out;
    out.name_is_null = dir.NameIsNull(row.group);
    if (!out.name_is_null) {
      out.name = std::string(dir.Name(row.group));
    }
    out.cik_is_null = dir.CikIsNull(row.group);
    out.cik = dir.Cik(row.group);
    out.total_value = row.total;
    rows.push_back(std::move(out));
  }
  return rows;
}

// ---------------------------------------------------------------------------
// Encoding (footprint accounting)
// ---------------------------------------------------------------------------

namespace {

void PutVarint(ByteWriter &w, uint64_t v) {
  while (v >= 0x80) {
    w.U8(static_cast<uint8_t>(v | 0x80));
    v >>= 7;
  }
  w.U8(static_cast<uint8_t>(v));
}

// Integral doubles (typical for USD facts) as zigzag varints, others raw.
void PutSum(ByteWriter &w, double v) {
  if (std::fabs(v) < 4503599627370496.0 && v == std::floor(v)) {
    const auto i = static_cast<int64_t>(v);
    const uint64_t zz = (static_cast<uint64_t>(i) << 1) ^ static_cast<uint64_t>(i >> 63);
    PutVarint(w, (zz << 1) | 1);
  } else {
    PutVarint(w, 0);
    w.Double(v);
  }
}

// Compact serialization: keys sorted + delta varint, sums/counts/groups varint.
// `sub`: sub index entries (group, fy); else num index entries (sum, cnt).
void EncodeIndex(const SecEdgarQ3AdshIndex &index, ByteWriter &writer, bool sub) {
  core::WriteImageHeader(writer, kIndexMagic, kFormatVersion,
                         static_cast<uint64_t>(index.entry_count));
  std::vector<std::pair<uint64_t, SecEdgarQ3Entry>> all;
  all.reserve(index.entry_count);
  ForEachEntry(index, [&](uint64_t key, const SecEdgarQ3Entry &e) {
    all.emplace_back(key, e);
  });
  std::sort(all.begin(), all.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  uint64_t prev = 0;
  for (const auto &[key, entry] : all) {
    PutVarint(writer, key - prev);
    prev = key;
    if (sub) {
      PutVarint(writer, static_cast<uint64_t>(entry.group + 1));
      const auto zz = (static_cast<uint64_t>(static_cast<int64_t>(entry.fy)) << 1) ^
                      static_cast<uint64_t>(static_cast<int64_t>(entry.fy) >> 63);
      PutVarint(writer, zz);
    } else {
      PutSum(writer, entry.sum);
      PutSum(writer, entry.comp);
      PutVarint(writer, static_cast<uint64_t>(entry.cnt));
    }
  }
  writer.U32(index.overflow.count);
  for (uint32_t id = 0; id < index.overflow.count; ++id) {
    std::string_view s;
    index.overflow.strings.Get(id, s);
    writer.String(std::string(s));
    writer.U32(id);
  }
}

void EncodeIndexes(const SecEdgarQ3State &state, ByteWriter &writer) {
  if (state.subs) {
    EncodeIndex(*state.subs, writer, /*sub=*/true);
  }
  if (state.uom_dict) {
    writer.U32(static_cast<uint32_t>(state.uom_dict->names.size()));
    for (const auto &name : state.uom_dict->names) {
      writer.String(name);
    }
  }
  for (const auto &index : state.uom_index) {
    if (index) {
      EncodeIndex(*index, writer, /*sub=*/false);
    }
  }
}

void EncodeAgg(const SecEdgarQ3AggArray &array, ByteWriter &writer) {
  for (uint32_t idx = 0; idx < array.size; ++idx) {
    const auto &chunk = *array.chunks[idx / kSecEdgarQ3AggChunk];
    const auto i = idx % kSecEdgarQ3AggChunk;
    PutSum(writer, chunk.sum[i]);
    PutSum(writer, chunk.comp[i]);
    PutVarint(writer, static_cast<uint64_t>(chunk.cnt[i]));
  }
}

void EncodeDir(const SecEdgarQ3GroupDir &dir, ByteWriter &writer) {
  core::WriteImageHeader(writer, kStateMagic, kFormatVersion,
                         static_cast<uint64_t>(dir.GroupCount()));
  writer.U32(static_cast<uint32_t>(dir.CikCount()));
  for (uint32_t g = 0; g < dir.GroupCount(); ++g) {
    const auto meta = dir.meta.Get(g);
    writer.U8(meta.flags);
    writer.I32(meta.cik);
    PutVarint(writer, meta.cik_id);
    writer.String(std::string(dir.Name(g)));
  }
  for (uint32_t c = 0; c < dir.CikCount(); ++c) {
    const auto meta = dir.cik_meta.Get(c);
    writer.U8(meta.is_null);
    writer.I32(meta.value);
  }
}

} // namespace

std::vector<uint8_t> EncodeSecEdgarQ3Fy2022Cohort(const SecEdgarQ3State &state) {
  ByteWriter writer;
  EncodeIndexes(state, writer);
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ3CikTotals(const SecEdgarQ3State &state) {
  ByteWriter writer;
  EncodeIndexes(state, writer);
  if (state.dir) {
    EncodeDir(*state.dir, writer);
  }
  if (state.cohorts) {
    std::lock_guard<std::mutex> lock(state.cohorts->mutex);
    for (const auto &[key, totals] : state.cohorts->items) {
      writer.U32(static_cast<uint32_t>(key >> 32));
      writer.U32(static_cast<uint32_t>(key));
      EncodeAgg(totals->groups, writer);
      EncodeAgg(totals->ciks, writer);
    }
  }
  return std::move(writer.bytes);
}

namespace {

uint64_t IndexResidentBytes(const SecEdgarQ3AdshIndex &index) {
  uint64_t bytes = sizeof(SecEdgarQ3AdshIndex);
  for (const auto &mid : index.tops) {
    bytes += sizeof(SecEdgarQ3IndexMid) + 32 +
             mid->leaves.capacity() * sizeof(mid->leaves[0]);
    for (const auto &leaf : mid->leaves) {
      bytes += sizeof(SecEdgarQ3Leaf) + 32 + leaf->memory_bytes();
    }
  }
  const auto &ov = index.overflow;
  return bytes + ov.strings.MemoryBytes() + ov.by_hash.memory_bytes() +
         ov.next.MemoryBytes();
}

} // namespace

uint64_t SecEdgarQ3ResidentBytes(const SecEdgarQ3State &state) {
  uint64_t bytes = 0;
  if (state.subs) {
    bytes += IndexResidentBytes(*state.subs);
  }
  for (const auto &index : state.uom_index) {
    if (index) {
      bytes += IndexResidentBytes(*index);
    }
  }
  if (state.dir) {
    const auto &dir = *state.dir;
    bytes += sizeof(dir) + dir.names.MemoryBytes() + dir.meta.MemoryBytes() +
             dir.cik_meta.MemoryBytes() + dir.by_hash.memory_bytes() +
             dir.cik_ids.memory_bytes();
  }
  if (state.cohorts) {
    std::lock_guard<std::mutex> lock(state.cohorts->mutex);
    for (const auto &item : state.cohorts->items) {
      bytes += (item.second->groups.chunks.size() + item.second->ciks.chunks.size()) *
               (sizeof(SecEdgarQ3AggChunkData) + 16);
    }
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Txn buffer
// ---------------------------------------------------------------------------

bool SecEdgarQ3TxnBuffer::HasPendingChanges() const { return pending; }

void SecEdgarQ3TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  ops.clear();
  strings.clear();
  uom_strings.clear();
}

bool SecEdgarQ3TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

void SecEdgarQ3TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  ops.clear();
  strings.clear();
  uom_strings.clear();
}

void SecEdgarQ3TxnBuffer::AddOp(Op op) {
  pending = true;
  if (!requires_rebuild) {
    ops.push_back(op);
  }
}

uint32_t SecEdgarQ3TxnBuffer::InternString(std::string value) {
  strings.push_back(std::move(value));
  return static_cast<uint32_t>(strings.size() - 1);
}

uint32_t SecEdgarQ3TxnBuffer::InternUom(const std::string &uom) {
  const auto it = uom_strings.find(uom);
  if (it != uom_strings.end()) {
    return it->second;
  }
  const uint32_t idx = InternString(uom);
  uom_strings.emplace(uom, idx);
  return idx;
}

void SecEdgarQ3TxnBuffer::Absorb(SecEdgarQ3TxnBuffer &&from) {
  if (requires_rebuild) {
    return;
  }
  if (from.requires_rebuild) {
    MarkRebuild();
    return;
  }
  const auto rekey = [&](uint64_t key) {
    return (key & kSecEdgarQ3OverflowBit)
               ? kSecEdgarQ3OverflowBit |
                     InternString(from.strings[static_cast<std::size_t>(key & ~kSecEdgarQ3OverflowBit)])
               : key;
  };
  for (auto op : from.ops) {
    op.key = rekey(op.key);
    if (op.kind == OpKind::Num) {
      op.uom_idx = InternUom(from.strings[op.uom_idx]);
    } else if (op.kind == OpKind::SubAdd && !(op.flags & 1)) {
      op.name_idx = InternString(from.strings[op.name_idx]);
    }
    AddOp(op);
  }
}

uint64_t SecEdgarQ3TxnBuffer::KeyFor(const std::string &adsh) {
  uint64_t key = 0;
  if (SecEdgarQ3PackAdsh(adsh.data(), adsh.size(), key)) {
    return key;
  }
  return kSecEdgarQ3OverflowBit | InternString(adsh);
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

std::vector<core::TableDependency> SecEdgarQ3Module::Dependencies() const {
  return {{config.num_table, config.num_pos, config.num_attnums},
          {config.sub_table, config.sub_pos, config.sub_attnums}};
}

std::unique_ptr<core::AuxTxnBuffer> SecEdgarQ3Module::CreateTxnBuffer() const {
  return std::make_unique<SecEdgarQ3TxnBuffer>();
}

void SecEdgarQ3Module::Build(core::AuxEngineServices &services,
                             core::StateBuilder &builder) {
  const auto state =
      std::make_shared<const SecEdgarQ3State>(BuildState(services));
  builder.Write(core::StateImage::Make(Id(), kStatePartition, 0,
                                       MakeStateImage(state)));
  builder.Finish();
}

core::InvalidationPlan
SecEdgarQ3Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.num_table) {
    if (!UpdateTouches(mutation, config.num_pos)) {
      return {};
    }
    return AllPartitions();
  }
  if (mutation.table == config.sub_table) {
    if (!UpdateTouches(mutation, config.sub_pos)) {
      return {};
    }
    return AllPartitions();
  }
  return {};
}

namespace {

using TxnOp = SecEdgarQ3TxnBuffer::Op;
using TxnOpKind = SecEdgarQ3TxnBuffer::OpKind;

// Rows per slice of a mutation's row loop (parsing and key packing).
constexpr std::size_t kAccumulateGrain = 4096;

// Buffers the ops of rows [begin, end) of a num mutation into `buffer`.
void AccumulateNumRows(const SecEdgarQ3Config &config, const core::MutationBatch &mutation,
                       std::size_t begin, std::size_t end, SecEdgarQ3TxnBuffer &buffer) {
  auto num_op = [&](const NumRow &row, int sign) {
    TxnOp op;
    op.kind = TxnOpKind::Num;
    op.sign = static_cast<int8_t>(sign);
    op.key = buffer.KeyFor(*row.adsh);
    op.uom_idx = buffer.InternUom(*row.uom);
    op.value = sign * row.value;
    buffer.AddOp(op);
  };
  auto mark_if_nonfinite = [&](const NumRow &row) {
    if (row.relevant && !std::isfinite(row.value)) {
      buffer.MarkRebuild(); // NaN / inf cannot be subtracted back out
      return true;
    }
    return false;
  };
  // Insert / delete: every relevant row of the image is one op.
  const auto image_rows = [&](const std::vector<core::AuxRow> &rows, int sign) {
    for (std::size_t i = begin; i < end; ++i) {
      NumRow row;
      if (!ReadNumRow(rows[i], config.num_pos, row)) {
        buffer.MarkRebuild();
        return;
      }
      if (!row.relevant) {
        continue;
      }
      if (mark_if_nonfinite(row)) {
        return;
      }
      num_op(row, sign);
    }
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    image_rows(mutation.new_rows, 1);
    break;
  case core::MutationKind::Delete:
    image_rows(mutation.old_rows, -1);
    break;
  case core::MutationKind::Update:
    for (std::size_t i = begin; i < end; ++i) {
      NumRow old_row;
      NumRow new_row;
      if (!ReadNumRow(mutation.old_rows[i], config.num_pos, old_row) ||
          !ReadNumRow(mutation.new_rows[i], config.num_pos, new_row)) {
        buffer.MarkRebuild();
        return;
      }
      if (mark_if_nonfinite(old_row) || mark_if_nonfinite(new_row)) {
        return;
      }
      if (old_row.relevant && new_row.relevant && *old_row.adsh == *new_row.adsh &&
          *old_row.uom == *new_row.uom) {
        // value-only change: one lookup, no row-count change; the exact
        // difference is carried as a double-double
        TxnOp op;
        op.kind = TxnOpKind::Num;
        op.sign = 0;
        op.key = buffer.KeyFor(*new_row.adsh);
        op.uom_idx = buffer.InternUom(*new_row.uom);
        const TwoD diff = TwoSum(new_row.value, -old_row.value);
        op.value = diff.hi;
        op.value_lo = diff.lo;
        buffer.AddOp(op);
        continue;
      }
      if (old_row.relevant) {
        num_op(old_row, -1);
      }
      if (new_row.relevant) {
        num_op(new_row, 1);
      }
    }
    break;
  }
}

// Buffers the ops of rows [begin, end) of a sub mutation into `buffer`.
void AccumulateSubRows(const SecEdgarQ3Config &config, const core::MutationBatch &mutation,
                       std::size_t begin, std::size_t end, SecEdgarQ3TxnBuffer &buffer) {
  auto remove_op = [&](const SubRow &row) {
    TxnOp op;
    op.kind = TxnOpKind::SubRemove;
    op.key = buffer.KeyFor(*row.adsh);
    buffer.AddOp(op);
  };
  auto add_op = [&](const SubRow &row) {
    TxnOp op;
    op.kind = TxnOpKind::SubAdd;
    op.key = buffer.KeyFor(*row.adsh);
    op.cik = row.cik;
    op.fy = row.fy;
    op.flags = static_cast<uint8_t>((row.name == nullptr ? 1 : 0) |
                                    (row.cik_null ? 2 : 0));
    if (row.name) {
      op.name_idx = buffer.InternString(*row.name);
    }
    buffer.AddOp(op);
  };
  const auto image_rows = [&](const std::vector<core::AuxRow> &rows, bool added) {
    for (std::size_t i = begin; i < end; ++i) {
      SubRow row;
      if (!ReadSubRow(rows[i], config.sub_pos, row)) {
        buffer.MarkRebuild();
        return;
      }
      if (row.tracked) {
        added ? add_op(row) : remove_op(row);
      }
    }
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    image_rows(mutation.new_rows, true);
    break;
  case core::MutationKind::Delete:
    image_rows(mutation.old_rows, false);
    break;
  case core::MutationKind::Update:
    for (std::size_t i = begin; i < end; ++i) {
      SubRow old_row;
      SubRow new_row;
      if (!ReadSubRow(mutation.old_rows[i], config.sub_pos, old_row) ||
          !ReadSubRow(mutation.new_rows[i], config.sub_pos, new_row)) {
        buffer.MarkRebuild();
        return;
      }
      if (!old_row.tracked && !new_row.tracked) {
        continue;
      }
      if (old_row.tracked && new_row.tracked && old_row.fy == new_row.fy &&
          *old_row.adsh == *new_row.adsh && old_row.cik_null == new_row.cik_null &&
          (old_row.cik_null || old_row.cik == new_row.cik) &&
          (old_row.name == nullptr) == (new_row.name == nullptr) &&
          (old_row.name == nullptr || *old_row.name == *new_row.name)) {
        continue; // nothing observable changed
      }
      if (old_row.tracked) {
        remove_op(old_row);
      }
      if (new_row.tracked) {
        add_op(new_row);
      }
    }
    break;
  }
}

} // namespace

void SecEdgarQ3Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &services,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ3TxnBuffer &>(base_buffer);
  const bool is_num = mutation.table == config.num_table;
  if (!is_num && mutation.table != config.sub_table) {
    return;
  }
  if (!UpdateTouches(mutation, is_num ? config.num_pos : config.sub_pos)) {
    return;
  }
  if (mutation.kind == core::MutationKind::Update &&
      mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }
  const auto &rows =
      mutation.kind == core::MutationKind::Insert ? mutation.new_rows : mutation.old_rows;
  core::ParallelReduce(
      services, rows.size(), kAccumulateGrain, buffer,
      [&](SecEdgarQ3TxnBuffer &out, std::size_t begin, std::size_t end) {
        is_num ? AccumulateNumRows(config, mutation, begin, end, out)
               : AccumulateSubRows(config, mutation, begin, end, out);
      },
      [](SecEdgarQ3TxnBuffer &into, SecEdgarQ3TxnBuffer &&from) {
        into.Absorb(std::move(from));
      });
}

core::StagedMaintenanceOutcome
SecEdgarQ3Module::StageMaintenance(core::AuxEngineServices &services,
                                   const core::InvalidationPlan &,
                                   core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ3TxnBuffer &>(base_buffer);
  using core::MaintenanceAction;
  if (!buffer.HasPendingChanges()) {
    return {};
  }
  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false, MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    return RebuildOutcome(decision.action == MaintenanceAction::ApplyDelta
                              ? MaintenanceAction::RebuildAll
                              : decision.action);
  }
  if (buffer.Ops().empty()) {
    return {};
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Ops().size()), 1, true, false, true, false,
       false, MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != MaintenanceAction::ApplyDelta) {
    return RebuildOutcome(decision.action);
  }

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  const SecEdgarQ3StateImage *typed = nullptr;
  core::Version version = 0;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == kStatePartition) {
      typed = AsQ3Image(image);
      version = image.version;
    }
  }
  if (!typed) {
    return RebuildOutcome(MaintenanceAction::RebuildAll);
  }

  Work work(typed->state.get());
  using OpKind = SecEdgarQ3TxnBuffer::OpKind;
  const auto &strings = buffer.Strings();
  for (const auto &op : buffer.Ops()) {
    KeyRef ref;
    ref.key = op.key;
    if (op.key & kSecEdgarQ3OverflowBit) {
      ref.str = &strings[static_cast<std::size_t>(op.key & ~kSecEdgarQ3OverflowBit)];
    }
    bool ok = true;
    switch (op.kind) {
    case OpKind::Num:
      ok = work.ApplyNum(ref, strings[op.uom_idx], {op.value, op.value_lo}, op.sign);
      break;
    case OpKind::SubRemove:
      ok = work.RemoveSub(ref);
      break;
    case OpKind::SubAdd: {
      const bool name_null = (op.flags & 1) != 0;
      ok = work.AddSub(ref, name_null ? nullptr : &strings[op.name_idx],
                       (op.flags & 2) != 0, op.cik, op.fy);
      break;
    }
    }
    if (!ok) {
      return RebuildOutcome(MaintenanceAction::RebuildAll);
    }
  }

  core::StagedMaintenanceOutcome outcome;
  outcome.action = MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      Id(), kStatePartition, version, MakeStateImage(work.Finish())));
  outcome.expected_version = 0;
  return outcome;
}

bool SecEdgarQ3Module::CheckValidity(core::AuxEngineServices &services,
                                     const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

namespace {

struct QueryParams {
  bool is_null = false;
  int64_t fy = kSecEdgarQ3DefaultFy;
  std::string uom = kSecEdgarQ3DefaultUom;
};

QueryParams ParseArguments(const core::QueryArguments &args) {
  QueryParams p;
  if (args.values.empty()) {
    return p;
  }
  if (args.values.size() != 2) {
    throw std::invalid_argument(
        "sec_edgar q3 aux expects () or (fy BIGINT, uom VARCHAR)");
  }
  const auto &fy = args.values[0];
  const auto &uom = args.values[1];
  if (fy.is_null || uom.is_null) {
    p.is_null = true; // a comparison with NULL is never true: empty result
    return p;
  }
  if (std::holds_alternative<int64_t>(fy.payload)) {
    p.fy = std::get<int64_t>(fy.payload);
  } else if (std::holds_alternative<uint64_t>(fy.payload)) {
    const uint64_t v = std::get<uint64_t>(fy.payload);
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      p.is_null = true; // no fiscal year (INTEGER) can equal it
      return p;
    }
    p.fy = static_cast<int64_t>(v);
  } else if (std::holds_alternative<double>(fy.payload)) {
    const double v = std::get<double>(fy.payload);
    if (!(v == std::floor(v)) || !(std::fabs(v) < 9.0e18)) {
      p.is_null = true; // fractional / out of range: equals no integer fy
      return p;
    }
    p.fy = static_cast<int64_t>(v);
  } else if (std::holds_alternative<std::string>(fy.payload)) {
    const std::string &text = std::get<std::string>(fy.payload);
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
      p.is_null = true; // not an int64: equals no fiscal year
      return p;
    }
    p.fy = v;
  } else {
    throw std::invalid_argument("sec_edgar q3 aux: unsupported fy argument");
  }
  if (!std::holds_alternative<std::string>(uom.payload)) {
    throw std::invalid_argument("sec_edgar q3 aux: uom must be a string");
  }
  p.uom = std::get<std::string>(uom.payload);
  return p;
}

} // namespace

void SecEdgarQ3Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const QueryParams params = ParseArguments(arguments);
  if (params.is_null) {
    EmitRows(sink, nullptr);
    return;
  }
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  if (snapshot.available) {
    for (const auto &image : snapshot.partitions) {
      if (image.partition != kStatePartition) {
        continue;
      }
      if (const auto *typed = AsQ3Image(image)) {
        EmitOutput(services, *typed->state, params.fy, params.uom, sink);
        return;
      }
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void SecEdgarQ3Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments,
                                       core::AuxRowSink &sink) {
  const QueryParams params = ParseArguments(arguments);
  if (params.is_null) {
    EmitRows(sink, nullptr);
    return;
  }
  EmitOutput(services, BuildState(services), params.fy, params.uom, sink);
}

void SecEdgarQ3NumTotals::Add(std::string_view adsh, std::string_view uom, double value) {
  std::string key(uom);
  key.push_back('\0');
  key.append(adsh);
  Total &t = totals[key];
  DdAdd(t.sum, t.comp, value, 0.0);
  ++t.count;
}

void SecEdgarQ3NumTotals::Merge(SecEdgarQ3NumTotals &&from) {
  for (auto &[key, total] : from.totals) {
    Total &t = totals[key];
    DdAdd(t.sum, t.comp, total.sum, total.comp);
    t.count += total.count;
  }
}

void SecEdgarQ3NumTotals::ForEach(
    const std::function<void(std::string_view, std::string_view, const Total &)> &fn) const {
  for (const auto &[key, total] : totals) {
    const auto split = key.find('\0');
    fn(std::string_view(key).substr(split + 1), std::string_view(key).substr(0, split), total);
  }
}

SecEdgarQ3State
SecEdgarQ3Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q3 requires a typed initial scan source");
  }
  Work work(nullptr, /*bulk=*/true);
  const auto add_total = [&](std::string_view adsh, std::string_view uom, double sum, double comp,
                             int64_t count) {
    if (count <= 0) {
      return;
    }
    uint32_t uid;
    work.UomId(std::string(uom), /*create=*/true, uid);
    IndexWork &ui = work.Uom(uid);
    uint64_t key = 0;
    if (!ResolveIn(ui, MakeRef(std::string(adsh)), /*create=*/true, key)) {
      return;
    }
    ui.BulkAdd(key, sum, comp, count);
  };
  SecEdgarQ3NumTotals totals;
  if (initial_scan->ScanNumTotals(services, totals)) {
    totals.ForEach([&](std::string_view adsh, std::string_view uom, const SecEdgarQ3NumTotals::Total &t) {
      add_total(adsh, uom, t.sum, t.comp, t.count);
    });
  } else {
    initial_scan->ScanNumAggregates([&](const SecEdgarQ3NumAggInput &input) {
      add_total(input.adsh, input.uom, input.sum, 0.0, input.count);
    });
  }
  std::unordered_map<int32_t, uint64_t> fy_filings;
  initial_scan->ScanSub([&](const SecEdgarQ3SubInput &input) {
    if (input.fy_is_null) {
      return;
    }
    ++fy_filings[input.fy];
    if (!work.AddSub(MakeRef(input.adsh), input.name_is_null ? nullptr : &input.name,
                     input.cik_is_null, input.cik, input.fy)) {
      throw std::runtime_error("SEC EDGAR Q3: duplicate sub.adsh");
    }
  });
  const auto state = work.Finish();
  // Cohorts of the common parameters are served without a first-read build:
  // the defaults, then every (fy, uom) of the fiscal years holding >= 2% of
  // the filings and the units of measure with the most filings (data-driven,
  // bounded by kSecEdgarQ3PrebuiltCohorts; any other pair is still built on
  // first use). Each costs one pass over its unit's adsh index (~ms).
  std::vector<std::pair<uint64_t, int32_t>> fys;
  uint64_t filings = 0;
  for (const auto &[fy, n] : fy_filings) {
    filings += n;
  }
  for (const auto &[fy, n] : fy_filings) {
    if (n * 50 >= filings) {
      fys.emplace_back(n, fy);
    }
  }
  std::sort(fys.rbegin(), fys.rend());
  std::vector<std::pair<uint64_t, uint32_t>> uoms;
  for (uint32_t uid = 0; uid < state->uom_index.size(); ++uid) {
    if (state->uom_index[uid]) {
      uoms.emplace_back(state->uom_index[uid]->entry_count, uid);
    }
  }
  std::sort(uoms.rbegin(), uoms.rend());
  SecEdgarQ3CohortFor(services, *state, kSecEdgarQ3DefaultFy, kSecEdgarQ3DefaultUom);
  if (!fys.empty()) {
    const std::size_t uom_take =
        std::min(uoms.size(), std::max<std::size_t>(1, kSecEdgarQ3PrebuiltCohorts / fys.size()));
    for (std::size_t u = 0; u < uom_take; ++u) {
      for (const auto &fy : fys) {
        SecEdgarQ3CohortFor(services, *state, fy.second,
                            state->uom_dict->names[uoms[u].second]);
      }
    }
  }
  return *state;
}

void SecEdgarQ3Module::EmitRows(
    core::AuxRowSink &sink,
    const std::shared_ptr<const std::vector<core::AuxRow>> &rows) const {
  sink.Begin({{{"name", core::AuxType::String, true},
               {"cik", core::AuxType::Int64, true},
               {"total_value", core::AuxType::Double, false}}});
  if (rows) {
    for (const auto &row : *rows) {
      sink.WriteRow(row);
    }
  }
  sink.End();
}

void SecEdgarQ3Module::EmitOutput(const core::AuxEngineServices &services,
                                  const SecEdgarQ3State &state, int64_t fy,
                                  const std::string &uom,
                                  core::AuxRowSink &sink) const {
  const auto totals = SecEdgarQ3CohortFor(services, state, fy, uom);
  EmitRows(sink, totals ? totals->OutputRows() : nullptr);
}

} // namespace accelerator_codegen::aux_state::query

