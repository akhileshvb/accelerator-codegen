#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
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
constexpr int64_t kNullCikKey = std::numeric_limits<int64_t>::min();

inline uint64_t KeyHash(uint64_t key) { return key * 0x9E3779B97F4A7C15ULL; }
inline std::size_t TopOf(uint64_t key) {
  return static_cast<std::size_t>(KeyHash(key) >> 60);
}
inline std::size_t MidOf(uint64_t key) {
  return static_cast<std::size_t>((KeyHash(key) >> 56) & 15);
}

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

class IndexWork {
public:
  explicit IndexWork(const SecEdgarQ3AdshIndex *base) {
    if (base) {
      for (std::size_t i = 0; i < kSecEdgarQ3Fanout; ++i) {
        mids[i] = base->mids[i];
      }
      overflow = base->overflow;
      entry_count = base->entry_count;
    } else {
      for (std::size_t i = 0; i < kSecEdgarQ3Fanout; ++i) {
        owned_mids[i] = std::make_shared<SecEdgarQ3IndexMid>();
        for (std::size_t j = 0; j < kSecEdgarQ3Fanout; ++j) {
          auto shard = std::make_shared<SecEdgarQ3Shard>();
          owned_shards[i * kSecEdgarQ3Fanout + j] = shard;
          owned_mids[i]->shards[j] = shard;
        }
        mids[i] = owned_mids[i];
      }
    }
  }

  // Resolves an adsh string to its key. Non-conforming strings are interned
  // in the overflow dictionary when `create` is set.
  bool ResolveKey(const std::string &adsh, bool create, uint64_t &key) {
    if (SecEdgarQ3PackAdsh(adsh.data(), adsh.size(), key)) {
      return true;
    }
    if (overflow) {
      const auto it = overflow->find(adsh);
      if (it != overflow->end()) {
        key = kSecEdgarQ3OverflowBit | it->second;
        return true;
      }
    }
    if (!create) {
      return false;
    }
    if (!overflow_owned) {
      overflow_owned = overflow
                           ? std::make_shared<SecEdgarQ3OverflowMap>(*overflow)
                           : std::make_shared<SecEdgarQ3OverflowMap>();
      overflow = overflow_owned;
    }
    const auto id = static_cast<uint32_t>(overflow_owned->size());
    overflow_owned->emplace(adsh, id);
    key = kSecEdgarQ3OverflowBit | id;
    return true;
  }

  SecEdgarQ3Entry *Find(uint64_t key) {
    const auto t = TopOf(key);
    const auto m = MidOf(key);
    const auto slot = t * kSecEdgarQ3Fanout + m;
    const SecEdgarQ3Shard *shard =
        owned_shards[slot] ? owned_shards[slot].get() : mids[t]->shards[m].get();
    const auto &entries = shard->entries;
    auto it = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const SecEdgarQ3Entry &e, uint64_t k) { return e.key < k; });
    if (it == entries.end() || it->key != key) {
      return nullptr;
    }
    const auto idx = static_cast<std::size_t>(it - entries.begin());
    return &Own(t, m)->entries[idx];
  }

  SecEdgarQ3Entry *FindOrCreate(uint64_t key) {
    if (auto *e = Find(key)) {
      return e;
    }
    auto *shard = Own(TopOf(key), MidOf(key));
    auto &entries = shard->entries;
    auto it = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const SecEdgarQ3Entry &e, uint64_t k) { return e.key < k; });
    SecEdgarQ3Entry entry;
    entry.key = key;
    it = entries.insert(it, entry);
    ++entry_count;
    return &*it;
  }

  // Bulk loading (unsorted appends, then Seal()); only valid on a fresh index.
  void BulkAdd(uint64_t key, double sum, int32_t cnt) {
    SecEdgarQ3Entry entry;
    entry.key = key;
    entry.sum = sum;
    entry.cnt = cnt;
    owned_shards[TopOf(key) * kSecEdgarQ3Fanout + MidOf(key)]->entries.push_back(
        entry);
  }

  void Seal() {
    entry_count = 0;
    for (auto &shard : owned_shards) {
      if (!shard) {
        continue;
      }
      auto &entries = shard->entries;
      std::sort(entries.begin(), entries.end(),
                [](const SecEdgarQ3Entry &a, const SecEdgarQ3Entry &b) {
                  return a.key < b.key;
                });
      // merge duplicates (cannot happen for GROUP BY adsh input)
      std::size_t out = 0;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (out > 0 && entries[out - 1].key == entries[i].key) {
          entries[out - 1].sum += entries[i].sum;
          entries[out - 1].cnt += entries[i].cnt;
        } else {
          entries[out++] = entries[i];
        }
      }
      entries.resize(out);
      entries.shrink_to_fit();
      entry_count += out;
    }
  }

  std::shared_ptr<const SecEdgarQ3AdshIndex> Finish() {
    auto index = std::make_shared<SecEdgarQ3AdshIndex>();
    for (std::size_t i = 0; i < kSecEdgarQ3Fanout; ++i) {
      index->mids[i] = mids[i];
    }
    index->overflow = overflow;
    index->entry_count = entry_count;
    return index;
  }

