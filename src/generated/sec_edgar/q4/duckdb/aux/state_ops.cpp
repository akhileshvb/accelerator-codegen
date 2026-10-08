#include "sec_edgar/q4/duckdb/aux/state_ops.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define Q4_HAVE_X86_SIMD 1
#endif

namespace accelerator_codegen::aux_state::query::q4 {
namespace {

constexpr int32_t kMinDistinctCik = 2;
constexpr size_t kOutputLimit = 500;

inline uint64_t EntryKey(uint32_t adsh_id, uint32_t tv_id) {
  return (static_cast<uint64_t>(adsh_id) << 32) | tv_id;
}

} // namespace

// ---------------------------------------------------------------- Flat

uint32_t Flat::GroupFor(int32_t sic, uint32_t tl) {
  const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(sic)) << 32) | tl;
  if (const uint32_t *found = group_index.Find(static_cast<int64_t>(key))) {
    return *found;
  }
  if (groups.size() >= kQ4None - 1) {
    throw std::length_error("sec_edgar q4: group id space exhausted");
  }
  const uint32_t id = static_cast<uint32_t>(groups.size());
  Group g;
  g.sic = sic;
  g.tl = tl;
  groups.push_back(g);
  group_index.Upsert(static_cast<int64_t>(key), id);
  return id;
}

void Flat::PairAdd(uint32_t group, uint32_t cik_id, int64_t delta) {
  if (cik_id == kQ4None || delta == 0) {
    return;
  }
  auto &cnt = pairs[Q4PairKey(group, cik_id)];
  const int64_t before = cnt;
  cnt += delta;
  if (cnt < 0) {
    bad = true;
    cnt = 0;
  }
  auto &g = groups[group];
  if (before <= 0 && cnt > 0) {
    ++g.distinct;
  } else if (before > 0 && cnt <= 0) {
    --g.distinct;
  }
}

void Flat::Contribute(const SecEdgarQ4Entry &e, int sign) {
  auto &g = groups[e.group];
  const double m = static_cast<double>(e.mult) * sign;
  Q4DDAddScaled(g.hi, g.lo, e.ns_hi, m);
  Q4DDAddScaled(g.hi, g.lo, e.ns_lo, m);
  const int64_t rows = static_cast<int64_t>(e.mult) * e.nc * sign;
  g.count += rows;
  if (g.count == 0) {
    g.hi = g.lo = 0.0;
  }
  PairAdd(e.group, cohort.cik_id[e.adsh_id], rows);
}

void Flat::AddRow(const SecEdgarQ4BridgeRow &row) {
  if (row.mult == 0) {
    return;
  }
  const uint32_t adsh_id = cohort.adsh.Insert(row.adsh);
  if (cohort.cik_id.size() <= adsh_id) {
    cohort.cik_id.resize(adsh_id + 1, kQ4None);
  }
  uint32_t cik = kQ4None;
  if (!row.cik_null) {
    if (const uint32_t *found = cik_index.Find(row.cik)) {
      cik = *found;
    } else {
      cik = static_cast<uint32_t>(cohort.ciks.size());
      cohort.ciks.push_back(row.cik);
      cik_index.Upsert(row.cik, cik);
    }
  }
  cohort.cik_id[adsh_id] = cik;

  std::string key;
  key.reserve(row.tag.size() + row.version.size() + 1);
  key.append(row.tag);
  key.push_back('\0');
  key.append(row.version);
  const uint32_t tv_id = tv.Insert(key);
  if (tv_tl.size() <= tv_id) {
    tv_tl.resize(tv_id + 1, kQ4None);
  }
  const uint32_t tl = row.tlabel_null ? kQ4None : tlabels.Insert(row.tlabel);
  tv_tl[tv_id] = tl;

  const int64_t ekey = static_cast<int64_t>(EntryKey(adsh_id, tv_id));
  if (entry_index.Contains(ekey)) {
    throw std::runtime_error("sec_edgar q4: duplicate bridge key in scan");
  }
  if (entries.size() >= kQ4None - 1) {
    throw std::length_error("sec_edgar q4: entry id space exhausted");
  }
  SecEdgarQ4Entry e;
  e.adsh_id = adsh_id;
  e.tv_id = tv_id;
  e.group = GroupFor(row.sic, tl);
  e.mult = row.mult;
  e.nc = row.nc;
  e.ns_hi = row.ns;
  e.ns_lo = row.ns_lo;
  if (e.nc == 0) {
    e.ns_hi = e.ns_lo = 0.0;
  }
  entry_index.Upsert(ekey, static_cast<uint32_t>(entries.size()));
  entries.push_back(e);
  Contribute(e, +1);
}

