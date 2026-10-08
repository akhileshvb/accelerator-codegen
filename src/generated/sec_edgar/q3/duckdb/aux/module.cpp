#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
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
constexpr int32_t kTargetFy = 2022;
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
  double value = 0.0;
  bool relevant = false; // adsh non-null, uom = 'USD', value non-null
};

// num layout (see catalog MakeNumTableAuxRow): 0 adsh, 5 uom, 7 value
bool ReadNumRow(const core::AuxRow &row, NumRow &out) {
  if (row.values.size() < 8) {
    return false;
  }
  const auto &adsh = row.values[0];
  const auto &uom = row.values[5];
  const auto &value = row.values[7];
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
  out.relevant = std::get<std::string>(uom.payload) == "USD";
  return true;
}

struct SubRow {
  const std::string *adsh = nullptr;
  const std::string *name = nullptr; // null when NULL
  int32_t cik = 0;
  bool cik_null = false;
  bool in_cohort = false; // fy = 2022
};

// sub layout: 0 adsh, 1 name, 2 cik, 3 fy
bool ReadSubRow(const core::AuxRow &row, SubRow &out) {
  if (row.values.size() < 4) {
    return false;
  }
  const auto &adsh = row.values[0];
  const auto &name = row.values[1];
  const auto &cik = row.values[2];
  const auto &fy = row.values[3];
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
  int32_t fy_value = 0;
  if (fy.is_null) {
    out.in_cohort = false;
  } else {
    if (!ReadIntCell(fy, fy_value)) {
      return false;
    }
    out.in_cohort = fy_value == kTargetFy;
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
  explicit IndexWork(const SecEdgarQ3AdshIndex *base) : overflow(base ? &base->overflow : nullptr) {
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
  void BulkAdd(uint64_t key, double sum, int64_t cnt) {
    if (entry_count >= (static_cast<uint64_t>(kLeafTarget) << top_bits << mid_bits) &&
        top_bits + mid_bits < kMaxIndexBits) {
      Repartition(top_bits + mid_bits + 1);
    }
    SecEdgarQ3Entry entry;
    entry.sum = sum;
    entry.cnt = cnt;
    auto *leaf = OwnLeaf(LeafOf(key));
    const auto inserted = leaf->Insert(static_cast<int64_t>(key), entry);
    if (inserted.second) {
      ++entry_count;
    } else {
      DdAdd(inserted.first->sum, inserted.first->comp, sum, 0.0);
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
  OverflowWork overflow;
  uint64_t entry_count = 0;
};

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
  uint32_t FindOrAddGroup(const std::string *name, bool cik_null, int32_t cik,
                          AggWork &groups, AggWork &ciks) {
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
      ciks.Grow(cik_id + 1);
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
    groups.Grow(group + 1);
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

class Work {
public:
  explicit Work(const SecEdgarQ3State *base)
      : index(base && base->index ? base->index.get() : nullptr),
        dir(base && base->totals ? base->totals->dir
                                 : std::shared_ptr<const SecEdgarQ3GroupDir>()),
        groups(base && base->totals ? &base->totals->groups : nullptr),
        ciks(base && base->totals ? &base->totals->ciks : nullptr) {}

  IndexWork index;
  DirWork dir;
  AggWork groups;
  AggWork ciks;

  void Propagate(uint32_t group, TwoD d, int64_t dcnt) {
    groups.Add(group, d, dcnt);
    ciks.Add(dir.CikIdOfGroup(group), d, dcnt);
  }

  // dsum / dcnt: change of the filing's qualifying-row aggregate.
  bool ApplyNum(uint64_t key, double dsum, int64_t dcnt) {
    auto *entry = index.Find(key);
    if (!entry) {
      if (dcnt <= 0) {
        return false; // delete / value update of an untracked filing
      }
      entry = index.FindOrCreate(key);
    } else if (dcnt == 0 && entry->cnt == 0) {
      return false;
    }
    const double old_sum = entry->sum;
    const double old_comp = entry->comp;
    entry->cnt += dcnt;
    if (entry->cnt < 0) {
      return false;
    }
    DdAdd(entry->sum, entry->comp, dsum, 0.0);
    if (entry->cnt == 0) {
      entry->sum = 0.0;
      entry->comp = 0.0;
    }
    const int32_t group = entry->group;
    if (group >= 0) {
      // NaN / inf contributions cannot be subtracted back out of the group
      // and cik sums: let a rebuild recompute them.
      if (!std::isfinite(old_sum) || !std::isfinite(entry->sum)) {
        return false;
      }
      // effective change = new - old as a double-double
      const TwoD diff = TwoSum(entry->sum, -old_sum);
      Propagate(static_cast<uint32_t>(group),
                {diff.hi, diff.lo + (entry->comp - old_comp)}, dcnt);
    } else if (entry->cnt == 0) {
      index.Erase(key);
    }
    return !groups.corrupt && !ciks.corrupt;
  }

  bool AddMember(uint64_t key, const std::string *name, bool cik_null,
                 int32_t cik) {
    auto *entry = index.FindOrCreate(key);
    if (entry->group >= 0) {
      return false; // duplicate sub.adsh in the cohort
    }
    const double sum = entry->sum;
    const double comp = entry->comp;
    const int64_t cnt = entry->cnt;
    const uint32_t group = dir.FindOrAddGroup(name, cik_null, cik, groups, ciks);
    // FindOrAddGroup does not touch the index, the pointer is still valid.
    entry->group = static_cast<int32_t>(group);
    if (cnt != 0) {
      Propagate(group, {sum, comp}, cnt);
    }
    return !groups.corrupt && !ciks.corrupt;
  }

  bool RemoveMember(uint64_t key) {
    auto *entry = index.Find(key);
    if (!entry || entry->group < 0) {
      return false;
    }
    const auto group = static_cast<uint32_t>(entry->group);
    const double sum = entry->sum;
    const double comp = entry->comp;
    const int64_t cnt = entry->cnt;
    if (!std::isfinite(sum)) {
      return false; // cannot subtract NaN / inf from the group sums
    }
    entry->group = -1;
    if (cnt != 0) {
      Propagate(group, {-sum, -comp}, -cnt);
    } else {
      index.Erase(key);
    }
    return !groups.corrupt && !ciks.corrupt;
  }

  std::shared_ptr<const SecEdgarQ3State> Finish() {
    auto totals = std::make_shared<SecEdgarQ3Totals>();
    totals->dir = dir.Finish();
    totals->groups = groups.Finish();
    totals->ciks = ciks.Finish();
    auto state = std::make_shared<SecEdgarQ3State>();
    state->index = index.Finish();
    state->totals = std::move(totals);
    return state;
  }
};

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
                 payload->state && payload->state->index &&
                 payload->state->totals
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
                   std::initializer_list<core::ColumnId> columns) {
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

std::vector<SecEdgarQ3OutputRow>
BuildSecEdgarQ3Output(const SecEdgarQ3State &state) {
  std::vector<SecEdgarQ3OutputRow> rows;
  if (!state.totals) {
    return rows;
  }
  const auto result = state.totals->Result();
  const auto &dir = *state.totals->dir;
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
void EncodeIndex(const SecEdgarQ3AdshIndex &index, ByteWriter &writer) {
  core::WriteImageHeader(writer, kIndexMagic, kFormatVersion,
                         static_cast<uint32_t>(index.entry_count));
  std::vector<std::pair<uint64_t, SecEdgarQ3Entry>> all;
  all.reserve(index.entry_count);
  for (const auto &mid : index.tops) {
    for (const auto &leaf : mid->leaves) {
      for (auto e : static_cast<const SecEdgarQ3Leaf &>(*leaf)) {
        all.emplace_back(static_cast<uint64_t>(e.first), e.second);
      }
    }
  }
  std::sort(all.begin(), all.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  uint64_t prev = 0;
  for (const auto &[key, entry] : all) {
    PutVarint(writer, key - prev);
    prev = key;
    PutSum(writer, entry.sum);
    PutSum(writer, entry.comp);
    PutVarint(writer, static_cast<uint64_t>(entry.cnt));
    PutVarint(writer, static_cast<uint64_t>(entry.group + 1));
  }
  writer.U32(index.overflow.count);
  for (uint32_t id = 0; id < index.overflow.count; ++id) {
    std::string_view s;
    index.overflow.strings.Get(id, s);
    writer.String(std::string(s));
    writer.U32(id);
  }
}

void EncodeTotals(const SecEdgarQ3Totals &totals, ByteWriter &writer) {
  const auto &dir = *totals.dir;
  core::WriteImageHeader(writer, kStateMagic, kFormatVersion,
                         static_cast<uint32_t>(dir.GroupCount()));
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
  auto write_agg = [&](const SecEdgarQ3AggArray &array) {
    for (uint32_t idx = 0; idx < array.size; ++idx) {
      const auto &chunk = *array.chunks[idx / kSecEdgarQ3AggChunk];
      const auto i = idx % kSecEdgarQ3AggChunk;
      PutSum(writer, chunk.sum[i]);
      PutSum(writer, chunk.comp[i]);
      PutVarint(writer, static_cast<uint64_t>(chunk.cnt[i]));
    }
  };
  write_agg(totals.groups);
  write_agg(totals.ciks);
}

} // namespace

std::vector<uint8_t> EncodeSecEdgarQ3Fy2022Cohort(const SecEdgarQ3State &state) {
  ByteWriter writer;
  if (state.index) {
    EncodeIndex(*state.index, writer);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeSecEdgarQ3CikTotals(const SecEdgarQ3State &state) {
  ByteWriter writer;
  if (state.index) {
    EncodeIndex(*state.index, writer);
  }
  if (state.totals) {
    EncodeTotals(*state.totals, writer);
  }
  return std::move(writer.bytes);
}

uint64_t SecEdgarQ3ResidentBytes(const SecEdgarQ3State &state) {
  uint64_t bytes = 0;
  if (state.index) {
    bytes += sizeof(SecEdgarQ3AdshIndex);
    for (const auto &mid : state.index->tops) {
      bytes += sizeof(SecEdgarQ3IndexMid) + 32 +
               mid->leaves.capacity() * sizeof(mid->leaves[0]);
      for (const auto &leaf : mid->leaves) {
        bytes += sizeof(SecEdgarQ3Leaf) + 32 + leaf->memory_bytes();
      }
    }
    const auto &ov = state.index->overflow;
    bytes += ov.strings.MemoryBytes() + ov.by_hash.memory_bytes() + ov.next.MemoryBytes();
  }
  if (state.totals) {
    const auto &dir = *state.totals->dir;
    bytes += sizeof(dir) + dir.names.MemoryBytes() + dir.meta.MemoryBytes() +
             dir.cik_meta.MemoryBytes() + dir.by_hash.memory_bytes() +
             dir.cik_ids.memory_bytes();
    bytes += (state.totals->groups.chunks.size() +
              state.totals->ciks.chunks.size()) *
             (sizeof(SecEdgarQ3AggChunkData) + 16);
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
}

bool SecEdgarQ3TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

void SecEdgarQ3TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  ops.clear();
  strings.clear();
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
  return {{config.num_table, {0, 5, 7}}, {config.sub_table, {0, 1, 2, 3}}};
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
    if (!UpdateTouches(mutation, {0, 5, 7})) {
      return {};
    }
    return AllPartitions();
  }
  if (mutation.table == config.sub_table) {
    if (!UpdateTouches(mutation, {0, 1, 2, 3})) {
      return {};
    }
    return AllPartitions();
  }
  return {};
}

void SecEdgarQ3Module::AccumulateMutation(const core::MutationBatch &mutation,
                                          core::AuxEngineServices &,
                                          core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<SecEdgarQ3TxnBuffer &>(base_buffer);
  using Op = SecEdgarQ3TxnBuffer::Op;
  using OpKind = SecEdgarQ3TxnBuffer::OpKind;

  if (mutation.table == config.num_table) {
    if (!UpdateTouches(mutation, {0, 5, 7})) {
      return;
    }
    auto num_op = [&](const NumRow &row, int sign) {
      Op op;
      op.kind = OpKind::Num;
      op.sign = static_cast<int8_t>(sign);
      op.key = buffer.KeyFor(*row.adsh);
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
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &raw : mutation.new_rows) {
        NumRow row;
        if (!ReadNumRow(raw, row)) {
          buffer.MarkRebuild();
          return;
        }
        if (!row.relevant) {
          continue;
        }
        if (mark_if_nonfinite(row)) {
          return;
        }
        num_op(row, 1);
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &raw : mutation.old_rows) {
        NumRow row;
        if (!ReadNumRow(raw, row)) {
          buffer.MarkRebuild();
          return;
        }
        if (!row.relevant) {
          continue;
        }
        if (mark_if_nonfinite(row)) {
          return;
        }
        num_op(row, -1);
      }
      break;
    case core::MutationKind::Update: {
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        NumRow old_row;
        NumRow new_row;
        if (!ReadNumRow(mutation.old_rows[i], old_row) ||
            !ReadNumRow(mutation.new_rows[i], new_row)) {
          buffer.MarkRebuild();
          return;
        }
        if (mark_if_nonfinite(old_row) || mark_if_nonfinite(new_row)) {
          return;
        }
        if (old_row.relevant && new_row.relevant &&
            *old_row.adsh == *new_row.adsh) {
          // value-only change: one lookup, no row-count change
          Op op;
          op.kind = OpKind::Num;
          op.sign = 0;
          op.key = buffer.KeyFor(*new_row.adsh);
          op.value = new_row.value - old_row.value;
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
    return;
  }

  if (mutation.table != config.sub_table) {
    return;
  }
  if (!UpdateTouches(mutation, {0, 1, 2, 3})) {
    return;
  }
  auto remove_op = [&](const SubRow &row) {
    Op op;
    op.kind = OpKind::SubRemove;
    op.key = buffer.KeyFor(*row.adsh);
    buffer.AddOp(op);
  };
  auto add_op = [&](const SubRow &row) {
    Op op;
    op.kind = OpKind::SubAdd;
    op.key = buffer.KeyFor(*row.adsh);
    op.cik = row.cik;
    op.flags = static_cast<uint8_t>((row.name == nullptr ? 1 : 0) |
                                    (row.cik_null ? 2 : 0));
    if (row.name) {
      op.name_idx = buffer.InternString(*row.name);
    }
    buffer.AddOp(op);
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &raw : mutation.new_rows) {
      SubRow row;
      if (!ReadSubRow(raw, row)) {
        buffer.MarkRebuild();
        return;
      }
      if (row.in_cohort) {
        add_op(row);
      }
    }
    break;
  case core::MutationKind::Delete:
    for (const auto &raw : mutation.old_rows) {
      SubRow row;
      if (!ReadSubRow(raw, row)) {
        buffer.MarkRebuild();
        return;
      }
      if (row.in_cohort) {
        remove_op(row);
      }
    }
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      SubRow old_row;
      SubRow new_row;
      if (!ReadSubRow(mutation.old_rows[i], old_row) ||
          !ReadSubRow(mutation.new_rows[i], new_row)) {
        buffer.MarkRebuild();
        return;
      }
      if (!old_row.in_cohort && !new_row.in_cohort) {
        continue;
      }
      if (old_row.in_cohort && new_row.in_cohort &&
          *old_row.adsh == *new_row.adsh && old_row.cik_null == new_row.cik_null &&
          (old_row.cik_null || old_row.cik == new_row.cik) &&
          (old_row.name == nullptr) == (new_row.name == nullptr) &&
          (old_row.name == nullptr || *old_row.name == *new_row.name)) {
        continue; // nothing observable changed
      }
      if (old_row.in_cohort) {
        remove_op(old_row);
      }
      if (new_row.in_cohort) {
        add_op(new_row);
      }
    }
    break;
  }
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
    uint64_t key = op.key;
    if (key & kSecEdgarQ3OverflowBit) {
      const auto local = static_cast<std::size_t>(key & ~kSecEdgarQ3OverflowBit);
      if (!work.index.ResolveKey(strings[local], /*create=*/true, key)) {
        return RebuildOutcome(MaintenanceAction::RebuildAll);
      }
    }
    bool ok = true;
    switch (op.kind) {
    case OpKind::Num:
      ok = work.ApplyNum(key, op.value, op.sign);
      break;
    case OpKind::SubRemove:
      ok = work.RemoveMember(key);
      break;
    case OpKind::SubAdd: {
      const bool name_null = (op.flags & 1) != 0;
      ok = work.AddMember(key, name_null ? nullptr : &strings[op.name_idx],
                          (op.flags & 2) != 0, op.cik);
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

void SecEdgarQ3Module::Execute(core::AuxEngineServices &services,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  if (snapshot.available) {
    for (const auto &image : snapshot.partitions) {
      if (image.partition != kStatePartition) {
        continue;
      }
      if (const auto *typed = AsQ3Image(image)) {
        EmitOutput(*typed->state, sink);
        return;
      }
    }
  }
  ExecuteFallback(services, arguments, sink);
}

void SecEdgarQ3Module::ExecuteFallback(core::AuxEngineServices &services,
                                       const core::QueryArguments &,
                                       core::AuxRowSink &sink) {
  EmitOutput(BuildState(services), sink);
}

SecEdgarQ3State
SecEdgarQ3Module::BuildState(core::AuxEngineServices &) const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q3 requires a typed initial scan source");
  }
  Work work(nullptr);
  initial_scan->ScanNumAggregates([&](const SecEdgarQ3NumAggInput &input) {
    if (input.count <= 0) {
      return;
    }
    uint64_t key = 0;
    if (!work.index.ResolveKey(input.adsh, /*create=*/true, key)) {
      return;
    }
    work.index.BulkAdd(key, input.sum, input.count);
  });
  initial_scan->ScanSub([&](const SecEdgarQ3SubInput &input) {
    if (input.fy_is_null || input.fy != kTargetFy) {
      return;
    }
    uint64_t key = 0;
    if (!work.index.ResolveKey(input.adsh, /*create=*/true, key)) {
      return;
    }
    if (!work.AddMember(key, input.name_is_null ? nullptr : &input.name,
                        input.cik_is_null, input.cik)) {
      throw std::runtime_error("SEC EDGAR Q3: duplicate sub.adsh in cohort");
    }
  });
  const auto state = work.Finish();
  return *state;
}

void SecEdgarQ3Module::EmitOutput(const SecEdgarQ3State &state,
                                  core::AuxRowSink &sink) const {
  sink.Begin({{{"name", core::AuxType::String, true},
               {"cik", core::AuxType::Int64, true},
               {"total_value", core::AuxType::Double, false}}});
  const auto rows = state.totals->OutputRows();
  for (const auto &row : *rows) {
    sink.WriteRow(row);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query