private:
  SecEdgarQ3Shard *Own(std::size_t t, std::size_t m) {
    const auto slot = t * kSecEdgarQ3Fanout + m;
    if (owned_shards[slot]) {
      return owned_shards[slot].get();
    }
    if (!owned_mids[t]) {
      owned_mids[t] = std::make_shared<SecEdgarQ3IndexMid>(*mids[t]);
      mids[t] = owned_mids[t];
    }
    owned_shards[slot] =
        std::make_shared<SecEdgarQ3Shard>(*owned_mids[t]->shards[m]);
    owned_mids[t]->shards[m] = owned_shards[slot];
    return owned_shards[slot].get();
  }

  std::array<std::shared_ptr<const SecEdgarQ3IndexMid>, kSecEdgarQ3Fanout> mids;
  std::array<std::shared_ptr<SecEdgarQ3IndexMid>, kSecEdgarQ3Fanout> owned_mids;
  std::array<std::shared_ptr<SecEdgarQ3Shard>,
             kSecEdgarQ3Fanout * kSecEdgarQ3Fanout>
      owned_shards;
  std::shared_ptr<const SecEdgarQ3OverflowMap> overflow;
  std::shared_ptr<SecEdgarQ3OverflowMap> overflow_owned;
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

  void Add(uint32_t idx, double dsum, int32_t dcnt) {
    const auto c = idx / kSecEdgarQ3AggChunk;
    const auto i = idx % kSecEdgarQ3AggChunk;
    if (!owned[c]) {
      owned[c] = std::make_shared<SecEdgarQ3AggChunkData>(*shared[c]);
      shared[c] = owned[c];
    }
    auto &chunk = *owned[c];
    chunk.cnt[i] += dcnt;
    chunk.sum[i] += dsum;
    if (chunk.cnt[i] == 0) {
      chunk.sum[i] = 0.0;
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

class DirWork {
public:
  explicit DirWork(std::shared_ptr<const SecEdgarQ3GroupDir> base)
      : shared(std::move(base)) {
    base_ptr = shared.get();
  }

  const SecEdgarQ3GroupDir &View() const {
    if (cloned) {
      return *cloned;
    }
    if (base_ptr) {
      return *base_ptr;
    }
    static const SecEdgarQ3GroupDir empty;
    return empty;
  }

  uint32_t CikIdOfGroup(uint32_t group) const {
    return View().group_cik_id[group];
  }

  // Finds or creates the (name, cik) group; grows the aggregate arrays.
  uint32_t FindOrAddGroup(const std::string *name, bool cik_null, int32_t cik,
                          AggWork &groups, AggWork &ciks) {
    EnsureMaps();
    const std::string key = GroupKey(name, cik_null, cik);
    const auto it = group_map.find(key);
    if (it != group_map.end()) {
      return it->second;
    }
    Clone();
    // cik id
    const int64_t cik_key = cik_null ? kNullCikKey : static_cast<int64_t>(cik);
    uint32_t cik_id;
    const auto cit = cik_map.find(cik_key);
    if (cit == cik_map.end()) {
      cik_id = static_cast<uint32_t>(cloned->cik_value.size());
      cloned->cik_value.push_back(cik_null ? 0 : cik);
      cloned->cik_is_null.push_back(cik_null ? 1 : 0);
      cik_map.emplace(cik_key, cik_id);
      ciks.Grow(cik_id + 1);
    } else {
      cik_id = cit->second;
    }
    const auto group = static_cast<uint32_t>(cloned->group_flags.size());
    if (name) {
      cloned->name_pool.append(*name);
    }
    cloned->name_off.push_back(static_cast<uint32_t>(cloned->name_pool.size()));
    cloned->group_flags.push_back(
        static_cast<uint8_t>((name == nullptr ? 1 : 0) | (cik_null ? 2 : 0)));
    cloned->group_cik.push_back(cik_null ? 0 : cik);
    cloned->group_cik_id.push_back(cik_id);
    group_map.emplace(key, group);
    groups.Grow(group + 1);
    return group;
  }

  std::shared_ptr<const SecEdgarQ3GroupDir> Finish() {
    if (cloned) {
      cloned->name_pool.shrink_to_fit();
      return cloned;
    }
    if (shared) {
      return shared;
    }
    return std::make_shared<SecEdgarQ3GroupDir>();
  }

private:
  static std::string GroupKey(const std::string *name, bool cik_null,
                              int32_t cik) {
    std::string key;
    key.reserve(6 + (name ? name->size() : 0));
    key.push_back(name ? 'n' : 'N');
    key.push_back(cik_null ? 'C' : 'c');
    if (!cik_null) {
      key.append(reinterpret_cast<const char *>(&cik), sizeof(cik));
    }
    if (name) {
      key.append(*name);
    }
    return key;
  }

  void Clone() {
    if (!cloned) {
      cloned = base_ptr ? std::make_shared<SecEdgarQ3GroupDir>(*base_ptr)
                        : std::make_shared<SecEdgarQ3GroupDir>();
    }
  }

  void EnsureMaps() {
    if (maps_ready) {
      return;
    }
    maps_ready = true;
    const auto &dir = View();
    group_map.reserve(dir.GroupCount());
    for (uint32_t g = 0; g < dir.GroupCount(); ++g) {
      const bool name_null = (dir.group_flags[g] & 1) != 0;
      const bool cik_null = (dir.group_flags[g] & 2) != 0;
      std::string name(dir.name_pool.data() + dir.name_off[g],
                       dir.name_off[g + 1] - dir.name_off[g]);
      group_map.emplace(
          GroupKey(name_null ? nullptr : &name, cik_null, dir.group_cik[g]), g);
    }
    for (uint32_t c = 0; c < dir.CikCount(); ++c) {
      cik_map.emplace(dir.cik_is_null[c] ? kNullCikKey
                                         : static_cast<int64_t>(dir.cik_value[c]),
                      c);
    }
  }

  std::shared_ptr<const SecEdgarQ3GroupDir> shared;
  const SecEdgarQ3GroupDir *base_ptr = nullptr;
  std::shared_ptr<SecEdgarQ3GroupDir> cloned;
  bool maps_ready = false;
  std::unordered_map<std::string, uint32_t> group_map;
  std::unordered_map<int64_t, uint32_t> cik_map;
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

  // dsum / dcnt: change of the filing's qualifying-row aggregate.
  bool ApplyNum(uint64_t key, double dsum, int32_t dcnt) {
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
    entry->cnt += dcnt;
    if (entry->cnt < 0) {
      return false;
    }
    entry->sum += dsum;
    if (entry->cnt == 0) {
      entry->sum = 0.0;
    }
    const double effective = entry->sum - old_sum;
    const int32_t group = entry->group;
    if (group >= 0) {
      // NaN / inf contributions cannot be subtracted back out of the group
      // and cik sums: let a rebuild recompute them.
      if (!std::isfinite(old_sum) || !std::isfinite(entry->sum)) {
        return false;
      }
      groups.Add(static_cast<uint32_t>(group), effective, dcnt);
      ciks.Add(dir.CikIdOfGroup(static_cast<uint32_t>(group)), effective, dcnt);
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
    const int32_t cnt = entry->cnt;
    const uint32_t group = dir.FindOrAddGroup(name, cik_null, cik, groups, ciks);
    // FindOrAddGroup does not touch the index, the pointer is still valid.
    entry->group = static_cast<int32_t>(group);
    if (cnt != 0) {
      groups.Add(group, sum, cnt);
      ciks.Add(dir.CikIdOfGroup(group), sum, cnt);
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
    const int32_t cnt = entry->cnt;
    if (!std::isfinite(sum)) {
      return false; // cannot subtract NaN / inf from the group sums
    }
    entry->group = -1;
    if (cnt != 0) {
      groups.Add(group, -sum, -cnt);
      ciks.Add(dir.CikIdOfGroup(group), -sum, -cnt);
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
  const __m128i zero = _mm_setzero_si128();
  for (const auto &chunk_ptr : array.chunks) {
    const auto &chunk = *chunk_ptr;
    for (std::size_t i = 0; i < kSecEdgarQ3AggChunk; i += 8) {
      const __m128i c0 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(chunk.cnt + i));
      const __m128i c1 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(chunk.cnt + i + 4));
      const __m128i m0 = _mm_cmpgt_epi32(c0, zero);
      const __m128i m1 = _mm_cmpgt_epi32(c1, zero);
      n += static_cast<unsigned>(__builtin_popcount(
          _mm_movemask_ps(_mm_castsi128_ps(m0)) |
          (_mm_movemask_ps(_mm_castsi128_ps(m1)) << 4)));
      const __m256d s0 = _mm256_load_pd(chunk.sum + i);
      const __m256d s1 = _mm256_load_pd(chunk.sum + i + 4);
      acc0 = _mm256_add_pd(
          acc0, _mm256_and_pd(s0, _mm256_castsi256_pd(_mm256_cvtepi32_epi64(m0))));
      acc1 = _mm256_add_pd(
          acc1, _mm256_and_pd(s1, _mm256_castsi256_pd(_mm256_cvtepi32_epi64(m1))));
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
  const __m128i zero = _mm_setzero_si128();
  uint32_t base = 0;
  for (const auto &chunk_ptr : array.chunks) {
    const auto &chunk = *chunk_ptr;
    for (std::size_t i = 0; i < kSecEdgarQ3AggChunk; i += 4) {
      const __m256d s = _mm256_load_pd(chunk.sum + i);
      const __m128i c =
          _mm_load_si128(reinterpret_cast<const __m128i *>(chunk.cnt + i));
      const __m256d gt = _mm256_cmp_pd(s, thr, _CMP_GT_OQ);
      const __m256d live = _mm256_castsi256_pd(
          _mm256_cvtepi32_epi64(_mm_cmpgt_epi32(c, zero)));
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

bool TieLess(const SecEdgarQ3GroupDir &dir, uint32_t a, uint32_t b) {
  const bool a_null = (dir.group_flags[a] & 1) != 0;
  const bool b_null = (dir.group_flags[b] & 1) != 0;
  if (a_null != b_null) {
    return a_null;
  }
  if (!a_null) {
    const std::size_t la = dir.name_off[a + 1] - dir.name_off[a];
    const std::size_t lb = dir.name_off[b + 1] - dir.name_off[b];
    const int cmp = std::memcmp(dir.name_pool.data() + dir.name_off[a],
                                dir.name_pool.data() + dir.name_off[b],
                                std::min(la, lb));
    if (cmp != 0) {
      return cmp < 0;
    }
    if (la != lb) {
      return la < lb;
    }
  }
  return dir.group_cik[a] < dir.group_cik[b];
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
    if (dir.group_flags[g] & 1) {
      out.values.push_back(core::AuxValue::Null(core::AuxType::String));
    } else {
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::String,
          std::string(dir.name_pool.data() + dir.name_off[g],
                      dir.name_off[g + 1] - dir.name_off[g])));
    }
    if (dir.group_flags[g] & 2) {
      out.values.push_back(core::AuxValue::Null(core::AuxType::Int64));
    } else {
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::Int64, static_cast<int64_t>(dir.group_cik[g])));
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
    out.name_is_null = (dir.group_flags[row.group] & 1) != 0;
    if (!out.name_is_null) {
      out.name.assign(dir.name_pool.data() + dir.name_off[row.group],
                      dir.name_off[row.group + 1] - dir.name_off[row.group]);
    }
    out.cik_is_null = (dir.group_flags[row.group] & 2) != 0;
    out.cik = dir.group_cik[row.group];
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
  std::vector<const SecEdgarQ3Entry *> all;
  all.reserve(index.entry_count);
  for (const auto &mid : index.mids) {
    for (const auto &shard : mid->shards) {
      for (const auto &entry : shard->entries) {
        all.push_back(&entry);
      }
    }
  }
  std::sort(all.begin(), all.end(),
            [](const SecEdgarQ3Entry *a, const SecEdgarQ3Entry *b) {
              return a->key < b->key;
            });
  uint64_t prev = 0;
  for (const auto *entry : all) {
    PutVarint(writer, entry->key - prev);
    prev = entry->key;
    PutSum(writer, entry->sum);
    PutVarint(writer, static_cast<uint64_t>(entry->cnt));
    PutVarint(writer, static_cast<uint64_t>(entry->group + 1));
  }
  const uint32_t overflow_count =
      index.overflow ? static_cast<uint32_t>(index.overflow->size()) : 0;
  writer.U32(overflow_count);
  if (index.overflow) {
    for (const auto &[adsh, id] : *index.overflow) {
      writer.String(adsh);
      writer.U32(id);
    }
  }
}

void EncodeTotals(const SecEdgarQ3Totals &totals, ByteWriter &writer) {
  const auto &dir = *totals.dir;
  core::WriteImageHeader(writer, kStateMagic, kFormatVersion,
                         static_cast<uint32_t>(dir.GroupCount()));
  writer.U32(static_cast<uint32_t>(dir.CikCount()));
  writer.String(dir.name_pool);
  for (const auto off : dir.name_off) {
    writer.U32(off);
  }
  for (std::size_t g = 0; g < dir.GroupCount(); ++g) {
    writer.U8(dir.group_flags[g]);
    writer.I32(dir.group_cik[g]);
    PutVarint(writer, dir.group_cik_id[g]);
  }
  for (std::size_t c = 0; c < dir.CikCount(); ++c) {
    writer.U8(dir.cik_is_null[c]);
    writer.I32(dir.cik_value[c]);
  }
  auto write_agg = [&](const SecEdgarQ3AggArray &array) {
    for (uint32_t idx = 0; idx < array.size; ++idx) {
      const auto &chunk = *array.chunks[idx / kSecEdgarQ3AggChunk];
      PutSum(writer, chunk.sum[idx % kSecEdgarQ3AggChunk]);
      PutVarint(writer, static_cast<uint64_t>(chunk.cnt[idx % kSecEdgarQ3AggChunk]));
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
    for (const auto &mid : state.index->mids) {
      bytes += sizeof(SecEdgarQ3IndexMid) + 32;
      for (const auto &shard : mid->shards) {
        bytes += sizeof(SecEdgarQ3Shard) + 32 +
                 shard->entries.capacity() * sizeof(SecEdgarQ3Entry);
      }
    }
    if (state.index->overflow) {
      for (const auto &[adsh, id] : *state.index->overflow) {
        bytes += adsh.capacity() + 48;
      }
    }
  }
  if (state.totals) {
    const auto &dir = *state.totals->dir;
    bytes += sizeof(dir) + dir.name_pool.capacity() +
             dir.name_off.capacity() * 4 + dir.group_flags.capacity() +
             dir.group_cik.capacity() * 4 + dir.group_cik_id.capacity() * 4 +
             dir.cik_value.capacity() * 4 + dir.cik_is_null.capacity();
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
    work.index.BulkAdd(key, input.sum, static_cast<int32_t>(input.count));
  });
  work.index.Seal();
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