void Flat::RemoveEntry(uint32_t idx) {
  auto &e = entries[idx];
  if (e.mult == 0) {
    return;
  }
  Contribute(e, -1);
  entry_index.Erase(static_cast<int64_t>(EntryKey(e.adsh_id, e.tv_id)));
  e.mult = 0;
  e.nc = 0;
  e.ns_hi = e.ns_lo = 0.0;
}

void StateToFlat(const SecEdgarQ4State &state, Flat &flat) {
  flat = Flat();
  flat.cohort = *state.cohort;
  for (uint32_t i = 0; i < flat.cohort.ciks.size(); ++i) {
    flat.cik_index.Upsert(flat.cohort.ciks[i], i);
  }
  const auto &bridge = *state.bridge;
  flat.tv = *bridge.tv_dict;
  flat.tv_tl = *bridge.tv_tl;
  flat.tlabels = state.groups->labels->tlabels;
  flat.entries.reserve(bridge.entry_count);
  flat.entry_index.Reserve(bridge.entry_count);
  for (uint32_t id = 0; id < bridge.entry_count; ++id) {
    const auto &e = bridge.At(id);
    flat.entry_index.Upsert(static_cast<int64_t>(EntryKey(e.adsh_id, e.tv_id)), id);
    flat.entries.push_back(e);
  }
  const auto &groups = *state.groups;
  flat.groups.resize(groups.group_count);
  for (uint32_t g = 0; g < groups.group_count; ++g) {
    const auto &sh = *groups.gshards[g / kQ4GroupShardSize];
    const uint32_t i = g % kQ4GroupShardSize;
    auto &out = flat.groups[g];
    out.sic = sh.sic[i];
    out.tl = sh.tl[i];
    out.hi = sh.sum_hi[i];
    out.lo = sh.sum_lo[i];
    out.count = sh.count[i];
    out.distinct = sh.distinct[i];
    flat.group_index.Upsert(
        static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(out.sic)) << 32) |
                             out.tl),
        g);
  }
  groups.pairs.ForEach([&](int64_t key, const int64_t &count) {
    if (count != 0) flat.pairs.Upsert(key, count);
  });
}

SecEdgarQ4State FlatToState(Flat &flat, uint64_t generation) {
  SecEdgarQ4State state;
  // Compact away dead entries; ids are reassigned.
  std::vector<uint32_t> keep;
  keep.reserve(flat.entries.size());
  for (uint32_t i = 0; i < flat.entries.size(); ++i) {
    if (flat.entries[i].mult != 0) keep.push_back(i);
  }
  auto bridge = std::make_shared<SecEdgarQ4Bridge>();
  bridge->tv_dict = std::make_shared<const SecEdgarQ4StrDict>(std::move(flat.tv));
  bridge->tv_tl = std::make_shared<const std::vector<uint32_t>>(std::move(flat.tv_tl));
  bridge->entry_count = static_cast<uint32_t>(keep.size());
  bridge->generation = generation;
  for (uint32_t k = 0; k < keep.size(); ++k) {
    bridge->entries.Mutable(k) = flat.entries[keep[k]];
  }
  size_t cap = 16;
  while (cap < keep.size() * 2 + 2) cap <<= 1;
  auto index = std::make_shared<std::vector<uint32_t>>(cap, kQ4None);
  for (uint32_t id = 0; id < keep.size(); ++id) {
    const auto &e = bridge->At(id);
    size_t i = static_cast<size_t>(SecEdgarQ4Bridge::KeyHash(e.adsh_id, e.tv_id)) & (cap - 1);
    while ((*index)[i] != kQ4None) i = (i + 1) & (cap - 1);
    (*index)[i] = id;
  }
  bridge->index = std::move(index);

  auto labels = std::make_shared<SecEdgarQ4Labels>();
  labels->tlabels = std::move(flat.tlabels);
  auto cohort = std::make_shared<SecEdgarQ4Cohort>(std::move(flat.cohort));

  auto groups = std::make_shared<SecEdgarQ4Groups>();
  groups->labels = labels;
  groups->generation = generation;
  groups->group_count = static_cast<uint32_t>(flat.groups.size());
  const size_t gshards = (flat.groups.size() + kQ4GroupShardSize - 1) / kQ4GroupShardSize;
  for (size_t s = 0; s < gshards; ++s) {
    auto sh = std::make_shared<SecEdgarQ4GroupShard>();
    for (size_t i = 0; i < kQ4GroupShardSize; ++i) {
      const size_t g = s * kQ4GroupShardSize + i;
      if (g >= flat.groups.size()) break;
      const auto &in = flat.groups[g];
      sh->sic[i] = in.sic;
      sh->tl[i] = in.tl;
      sh->sum_hi[i] = in.hi;
      sh->sum_lo[i] = in.lo;
      sh->count[i] = in.count;
      sh->distinct[i] = in.distinct;
    }
    groups->gshards.push_back(std::move(sh));
  }
  for (auto kv : flat.pairs) {
    if (kv.second > 0) groups->pairs.Upsert(kv.first, kv.second);
  }

  state.cohort = std::move(cohort);
  state.bridge = std::move(bridge);
  state.labels = std::move(labels);
  state.groups = std::move(groups);
  return state;
}

