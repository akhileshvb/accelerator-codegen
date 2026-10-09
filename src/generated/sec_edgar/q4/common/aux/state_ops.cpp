#include "sec_edgar/q4/common/aux/state_ops.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define Q4_HAVE_X86_SIMD 1
#endif

namespace accelerator_codegen::aux_state::query::q4 {
namespace {

constexpr int32_t kMinDistinctCik = 2;
constexpr size_t kOutputLimit = 500;
constexpr size_t kCacheEntries = 64;
constexpr uint32_t kUnsetTl = 0xFFFFFFFEu;

inline uint32_t KeyAdsh(int64_t key) { return static_cast<uint32_t>(static_cast<uint64_t>(key) >> 42); }
inline uint32_t KeyTv(int64_t key) {
  return static_cast<uint32_t>((static_cast<uint64_t>(key) >> 18) & 0xFFFFFFu);
}
inline uint32_t KeyStmt(int64_t key) {
  return static_cast<uint32_t>((static_cast<uint64_t>(key) >> 10) & 0xFFu);
}

} // namespace

// ---------------------------------------------------------------- Writer

Writer::Writer(const SecEdgarQ4State *base) {
  if (base && base->cohort && base->bridge && base->groups && base->labels) {
    cohort_ = base->cohort;
    tv_dict_ = base->bridge->tv_dict;
    tv_tl_ = base->bridge->tv_tl;
    sud_ = base->bridge->sud;
    labels_ = base->labels;
    base_groups_ = base->groups;
    pre_ = base->bridge->pre;
    num_ = base->bridge->num;
    pairs_ = base->groups->pairs;
    gindex_ = base->groups->gindex;
    shards_ = base->groups->gshards;
    group_count_ = base->groups->group_count;
    generation_ = base->bridge->generation;
  } else {
    cohort_ = std::make_shared<const SecEdgarQ4Cohort>();
    tv_dict_ = std::make_shared<const SecEdgarQ4StrDict>();
    tv_tl_ = std::make_shared<const std::vector<uint32_t>>();
    sud_ = std::make_shared<const SecEdgarQ4Sud>();
    labels_ = std::make_shared<const SecEdgarQ4Labels>();
  }
  own_.resize(shards_.size());
}

SecEdgarQ4Cohort &Writer::MutCohort() {
  if (!mut_cohort_) mut_cohort_ = std::make_shared<SecEdgarQ4Cohort>(*cohort_);
  return *mut_cohort_;
}
SecEdgarQ4StrDict &Writer::MutTvDict() {
  if (!mut_tv_dict_) mut_tv_dict_ = std::make_shared<SecEdgarQ4StrDict>(*tv_dict_);
  return *mut_tv_dict_;
}
std::vector<uint32_t> &Writer::MutTvTl() {
  if (!mut_tv_tl_) mut_tv_tl_ = std::make_shared<std::vector<uint32_t>>(*tv_tl_);
  return *mut_tv_tl_;
}
SecEdgarQ4StrDict &Writer::MutTlabels() {
  if (!mut_labels_) mut_labels_ = std::make_shared<SecEdgarQ4Labels>(*labels_);
  return mut_labels_->tlabels;
}
SecEdgarQ4Sud &Writer::MutSud() {
  if (!mut_sud_) mut_sud_ = std::make_shared<SecEdgarQ4Sud>(*sud_);
  return *mut_sud_;
}

Writer::Shard &Writer::MutShard(uint32_t gid) {
  const size_t idx = gid / kQ4GroupShardSize;
  if (idx >= shards_.size()) {
    shards_.resize(idx + 1);
    own_.resize(idx + 1);
  }
  auto &slot = own_[idx];
  if (!slot) {
    if (shards_[idx]) {
      slot = std::make_shared<Shard>(*shards_[idx]);
    } else {
      slot = std::make_shared<Shard>();
      for (uint32_t i = 0; i < kQ4GroupShardSize; ++i) {
        slot->su[i] = 0xFFFFFFFFu;
        slot->tl[i] = kQ4None;
      }
    }
  }
  return *slot;
}

const Writer::Shard &Writer::ConstShard(uint32_t gid) const {
  const size_t idx = gid / kQ4GroupShardSize;
  return own_[idx] ? *own_[idx] : *shards_[idx];
}

uint32_t Writer::TvFind(std::string_view tag, std::string_view version) {
  scratch_.assign(tag.data(), tag.size());
  scratch_.push_back('\0');
  scratch_.append(version.data(), version.size());
  return (mut_tv_dict_ ? *mut_tv_dict_ : *tv_dict_).Find(scratch_);
}

uint32_t Writer::GroupFor(uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom) {
  const int64_t key = Q4GroupKey(sic_id, tl, stmt, uom);
  if (const uint32_t *found = gindex_.Find(key)) {
    return *found;
  }
  if (group_count_ >= kQ4None - 1) {
    throw std::length_error("sec_edgar q4: group id space exhausted");
  }
  const uint32_t gid = group_count_++;
  gindex_.Upsert(key, gid);
  auto &sh = MutShard(gid);
  const uint32_t i = gid % kQ4GroupShardSize;
  sh.sic[i] = Coh().sics[sic_id];
  sh.tl[i] = tl;
  sh.su[i] = (stmt << 16) | uom;
  sh.distinct[i] = 0;
  sh.count[i] = 0;
  sh.sum_hi[i] = sh.sum_lo[i] = 0.0;
  return gid;
}

bool Writer::Contribute(uint32_t gid, uint32_t cik_id, double hi, double lo, double mult,
                        int64_t rows, bool strict) {
  auto &sh = MutShard(gid);
  const uint32_t i = gid % kQ4GroupShardSize;
  if (strict && (!std::isfinite(sh.sum_hi[i]) || !std::isfinite(hi) || !std::isfinite(lo))) {
    return false; // an inf/NaN contribution cannot be subtracted back out
  }
  Q4DDAddScaled(sh.sum_hi[i], sh.sum_lo[i], hi, mult);
  if (lo != 0.0) Q4DDAddScaled(sh.sum_hi[i], sh.sum_lo[i], lo, mult);
  sh.count[i] += rows;
  if (sh.count[i] == 0) {
    sh.sum_hi[i] = sh.sum_lo[i] = 0.0;
  }
  if (cik_id != kQ4None && rows != 0) {
    const int64_t pkey = Q4PairKey(gid, cik_id);
    const int64_t *cur = pairs_.Find(pkey);
    const int64_t before = cur ? *cur : 0;
    const int64_t after = before + rows;
    if (after < 0) {
      return false;
    }
    if (after == 0) {
      pairs_.Erase(pkey);
    } else {
      pairs_.Upsert(pkey, after);
    }
    if (before == 0 && after > 0) {
      ++sh.distinct[i];
    } else if (before > 0 && after == 0) {
      --sh.distinct[i];
    }
  }
  return true;
}

bool Writer::ApplyNum(std::string_view adsh, std::string_view tag, std::string_view version,
                      std::string_view uom, double value, int sign) {
  const auto &coh = Coh();
  const uint32_t a = coh.adsh.Find(adsh);
  if (a == kQ4None) {
    return true;
  }
  const uint32_t tv = TvFind(tag, version);
  if (tv == kQ4None) {
    return true;
  }
  const auto &sud0 = mut_sud_ ? *mut_sud_ : *sud_;
  uint32_t sids[kQ4MaxStmt];
  uint32_t mults[kQ4MaxStmt];
  uint32_t n = 0;
  const uint32_t S = std::min<uint32_t>(sud0.stmts.Size(), kQ4MaxStmt);
  for (uint32_t s = 0; s < S; ++s) {
    if (const uint32_t *m = pre_.Find(Q4RecKey(a, tv, s, 0))) {
      sids[n] = s;
      mults[n] = *m;
      ++n;
    }
  }
  if (n == 0) {
    return true; // key is not in any (stmt) group
  }
  if (!std::isfinite(value)) {
    return false;
  }
  uint32_t u = sud0.uoms.Find(uom);
  if (u == kQ4None) {
    if (sign < 0 || sud0.uoms.Size() >= kQ4MaxUom) {
      return false;
    }
    u = MutSud().uoms.Insert(uom);
  }
  const uint32_t sic_id = coh.sic_id[a];
  const uint32_t cik = coh.cik_id[a];
  const uint32_t tl = (*(mut_tv_tl_ ? mut_tv_tl_ : tv_tl_))[tv];
  if (sic_id == kQ4None) {
    return false;
  }
  for (uint32_t k = 0; k < n; ++k) {
    const int64_t key = Q4RecKey(a, tv, sids[k], u);
    auto &rec = num_[key];
    uint32_t gid;
    if (rec.nc == 0) {
      if (sign < 0) {
        num_.Erase(key);
        return false;
      }
      gid = GroupFor(sic_id, tl, sids[k], u);
      rec = SecEdgarQ4NumRec{};
      rec.group = gid;
    } else {
      gid = rec.group;
      if (!std::isfinite(rec.hi)) {
        return false;
      }
    }
    const int64_t nc = static_cast<int64_t>(rec.nc) + sign;
    if (nc < 0) {
      return false;
    }
    Q4DDAdd(rec.hi, rec.lo, sign * value);
    rec.nc = static_cast<uint32_t>(nc);
    if (nc == 0) {
      num_.Erase(key);
    }
    if (!Contribute(gid, cik, value, 0.0, static_cast<double>(mults[k]) * sign,
                    static_cast<int64_t>(mults[k]) * sign, true)) {
      return false;
    }
  }
  return true;
}

bool Writer::RemoveDirty(const std::vector<std::string> &adsh,
                         const std::vector<std::pair<std::string, std::string>> &tag_versions,
                         const std::vector<SecEdgarQ4BridgeFilter::Key> &keys) {
  const auto &coh = Coh();
  const auto &tvd = mut_tv_dict_ ? *mut_tv_dict_ : *tv_dict_;
  std::vector<char> amark(coh.adsh.Size(), 0), tmark(tvd.Size(), 0), tkmark(tvd.Size(), 0);
  std::unordered_set<uint64_t> kset;
  bool any = false;
  for (const auto &a : adsh) {
    const uint32_t id = coh.adsh.Find(a);
    if (id != kQ4None) amark[id] = any = true;
  }
  for (const auto &t : tag_versions) {
    const uint32_t id = TvFind(t.first, t.second);
    if (id != kQ4None) tmark[id] = any = true;
  }
  for (const auto &k : keys) {
    const uint32_t a = coh.adsh.Find(k.adsh);
    const uint32_t t = TvFind(k.tag, k.version);
    if (a == kQ4None || t == kQ4None) continue;
    kset.insert((static_cast<uint64_t>(a) << 32) | t);
    tkmark[t] = any = true;
  }
  if (!any) {
    return true;
  }
  const auto hit = [&](uint32_t a, uint32_t t) {
    return amark[a] || tmark[t] ||
           (tkmark[t] && kset.count((static_cast<uint64_t>(a) << 32) | t) != 0);
  };
  std::vector<int64_t> num_victims, pre_victims;
  num_.ForEach([&](int64_t key, const SecEdgarQ4NumRec &) {
    if (hit(KeyAdsh(key), KeyTv(key))) num_victims.push_back(key);
  });
  pre_.ForEach([&](int64_t key, const uint32_t &) {
    if (hit(KeyAdsh(key), KeyTv(key))) pre_victims.push_back(key);
  });
  for (const int64_t key : num_victims) {
    const SecEdgarQ4NumRec rec = *num_.Find(key);
    const uint32_t a = KeyAdsh(key);
    const uint32_t *m = pre_.Find(Q4RecKey(a, KeyTv(key), KeyStmt(key), 0));
    if (!m) {
      bad_ = true;
      return false;
    }
    if (!Contribute(rec.group, coh.cik_id[a], rec.hi, rec.lo, -static_cast<double>(*m),
                    -static_cast<int64_t>(*m) * rec.nc, true)) {
      bad_ = true;
      return false;
    }
    num_.Erase(key);
  }
  for (const int64_t key : pre_victims) {
    pre_.Erase(key);
  }
  return true;
}

bool Writer::AddRow(const SecEdgarQ4BridgeRow &row) {
  if (row.mult == 0) {
    return true;
  }
  auto &coh = MutCohort();
  const uint32_t a = coh.adsh.Insert(row.adsh);
  if (a >= kQ4MaxAdsh) {
    bad_ = true;
    return false;
  }
  if (coh.cik_id.size() <= a) {
    coh.cik_id.resize(a + 1, kQ4None);
    coh.sic_id.resize(a + 1, kQ4None);
  }
  uint32_t cik = kQ4None;
  if (!row.cik_null) {
    if (const uint32_t *f = coh.cik_index.Find(row.cik)) {
      cik = *f;
    } else {
      cik = static_cast<uint32_t>(coh.ciks.size());
      coh.ciks.push_back(row.cik);
      coh.cik_index.Upsert(row.cik, cik);
    }
  }
  uint32_t sic_id;
  if (const uint32_t *f = coh.sic_index.Find(row.sic)) {
    sic_id = *f;
  } else {
    sic_id = static_cast<uint32_t>(coh.sics.size());
    if (sic_id >= kQ4MaxSic) {
      bad_ = true;
      return false;
    }
    coh.sics.push_back(row.sic);
    coh.sic_index.Upsert(row.sic, sic_id);
  }
  coh.cik_id[a] = cik;
  coh.sic_id[a] = sic_id;

  // tag/version and tlabel
  scratch_.assign(row.tag);
  scratch_.push_back('\0');
  scratch_.append(row.version);
  uint32_t tv = (mut_tv_dict_ ? *mut_tv_dict_ : *tv_dict_).Find(scratch_);
  if (tv == kQ4None) {
    tv = MutTvDict().Insert(scratch_);
    if (tv >= kQ4MaxTv) {
      bad_ = true;
      return false;
    }
  }
  uint32_t tl = kQ4None;
  if (!row.tlabel_null) {
    const auto &tls = mut_labels_ ? mut_labels_->tlabels : labels_->tlabels;
    tl = tls.Find(row.tlabel);
    if (tl == kQ4None) {
      tl = MutTlabels().Insert(row.tlabel);
      if (tl >= kQ4MaxTl) {
        bad_ = true;
        return false;
      }
    }
  }
  {
    const auto &cur = mut_tv_tl_ ? *mut_tv_tl_ : *tv_tl_;
    if (cur.size() <= tv || cur[tv] != tl) {
      auto &w = MutTvTl();
      if (w.size() <= tv) w.resize(tv + 1, kUnsetTl);
      w[tv] = tl;
    }
  }
  const auto &sud0 = mut_sud_ ? *mut_sud_ : *sud_;
  uint32_t s = sud0.stmts.Find(row.stmt);
  if (s == kQ4None) {
    s = MutSud().stmts.Insert(row.stmt);
    if (s >= kQ4MaxStmt) {
      bad_ = true;
      return false;
    }
  }
  pre_[Q4RecKey(a, tv, s, 0)] = row.mult;
  if (!row.has_num || row.nc <= 0) {
    return true;
  }
  const auto &sud1 = mut_sud_ ? *mut_sud_ : *sud_;
  uint32_t u = sud1.uoms.Find(row.uom);
  if (u == kQ4None) {
    u = MutSud().uoms.Insert(row.uom);
    if (u >= kQ4MaxUom) {
      bad_ = true;
      return false;
    }
  }
  if (row.nc > static_cast<int64_t>(0xFFFFFFFFu)) {
    bad_ = true;
    return false;
  }
  const int64_t key = Q4RecKey(a, tv, s, u);
  if (num_.Contains(key)) {
    bad_ = true; // duplicate (adsh, tag, version, stmt, uom) in the scan
    return false;
  }
  const uint32_t gid = GroupFor(sic_id, tl, s, u);
  SecEdgarQ4NumRec rec;
  rec.nc = static_cast<uint32_t>(row.nc);
  rec.hi = row.ns;
  rec.lo = row.ns_lo;
  rec.group = gid;
  num_.Upsert(key, rec);
  return Contribute(gid, cik, row.ns, row.ns_lo, static_cast<double>(row.mult),
                    static_cast<int64_t>(row.mult) * row.nc, false);
}

SecEdgarQ4State Writer::Finish() {
  SecEdgarQ4State out;
  out.cohort = mut_cohort_ ? std::shared_ptr<const SecEdgarQ4Cohort>(std::move(mut_cohort_))
                           : cohort_;
  out.labels = mut_labels_ ? std::shared_ptr<const SecEdgarQ4Labels>(std::move(mut_labels_))
                           : labels_;
  auto bridge = std::make_shared<SecEdgarQ4Bridge>();
  bridge->tv_dict = mut_tv_dict_ ? std::shared_ptr<const SecEdgarQ4StrDict>(std::move(mut_tv_dict_))
                                 : tv_dict_;
  bridge->tv_tl = mut_tv_tl_
                      ? std::shared_ptr<const std::vector<uint32_t>>(std::move(mut_tv_tl_))
                      : tv_tl_;
  bridge->sud = mut_sud_ ? std::shared_ptr<const SecEdgarQ4Sud>(std::move(mut_sud_)) : sud_;
  bridge->pre = std::move(pre_);
  bridge->num = std::move(num_);
  bridge->generation = generation_ + 1;

  auto groups = std::make_shared<SecEdgarQ4Groups>();
  groups->labels = out.labels;
  groups->sud = bridge->sud;
  for (size_t i = 0; i < own_.size(); ++i) {
    if (own_[i]) shards_[i] = std::move(own_[i]);
  }
  groups->gshards = std::move(shards_);
  groups->group_count = group_count_;
  groups->gindex = std::move(gindex_);
  groups->pairs = std::move(pairs_);
  groups->generation = generation_ + 1;
  out.bridge = std::move(bridge);
  out.groups = std::move(groups);
  return out;
}

// ---------------------------------------------------------------- Top-K

namespace {

#ifdef Q4_HAVE_X86_SIMD
__attribute__((target("avx2"))) void CollectAvx2(const SecEdgarQ4Groups &groups, int32_t lo,
                                                 int32_t hi, uint32_t su,
                                                 std::vector<uint32_t> &out) {
  const __m256i one = _mm256_set1_epi32(1);
  const __m256i vlo = _mm256_set1_epi32(lo);
  const __m256i vhi = _mm256_set1_epi32(hi);
  const __m256i vsu = _mm256_set1_epi32(static_cast<int32_t>(su));
  for (size_t s = 0; s < groups.gshards.size(); ++s) {
    const auto &sh = *groups.gshards[s];
    const uint32_t base = static_cast<uint32_t>(s * kQ4GroupShardSize);
    for (uint32_t i = 0; i < kQ4GroupShardSize; i += 8) {
      const __m256i d = _mm256_load_si256(reinterpret_cast<const __m256i *>(sh.distinct + i));
      const __m256i c = _mm256_load_si256(reinterpret_cast<const __m256i *>(sh.sic + i));
      const __m256i u = _mm256_load_si256(reinterpret_cast<const __m256i *>(sh.su + i));
      __m256i m = _mm256_and_si256(_mm256_cmpgt_epi32(d, one), _mm256_cmpeq_epi32(u, vsu));
      m = _mm256_andnot_si256(_mm256_cmpgt_epi32(vlo, c), m);
      m = _mm256_andnot_si256(_mm256_cmpgt_epi32(c, vhi), m);
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

void CollectScalar(const SecEdgarQ4Groups &groups, int32_t lo, int32_t hi, uint32_t su,
                   std::vector<uint32_t> &out) {
  for (size_t s = 0; s < groups.gshards.size(); ++s) {
    const auto &sh = *groups.gshards[s];
    const uint32_t base = static_cast<uint32_t>(s * kQ4GroupShardSize);
    for (uint32_t i = 0; i < kQ4GroupShardSize; ++i) {
      if (sh.distinct[i] >= static_cast<uint32_t>(kMinDistinctCik) && sh.su[i] == su &&
          sh.sic[i] >= lo && sh.sic[i] <= hi) {
        out.push_back(base + i);
      }
    }
  }
}

} // namespace

void CollectCandidates(const SecEdgarQ4Groups &groups, int32_t lo, int32_t hi, uint32_t su,
                       std::vector<uint32_t> &out, bool allow_simd) {
#ifdef Q4_HAVE_X86_SIMD
  if (allow_simd && HaveAvx2()) {
    CollectAvx2(groups, lo, hi, su, out);
    return;
  }
#endif
  (void)allow_simd;
  CollectScalar(groups, lo, hi, su, out);
}

std::shared_ptr<const SecEdgarQ4Result>
ComputeResult(const SecEdgarQ4Groups &groups, const SecEdgarQ4Params &params) {
  struct Cand {
    double sum;
    uint32_t gid;
  };
  auto result = std::make_shared<SecEdgarQ4Result>();
  if (params.empty || params.sic_lo > params.sic_hi || !groups.sud || !groups.labels) {
    return result;
  }
  const uint32_t stmt = groups.sud->stmts.Find(params.stmt);
  const uint32_t uom = groups.sud->uoms.Find(params.uom);
  if (stmt == kQ4None || uom == kQ4None) {
    return result;
  }
  std::vector<uint32_t> ids;
  CollectCandidates(groups, params.sic_lo, params.sic_hi, (stmt << 16) | uom, ids);

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
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, params.stmt));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.distinct));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Double, row.total));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Double, row.avg));
    result->aux_rows.push_back(std::move(out));
  }
  return result;
}

std::shared_ptr<const SecEdgarQ4Result>
CachedResult(const SecEdgarQ4Groups &groups, const SecEdgarQ4Params &params) {
  if (params.empty || params.sic_lo > params.sic_hi) {
    return std::make_shared<SecEdgarQ4Result>();
  }
  const SecEdgarQ4Groups::CacheKey key{params.sic_lo, params.sic_hi, params.stmt, params.uom};
  {
    std::lock_guard<std::mutex> lock(groups.cache_mutex);
    for (const auto &entry : groups.cache) {
      if (entry.first == key) return entry.second;
    }
  }
  auto result = ComputeResult(groups, params);
  std::lock_guard<std::mutex> lock(groups.cache_mutex);
  for (const auto &entry : groups.cache) {
    if (entry.first == key) return entry.second;
  }
  if (groups.cache.size() < kCacheEntries) {
    groups.cache.emplace_back(key, result);
  } else {
    groups.cache[groups.cache_next++ % kCacheEntries] = {key, result};
  }
  return result;
}

} // namespace accelerator_codegen::aux_state::query::q4