// ---------------------------------------------------------------- CowApplier

CowApplier::CowApplier(const SecEdgarQ4State &base)
    : base_(base), cohort(*base_.cohort), bridge(*base_.bridge),
      groups(*base_.groups), entries(bridge.entries), pairs(groups.pairs),
      new_g(groups.gshards.size()) {}

bool CowApplier::Apply(std::string_view adsh, std::string_view tag,
                       std::string_view version, double value, int sign) {
  const uint32_t adsh_id = cohort.adsh.Find(adsh);
  if (adsh_id == kQ4None) {
    return true;
  }
  scratch.assign(tag.data(), tag.size());
  scratch.push_back('\0');
  scratch.append(version.data(), version.size());
  const uint32_t tv_id = bridge.tv_dict->Find(scratch);
  if (tv_id == kQ4None) {
    return true;
  }
  const uint32_t eid = bridge.Lookup(adsh_id, tv_id);
  if (eid == kQ4None) {
    return true;
  }
  if (!std::isfinite(value)) {
    return false;
  }
  auto &e = entries.Mutable(eid);
  if (!std::isfinite(e.ns_hi)) {
    return false;
  }
  const int64_t nc = e.nc + sign;
  if (nc < 0) {
    return false;
  }
  Q4DDAdd(e.ns_hi, e.ns_lo, sign * value);
  e.nc = nc;
  if (nc == 0) {
    e.ns_hi = e.ns_lo = 0.0;
  }

  const uint32_t g = e.group;
  const uint32_t gs = g / kQ4GroupShardSize;
  const uint32_t gi = g % kQ4GroupShardSize;
  auto &gshard = new_g[gs];
  if (!gshard) {
    gshard = std::make_shared<SecEdgarQ4GroupShard>(*groups.gshards[gs]);
  }
  const double m = static_cast<double>(e.mult) * sign;
  Q4DDAddScaled(gshard->sum_hi[gi], gshard->sum_lo[gi], value, m);
  const int64_t rows = static_cast<int64_t>(e.mult) * sign;
  gshard->count[gi] += rows;
  if (gshard->count[gi] == 0) {
    gshard->sum_hi[gi] = gshard->sum_lo[gi] = 0.0;
  }
  const uint32_t cik = cohort.cik_id[adsh_id];
  if (cik != kQ4None) {
    int64_t &slot = pairs[Q4PairKey(g, cik)];
    const int64_t before = slot;
    const int64_t after = before + rows;
    if (after < 0) {
      return false;
    }
    slot = after;
    if (before == 0 && after > 0) {
      ++gshard->distinct[gi];
    } else if (before > 0 && after == 0) {
      --gshard->distinct[gi];
    }
  }
  return true;
}

SecEdgarQ4State CowApplier::Finish() {
  auto new_bridge = std::make_shared<SecEdgarQ4Bridge>(bridge);
  new_bridge->entries = std::move(entries);
  new_bridge->generation = bridge.generation + 1;
  auto new_groups = std::make_shared<SecEdgarQ4Groups>(groups);
  for (size_t i = 0; i < new_g.size(); ++i) {
    if (new_g[i]) new_groups->gshards[i] = std::move(new_g[i]);
  }
  new_groups->pairs = std::move(pairs);
  new_groups->generation = groups.generation + 1;
  SecEdgarQ4State out = base_;
  out.bridge = std::move(new_bridge);
  out.groups = std::move(new_groups);
  return out;
}

// ---------------------------------------------------------------- Top-K

namespace {

#ifdef Q4_HAVE_X86_SIMD
__attribute__((target("avx2"))) void CollectAvx2(const SecEdgarQ4Groups &groups,
                                                 std::vector<uint32_t> &out) {
  const __m256i one = _mm256_set1_epi32(1);
  for (size_t s = 0; s < groups.gshards.size(); ++s) {
    const auto *d = groups.gshards[s]->distinct;
    const uint32_t base = static_cast<uint32_t>(s * kQ4GroupShardSize);
    for (uint32_t i = 0; i < kQ4GroupShardSize; i += 8) {
      const __m256i v = _mm256_load_si256(reinterpret_cast<const __m256i *>(d + i));
      const __m256i m = _mm256_cmpgt_epi32(v, one);
      uint32_t mask = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(m)));
      while (mask) {
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
        out.push_back(base + i + bit);
        mask &= mask - 1;
      }
    }
  }
}

bool HaveAvx2() {
  static const bool have = __builtin_cpu_supports("avx2");
  return have;
}
#endif

void CollectScalar(const SecEdgarQ4Groups &groups, std::vector<uint32_t> &out) {
  for (size_t s = 0; s < groups.gshards.size(); ++s) {
    const auto *d = groups.gshards[s]->distinct;
    const uint32_t base = static_cast<uint32_t>(s * kQ4GroupShardSize);
    for (uint32_t i = 0; i < kQ4GroupShardSize; ++i) {
      if (d[i] >= static_cast<uint32_t>(kMinDistinctCik)) {
        out.push_back(base + i);
      }
    }
  }
}

} // namespace

void CollectCandidates(const SecEdgarQ4Groups &groups, std::vector<uint32_t> &out,
                       bool allow_simd) {
#ifdef Q4_HAVE_X86_SIMD
  if (allow_simd && HaveAvx2()) {
    CollectAvx2(groups, out);
    return;
  }
#endif
  (void)allow_simd;
  CollectScalar(groups, out);
}

std::shared_ptr<const SecEdgarQ4Result>
ComputeResult(const SecEdgarQ4Groups &groups) {
  struct Cand {
    double sum;
    uint32_t gid;
  };
  std::vector<uint32_t> ids;
  ids.reserve(groups.group_count);
  CollectCandidates(groups, ids);

  const auto &tls = groups.labels->tlabels;
  std::vector<Cand> cands;
  cands.reserve(ids.size());
  for (const uint32_t g : ids) {
    const auto &sh = *groups.gshards[g / kQ4GroupShardSize];
    const uint32_t i = g % kQ4GroupShardSize;
    if (sh.count[i] <= 0) continue;
    cands.push_back({sh.sum_hi[i] + sh.sum_lo[i], g});
  }
  const auto better = [&](const Cand &a, const Cand &b) {
    const bool an = std::isnan(a.sum), bn = std::isnan(b.sum);
    if (an != bn) return an; // NaN sorts as the largest value (DuckDB)
    if (!an && a.sum != b.sum) return a.sum > b.sum;
    const auto &sa = *groups.gshards[a.gid / kQ4GroupShardSize];
    const auto &sb = *groups.gshards[b.gid / kQ4GroupShardSize];
    const uint32_t ia = a.gid % kQ4GroupShardSize, ib = b.gid % kQ4GroupShardSize;
    if (sa.sic[ia] != sb.sic[ib]) return sa.sic[ia] < sb.sic[ib];
    const uint32_t ta = sa.tl[ia], tb = sb.tl[ib];
    if (ta == tb) return false;
    if (ta == kQ4None) return true;
    if (tb == kQ4None) return false;
    return tls.Get(ta) < tls.Get(tb);
  };
  if (cands.size() > kOutputLimit) {
    std::nth_element(cands.begin(), cands.begin() + kOutputLimit, cands.end(), better);
    cands.resize(kOutputLimit);
  }
  std::sort(cands.begin(), cands.end(), better);

  auto result = std::make_shared<SecEdgarQ4Result>();
  result->rows.reserve(cands.size());
  result->aux_rows.reserve(cands.size());
  for (const auto &c : cands) {
    const auto &sh = *groups.gshards[c.gid / kQ4GroupShardSize];
    const uint32_t i = c.gid % kQ4GroupShardSize;
    SecEdgarQ4ResultRow row;
    row.sic = sh.sic[i];
    row.tl = sh.tl[i];
    row.distinct = sh.distinct[i];
    row.total = c.sum;
    row.avg = c.sum / static_cast<double>(sh.count[i]);
    result->rows.push_back(row);

    core::AuxRow out;
    out.values.reserve(6);
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(row.sic)));
    if (row.tl == kQ4None) {
      out.values.push_back(core::AuxValue::Null(core::AuxType::String));
    } else {
      out.values.push_back(core::AuxValue::Of(core::AuxType::String, std::string(tls.Get(row.tl))));
    }
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, std::string("EQ")));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.distinct));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Double, row.total));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Double, row.avg));
    result->aux_rows.push_back(std::move(out));
  }
  return result;
}

} // namespace accelerator_codegen::aux_state::query::q4
