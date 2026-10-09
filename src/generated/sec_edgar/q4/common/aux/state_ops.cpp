#include "sec_edgar/q4/common/aux/state_ops.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <sys/mman.h>

namespace accelerator_codegen::aux_state::query::q4 {
namespace {

constexpr int32_t kMinDistinctCik = 2;
constexpr size_t kOutputLimit = 500;
constexpr size_t kCacheEntries = 64;

std::string Join3(std::string_view a, std::string_view b, std::string_view c) {
  std::string out;
  out.reserve(a.size() + b.size() + c.size() + 2);
  out.append(a);
  out.push_back('\0');
  out.append(b);
  out.push_back('\0');
  out.append(c);
  return out;
}

std::string Join2(std::string_view a, std::string_view b) {
  std::string out;
  out.reserve(a.size() + b.size() + 1);
  out.append(a);
  out.push_back('\0');
  out.append(b);
  return out;
}

// Decodes a pre value into (stmt, mult) arrays (at most 64).
uint32_t DecodeStmts(int64_t pk, uint64_t v, const SecEdgarQ4PreOverflowMap &over, uint32_t *s,
                     uint32_t *m, bool &ok) {
  uint32_t n = 0;
  ok = Q4ForEachStmt(pk, v, over, [&](uint32_t stmt, uint32_t mult) {
    if (n < 64) {
      s[n] = stmt;
      m[n] = mult;
      ++n;
    }
  });
  return n;
}

} // namespace

// ---------------------------------------------------------------- Writer

Writer::Writer(const SecEdgarQ4State &base)
    : base_(base), bridge_(base.bridge.get()), groups_(base.groups.get()) {
  if (!base_.cohort || !bridge_ || !groups_ || !base_.labels || !bridge_->tdict || !bridge_->tags ||
      !bridge_->pre || !bridge_->pre_over || !groups_->sud || !groups_->su_slot ||
      !groups_->pairs || !groups_->gindex) {
    throw std::logic_error("sec_edgar q4: incomplete state");
  }
  slots_ = groups_->slots;
  own_slot_.assign(slots_.size(), 0);
}

SecEdgarQ4Cohort &Writer::MutCohort() {
  if (!mut_cohort_) mut_cohort_ = std::make_shared<SecEdgarQ4Cohort>(*base_.cohort);
  return *mut_cohort_;
}
SecEdgarQ4TagDicts &Writer::MutTD() {
  if (!mut_tdict_) mut_tdict_ = std::make_shared<SecEdgarQ4TagDicts>(*bridge_->tdict);
  return *mut_tdict_;
}
SecEdgarQ4TagMap &Writer::MutTM() {
  if (!mut_tags_) mut_tags_ = std::make_shared<SecEdgarQ4TagMap>(*bridge_->tags);
  return *mut_tags_;
}
SecEdgarQ4PreMap &Writer::MutPre() {
  if (!mut_pre_) mut_pre_ = std::make_shared<SecEdgarQ4PreMap>(*bridge_->pre);
  return *mut_pre_;
}
SecEdgarQ4PreOverflowMap &Writer::MutPreOver() {
  if (!mut_pre_over_) mut_pre_over_ = std::make_shared<SecEdgarQ4PreOverflowMap>(*bridge_->pre_over);
  return *mut_pre_over_;
}
SecEdgarQ4Sud &Writer::MutSud() {
  if (!mut_sud_) mut_sud_ = std::make_shared<SecEdgarQ4Sud>(*groups_->sud);
  return *mut_sud_;
}
SecEdgarQ4Labels &Writer::MutLabels() {
  if (!mut_labels_) mut_labels_ = std::make_shared<SecEdgarQ4Labels>(*base_.labels);
  return *mut_labels_;
}
SecEdgarQ4GroupIndex &Writer::MutGIndex() {
  if (!mut_gindex_) mut_gindex_ = std::make_shared<SecEdgarQ4GroupIndex>(*groups_->gindex);
  return *mut_gindex_;
}
SecEdgarQ4PairMap &Writer::MutPairs() {
  if (!mut_pairs_) mut_pairs_ = std::make_shared<SecEdgarQ4PairMap>(*groups_->pairs);
  return *mut_pairs_;
}

SecEdgarQ4SuGroups &Writer::MutSu(uint32_t slot) {
  if (!own_slot_[slot]) {
    slots_[slot] = std::make_shared<SecEdgarQ4SuGroups>(*slots_[slot]);
    own_slot_[slot] = 1;
  }
  return *slots_[slot];
}

SecEdgarQ4ShardChunk &Writer::MutChunk(SecEdgarQ4SuGroups &sg, uint32_t shard_index) {
  const uint32_t c = shard_index >> kQ4ChunkBits;
  while (sg.chunks.size() <= c) sg.chunks.push_back(std::make_shared<SecEdgarQ4ShardChunk>());
  auto &chunk = sg.chunks[c];
  // A chunk / shard inherited from the base version is also referenced by it
  // (the writer holds the base alive), so use_count() > 1 until cloned here.
  if (chunk.use_count() > 1) chunk = std::make_shared<SecEdgarQ4ShardChunk>(*chunk);
  return *chunk;
}

Writer::Shard &Writer::MutShard(uint64_t gid) {
  auto &sg = MutSu(Q4GidSlot(gid));
  const uint32_t k = Q4GidLocal(gid) >> kQ4GroupShardBits;
  auto &chunk = MutChunk(sg, k);
  auto &sh = chunk.shard[k & (kQ4ChunkShards - 1)];
  if (!sh) {
    sh = std::make_shared<Shard>();
  } else if (sh.use_count() > 1) {
    sh = std::make_shared<Shard>(*sh);
  }
  return *sh;
}

uint64_t Writer::GroupFor(uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom, bool create) {
  const int64_t key = Q4GroupKey(sic_id, tl, stmt, uom);
  if (const uint64_t *found = GIndex().Find(key)) {
    return *found;
  }
  if (!create) return kNoGroup;
  return NewGroup(key, sic_id, tl, stmt, uom);
}

uint64_t Writer::NewGroup(int64_t key, uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom) {
  const uint32_t su = Q4SuKey(stmt, uom);
  uint32_t slot;
  if (const uint32_t *s = SuSlot().Find(su)) {
    slot = *s;
  } else {
    slot = static_cast<uint32_t>(slots_.size());
    if (slot >= kQ4MaxSlot) throw std::length_error("sec_edgar q4: (stmt, uom) slot space exhausted");
    if (!mut_su_slot_) mut_su_slot_ = std::make_shared<core::FlatHashMap<uint32_t>>(*groups_->su_slot);
    mut_su_slot_->Upsert(su, slot);
    auto sg = std::make_shared<SecEdgarQ4SuGroups>();
    sg->stmt = stmt;
    sg->uom = uom;
    slots_.push_back(std::move(sg));
    own_slot_.push_back(1);
  }
  auto &sg = MutSu(slot);
  const uint32_t local = sg.n;
  if (local >= kQ4MaxLocal) throw std::length_error("sec_edgar q4: group id space exhausted");
  const uint64_t gid = Q4Gid(slot, local);
  sg.n = local + 1;
  auto &sh = MutShard(gid);
  const uint32_t k = local >> kQ4GroupShardBits, i = local & (kQ4GroupShardSize - 1);
  auto &chunk = MutChunk(sg, k);
  const int32_t sic = Coh().sics[sic_id];
  sh.sic[i] = sic;
  sh.tl[i] = tl;
  sh.distinct[i] = 0;
  sh.count[i] = 0;
  sh.sum_hi[i] = sh.sum_lo[i] = 0.0;
  const uint32_t ci = k & (kQ4ChunkShards - 1);
  chunk.min_sic[ci] = std::min(chunk.min_sic[ci], sic);
  chunk.max_sic[ci] = std::max(chunk.max_sic[ci], sic);
  MutGIndex().Upsert(key, gid);
  return gid;
}

bool Writer::Contribute(uint64_t gid, uint32_t cik_id, double hi, double lo, double mult,
                        int64_t rows) {
  auto &sh = MutShard(gid);
  const uint32_t i = Q4GidLocal(gid) & (kQ4GroupShardSize - 1);
  if (!std::isfinite(sh.sum_hi[i]) || !std::isfinite(hi) || !std::isfinite(lo)) {
    return false; // an inf/NaN contribution cannot be subtracted back out
  }
  Q4DDAddScaled(sh.sum_hi[i], sh.sum_lo[i], hi, mult);
  if (lo != 0.0) Q4DDAddScaled(sh.sum_hi[i], sh.sum_lo[i], lo, mult);
  sh.count[i] += rows;
  if (sh.count[i] < 0) return false;
  if (sh.count[i] == 0) sh.sum_hi[i] = sh.sum_lo[i] = 0.0;
  if (cik_id != kQ4None && rows != 0) {
    const int64_t pkey = Q4PairKey(gid, cik_id);
    const int64_t *cur = Pairs().Find(pkey);
    const int64_t before = cur ? *cur : 0;
    const int64_t after = before + rows;
    if (after < 0) return false;
    if (after == 0) {
      MutPairs().Erase(pkey);
    } else {
      MutPairs().Upsert(pkey, after);
    }
    if (before == 0 && after > 0) {
      ++sh.distinct[i];
    } else if (before > 0 && after == 0) {
      --sh.distinct[i];
    }
  }
  return true;
}

bool Writer::ErasePre(int64_t pk) {
  const SecEdgarQ4PreVal *v = Pre().Find(pk);
  if (!v) return true;
  if (v->stmts == kQ4PreOverflow) MutPreOver().Erase(pk);
  MutPre().Erase(pk);
  return true;
}

bool Writer::PutPre(int64_t pk, uint32_t tl, const std::vector<std::pair<uint32_t, uint32_t>> &stmts) {
  uint32_t s[64], m[64];
  if (stmts.size() > 16) return false;
  for (size_t i = 0; i < stmts.size(); ++i) {
    s[i] = stmts[i].first;
    m[i] = stmts[i].second;
  }
  SecEdgarQ4PreVal val;
  val.tl = tl;
  if (Q4PackStmts(s, m, static_cast<uint32_t>(stmts.size()), val.stmts)) {
    MutPre().Upsert(pk, val);
    return true;
  }
  SecEdgarQ4PreOverflow rec;
  rec.n = static_cast<uint32_t>(stmts.size());
  for (uint32_t i = 0; i < rec.n; ++i) {
    rec.stmt[i] = static_cast<uint8_t>(s[i]);
    rec.mult[i] = m[i];
  }
  MutPreOver().Upsert(pk, rec);
  val.stmts = kQ4PreOverflow;
  MutPre().Upsert(pk, val);
  return true;
}

uint32_t Writer::EnsureAdsh(std::string_view adsh) {
  uint32_t a = Coh().adsh.Find(adsh);
  if (a != kQ4None) return a;
  auto &coh = MutCohort();
  a = coh.adsh.Insert(adsh);
  if (a >= kQ4MaxAdsh) return kQ4None;
  if (coh.cik_id.size() <= a) {
    coh.cik_id.resize(a + 1, kQ4None);
    coh.sic_id.resize(a + 1, kQ4None);
  }
  return a;
}

bool Writer::ApplyStructural(const SecEdgarQ4DirtyRows &rows, const SecEdgarQ4TxnBuffer &buffer,
                             std::unordered_set<std::string> &keys_out) {
  struct Acc {
    double nh = 0.0, nl = 0.0; // current SUM(value) (double-double)
    int64_t nnc = 0;           // current COUNT(value)
    double dh = 0.0, dl = 0.0; // this transaction's net delta
    int64_t dnc = 0;
  };
  struct KeyInfo {
    std::string adsh, tag, version;
    std::map<std::string, uint32_t> stmts; // current pre rows per stmt
    std::map<std::string, Acc> uoms;
  };
  std::unordered_map<std::string, KeyInfo> keys;
  const auto touch = [&](const std::string &a, const std::string &t,
                         const std::string &v) -> KeyInfo & {
    auto [it, inserted] = keys.try_emplace(Join3(a, t, v));
    if (inserted) {
      it->second.adsh = a;
      it->second.tag = t;
      it->second.version = v;
    }
    return it->second;
  };
  for (const auto &k : buffer.dirty_keys) touch(k.adsh, k.tag, k.version);
  for (const auto &p : rows.pres) ++touch(p.adsh, p.tag, p.version).stmts[p.stmt];
  for (const auto &n : rows.nums) {
    if (n.value_is_null) continue;
    const auto it = keys.find(Join3(n.adsh, n.tag, n.version));
    if (it == keys.end()) continue;
    auto &acc = it->second.uoms[n.uom];
    Q4DDAdd(acc.nh, acc.nl, n.value);
    ++acc.nnc;
  }
  for (const auto &d : buffer.Deltas()) {
    const auto it = keys.find(Join3(buffer.Adsh(d), buffer.Tag(d), buffer.Version(d)));
    if (it == keys.end()) continue;
    auto &acc = it->second.uoms[std::string(buffer.Uom(d))];
    SecEdgarQ4TxnBuffer::ForEachSigned(d, [&](int sign, double value) {
      Q4DDAdd(acc.dh, acc.dl, sign * value);
      acc.dnc += sign;
    });
  }
  std::unordered_map<std::string, const SecEdgarQ4SubInput *> subs;
  for (const auto &s : rows.subs) subs[s.adsh] = &s;
  std::unordered_map<std::string, const SecEdgarQ4TagInput *> tags;
  for (const auto &t : rows.tags) {
    if (t.abstract_flag == 0) tags[Join2(t.tag, t.version)] = &t;
  }

  uint32_t ss[64], ms[64];
  // 1. remove the old contribution of every affected key
  for (auto &kv : keys) {
    const auto &k = kv.second;
    const auto &coh = Coh();
    const uint32_t a = coh.adsh.Find(k.adsh);
    const uint32_t t = TD().tags.Find(k.tag);
    const uint32_t v = TD().versions.Find(k.version);
    if (a == kQ4None || t == kQ4None || v == kQ4None) continue;
    const int64_t pk = Q4PreKey(a, t, v);
    const SecEdgarQ4PreVal *pv = Pre().Find(pk);
    if (!pv) continue;
    const uint32_t sic_id = coh.sic_id[a];
    const uint32_t cik = coh.cik_id[a];
    const uint32_t tl = pv->tl;
    if (sic_id == kQ4None) return false;
    bool ok;
    const uint32_t ns = DecodeStmts(pk, pv->stmts, PreOver(), ss, ms, ok);
    if (!ok) return false;
    for (const auto &u_acc : k.uoms) {
      const auto &acc = u_acc.second;
      const int64_t old_nc = acc.nnc - acc.dnc;
      if (old_nc < 0) return false;
      if (old_nc == 0) continue;
      const uint32_t u = Sud().uoms.Find(u_acc.first);
      if (u == kQ4None) return false;
      double oh = acc.nh, ol = acc.nl;
      Q4DDAdd(oh, ol, -acc.dh);
      Q4DDAdd(oh, ol, -acc.dl);
      for (uint32_t j = 0; j < ns; ++j) {
        const uint64_t gid = GroupFor(sic_id, tl, ss[j], u, false);
        if (gid == kNoGroup) return false;
        if (!Contribute(gid, cik, oh, ol, -static_cast<double>(ms[j]),
                        -static_cast<int64_t>(ms[j]) * old_nc)) {
          return false;
        }
      }
    }
    if (!ErasePre(pk)) return false;
  }

  const auto set_sub = [&](uint32_t a, const SecEdgarQ4SubInput &s) -> bool {
    auto &coh = MutCohort();
    uint32_t cik = kQ4None;
    if (!s.cik_null) {
      if (const uint32_t *f = coh.cik_index.Find(s.cik)) {
        cik = *f;
      } else {
        cik = static_cast<uint32_t>(coh.ciks.size());
        if (cik >= kQ4MaxCik - 1) return false;
        coh.ciks.push_back(s.cik);
        coh.cik_index.Upsert(s.cik, cik);
      }
    }
    uint32_t sic_id;
    if (const uint32_t *f = coh.sic_index.Find(s.sic)) {
      sic_id = *f;
    } else {
      sic_id = static_cast<uint32_t>(coh.sics.size());
      if (sic_id >= kQ4MaxSic) return false;
      coh.sics.push_back(s.sic);
      coh.sic_index.Upsert(s.sic, sic_id);
    }
    coh.cik_id[a] = cik;
    coh.sic_id[a] = sic_id;
    return true;
  };
  const auto ensure_tag = [&](std::string_view tag, std::string_view ver, uint32_t &t,
                              uint32_t &v) -> bool {
    t = TD().tags.Find(tag);
    if (t == kQ4None) t = MutTD().tags.Insert(tag);
    v = TD().versions.Find(ver);
    if (v == kQ4None) v = MutTD().versions.Insert(ver);
    return t < kQ4MaxTag && v < kQ4MaxVer;
  };
  const auto label_of = [&](const SecEdgarQ4TagInput &tg, uint32_t &tl) -> bool {
    tl = kQ4None;
    if (tg.tlabel_null) return true;
    tl = Labels().tlabels.Find(tg.tlabel);
    if (tl == kQ4None) tl = MutLabels().tlabels.Insert(tg.tlabel);
    return tl < kQ4MaxTl;
  };

  // 2. the dirty filings / tags take their current sub / tag rows
  for (const auto &adsh : buffer.dirty_adsh) {
    const auto it = subs.find(adsh);
    if (it != subs.end()) {
      const uint32_t a = EnsureAdsh(adsh);
      if (a == kQ4None || !set_sub(a, *it->second)) return false;
    } else {
      const uint32_t a = Coh().adsh.Find(adsh);
      if (a != kQ4None && Coh().sic_id[a] != kQ4None) MutCohort().sic_id[a] = kQ4None;
    }
  }
  for (const auto &tv : buffer.dirty_tv) {
    const auto it = tags.find(Join2(tv.first, tv.second));
    if (it != tags.end()) {
      uint32_t t, v, tl;
      if (!ensure_tag(tv.first, tv.second, t, v) || !label_of(*it->second, tl)) return false;
      MutTM().Upsert(Q4TagKey(t, v), tl);
    } else {
      const uint32_t t = TD().tags.Find(tv.first);
      const uint32_t v = TD().versions.Find(tv.second);
      if (t != kQ4None && v != kQ4None && TM().Find(Q4TagKey(t, v))) MutTM().Erase(Q4TagKey(t, v));
    }
  }

  // 3. add the current contribution
  std::vector<std::pair<uint32_t, uint32_t>> stmts;
  for (auto &kv : keys) {
    keys_out.insert(kv.first);
    const auto &k = kv.second;
    if (k.stmts.empty()) continue;
    const auto sub = subs.find(k.adsh);
    if (sub == subs.end()) continue;
    const auto tg = tags.find(Join2(k.tag, k.version));
    if (tg == tags.end()) continue;
    const uint32_t a = EnsureAdsh(k.adsh);
    if (a == kQ4None) return false;
    if (Coh().sic_id[a] == kQ4None && !set_sub(a, *sub->second)) return false;
    uint32_t t, v, tl;
    if (!ensure_tag(k.tag, k.version, t, v)) return false;
    if (const uint32_t *f = TM().Find(Q4TagKey(t, v))) {
      tl = *f;
    } else {
      if (!label_of(*tg->second, tl)) return false;
      MutTM().Upsert(Q4TagKey(t, v), tl);
    }
    stmts.clear();
    for (const auto &st : k.stmts) {
      uint32_t s = Sud().stmts.Find(st.first);
      if (s == kQ4None) {
        if (Sud().stmts.Size() >= kQ4MaxStmt) return false;
        s = MutSud().stmts.Insert(st.first);
      }
      stmts.emplace_back(s, st.second);
    }
    const int64_t pk = Q4PreKey(a, t, v);
    if (!PutPre(pk, tl, stmts)) return false;
    const uint32_t sic_id = Coh().sic_id[a];
    const uint32_t cik = Coh().cik_id[a];
    for (const auto &u_acc : k.uoms) {
      const auto &acc = u_acc.second;
      if (acc.nnc <= 0) continue;
      uint32_t u = Sud().uoms.Find(u_acc.first);
      if (u == kQ4None) {
        if (Sud().uoms.Size() >= kQ4MaxUom) return false;
        u = MutSud().uoms.Insert(u_acc.first);
      }
      for (const auto &sm : stmts) {
        const uint64_t gid = GroupFor(sic_id, tl, sm.first, u, true);
        if (!Contribute(gid, cik, acc.nh, acc.nl, static_cast<double>(sm.second),
                        static_cast<int64_t>(sm.second) * acc.nnc)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool Writer::ApplyNumDeltas(const SecEdgarQ4TxnBuffer &buffer,
                            const std::unordered_set<std::string> *skip) {
  const auto &deltas = buffer.Deltas();
  const size_t n = deltas.size();
  if (n == 0) return true;

  // 1. resolve (adsh, tag, version) -> pre key for the whole batch with
  //    pipelined lookups (the cache misses of different rows overlap)
  std::vector<std::string_view> va(n), vt(n), vv(n);
  for (size_t i = 0; i < n; ++i) {
    va[i] = buffer.Adsh(deltas[i]);
    vt[i] = buffer.Tag(deltas[i]);
    vv[i] = buffer.Version(deltas[i]);
  }
  std::vector<uint32_t> ia(n), it(n), iv(n);
  const auto &coh = Coh();
  coh.adsh.FindBatch(va.data(), n, ia.data());
  TD().tags.FindBatch(vt.data(), n, it.data());
  TD().versions.FindBatch(vv.data(), n, iv.data());
  std::vector<int64_t> pks(n);
  for (size_t i = 0; i < n; ++i) {
    const bool ok = ia[i] != kQ4None && it[i] != kQ4None && iv[i] != kQ4None &&
                    coh.sic_id[ia[i]] != kQ4None;
    pks[i] = ok ? Q4PreKey(ia[i], it[i], iv[i]) : int64_t{-1};
  }
  std::vector<const SecEdgarQ4PreVal *> pvs(n);
  const auto &pre = Pre();
  pre.FindBatch(pks.data(), n, pvs.data());

  // 2. net the deltas per (key, uom)
  struct Net {
    int64_t pk;
    SecEdgarQ4PreVal pv;
    uint32_t u;
    uint32_t next;
    double hi, lo;
    int64_t dnc;
  };
  std::vector<Net> nets;
  nets.reserve(n);
  core::FlatHashMap<uint32_t> head(n);
  std::string_view last_uom;
  uint32_t last_u = kQ4None;
  for (size_t i = 0; i < n; ++i) {
    if (pks[i] < 0 || !pvs[i]) continue; // the key is in no (stmt) group
    const auto &d = deltas[i];
    if (skip && skip->count(Join3(va[i], vt[i], vv[i]))) continue;
    const auto uom = buffer.Uom(d);
    uint32_t u;
    if (last_u != kQ4None && uom == last_uom) {
      u = last_u;
    } else {
      u = Sud().uoms.Find(uom);
      if (u == kQ4None) {
        if (d.sign <= 0 || Sud().uoms.Size() >= kQ4MaxUom) return false;
        u = MutSud().uoms.Insert(uom);
      }
      last_uom = uom;
      last_u = u;
    }
    const auto ins = head.Insert(pks[i], static_cast<uint32_t>(nets.size()));
    uint32_t idx = *ins.first;
    if (ins.second) {
      nets.push_back({pks[i], *pvs[i], u, kQ4None, 0.0, 0.0, 0});
    } else {
      while (nets[idx].u != u) {
        if (nets[idx].next == kQ4None) {
          const auto fresh = static_cast<uint32_t>(nets.size());
          nets[idx].next = fresh;
          nets.push_back({pks[i], *pvs[i], u, kQ4None, 0.0, 0.0, 0});
          idx = fresh;
          break;
        }
        idx = nets[idx].next;
      }
    }
    Net &net = nets[idx];
    SecEdgarQ4TxnBuffer::ForEachSigned(d, [&](int sign, double value) {
      Q4DDAdd(net.hi, net.lo, sign * value);
      net.dnc += sign;
    });
  }

  // 3. one group key per (net, stmt); group ids resolved as one batch
  struct Hit {
    uint32_t net;
    uint32_t mult;
  };
  std::vector<Hit> hits;
  std::vector<int64_t> gks;
  hits.reserve(nets.size() + nets.size() / 4);
  gks.reserve(nets.size() + nets.size() / 4);
  const auto &over = PreOver();
  uint32_t ss[64], ms[64];
  for (uint32_t k = 0; k < nets.size(); ++k) {
    const Net &net = nets[k];
    if (net.dnc == 0 && net.hi == 0.0 && net.lo == 0.0) continue;
    bool ok;
    const uint32_t ns = DecodeStmts(net.pk, net.pv.stmts, over, ss, ms, ok);
    if (!ok) return false;
    const uint32_t sic_id = coh.sic_id[Q4PreKeyAdsh(net.pk)];
    for (uint32_t j = 0; j < ns; ++j) {
      hits.push_back({k, ms[j]});
      gks.push_back(Q4GroupKey(sic_id, net.pv.tl, ss[j], net.u));
    }
  }
  std::vector<const uint64_t *> found(gks.size());
  GIndex().FindBatch(gks.data(), gks.size(), found.data());
  std::vector<uint64_t> gids(gks.size());
  for (size_t h = 0; h < gks.size(); ++h) gids[h] = found[h] ? *found[h] : kNoGroup;
  for (size_t h = 0; h < hits.size(); ++h) {
    const Net &net = nets[hits[h].net];
    uint64_t gid = gids[h];
    if (gid == kNoGroup) {
      if (net.dnc <= 0) return false; // rows of a group the state never counted
      gid = GroupFor(static_cast<uint32_t>(static_cast<uint64_t>(gks[h]) >> 40), net.pv.tl,
                     static_cast<uint32_t>((gks[h] >> 10) & 63), net.u, true);
    }
    const uint32_t cik = coh.cik_id[Q4PreKeyAdsh(net.pk)];
    if (!Contribute(gid, cik, net.hi, net.lo, static_cast<double>(hits[h].mult),
                    net.dnc * static_cast<int64_t>(hits[h].mult))) {
      return false;
    }
  }
  return true;
}

SecEdgarQ4State Writer::Finish() {
  SecEdgarQ4State out;
  out.cohort = mut_cohort_ ? std::shared_ptr<const SecEdgarQ4Cohort>(mut_cohort_) : base_.cohort;
  out.labels = mut_labels_ ? std::shared_ptr<const SecEdgarQ4Labels>(mut_labels_) : base_.labels;
  if (mut_tdict_ || mut_tags_ || mut_pre_ || mut_pre_over_) {
    auto b = std::make_shared<SecEdgarQ4Bridge>(*bridge_);
    if (mut_tdict_) b->tdict = mut_tdict_;
    if (mut_tags_) b->tags = mut_tags_;
    if (mut_pre_) b->pre = mut_pre_;
    if (mut_pre_over_) b->pre_over = mut_pre_over_;
    out.bridge = std::move(b);
  } else {
    out.bridge = base_.bridge;
  }
  auto g = std::make_shared<SecEdgarQ4Groups>();
  g->labels = out.labels;
  g->sud = mut_sud_ ? std::shared_ptr<const SecEdgarQ4Sud>(mut_sud_) : groups_->sud;
  g->su_slot = mut_su_slot_ ? std::shared_ptr<const core::FlatHashMap<uint32_t>>(mut_su_slot_)
                            : groups_->su_slot;
  g->slots = std::move(slots_);
  g->pairs = mut_pairs_ ? std::shared_ptr<const SecEdgarQ4PairMap>(mut_pairs_) : groups_->pairs;
  g->gindex =
      mut_gindex_ ? std::shared_ptr<const SecEdgarQ4GroupIndex>(mut_gindex_) : groups_->gindex;
  g->cohort = out.cohort;
  g->bridge = out.bridge;
  out.groups = std::move(g);
  return out;
}

// ---------------------------------------------------------------- Builder

namespace {

inline uint64_t BHash(uint64_t k) { return Q4Fmix(k); }

// One (adsh, tag, version) key with pre rows and the running num aggregate of
// its first uom (further uoms go to Impl::extra).  32 bytes, so that a num row
// probe touches one cache line; the stmts / tlabel of the key live in side
// arrays indexed by the same slot (only the pre pass and the expansion use them).
struct BEntry {
  uint64_t key1; // pre key + 1; 0 = empty slot
  double hi, lo;
  uint32_t nc;
  uint32_t u1; // uom id + 1; 0 = none yet
};
static_assert(sizeof(BEntry) == 32, "BEntry layout");

struct BAcc {
  double hi = 0.0, lo = 0.0;
  int64_t nc = 0;
};

// One joined (key, stmt, uom) contribution to a group.
struct GRec {
  int64_t gk;
  uint64_t cik_rows; // cik id (24 bits) | COUNT(value) * mult << 24
  double hi, lo;     // SUM(value) * mult (double-double)
};
constexpr int64_t kGRecMaxRows = int64_t{1} << 39;

struct TagSlot {
  uint64_t key1; // Q4TagKey + 1; 0 empty
  uint64_t tl;
};

constexpr size_t kAhead = 24;

// "DDDDDDDDDD-DD-DDDDDD" -> its 18 digits as an integer (+1, so 0 = not canonical).
inline uint64_t AdshNum1(std::string_view s) {
  if (s.size() != 20 || s[10] != '-' || s[13] != '-') return 0;
  uint64_t v = 0;
  for (size_t i = 0; i < 20; ++i) {
    if (i == 10 || i == 13) continue;
    const unsigned d = static_cast<unsigned char>(s[i]) - static_cast<unsigned>('0');
    if (d > 9) return 0;
    v = v * 10 + d;
  }
  return v + 1;
}
constexpr unsigned kGroupPartBits = 8;


// Zeroed array on (transparent) huge pages: the build table is probed at random.
template <typename T> class HugeArray {
public:
  HugeArray() = default;
  ~HugeArray() { Reset(); }
  HugeArray(const HugeArray &) = delete;
  HugeArray &operator=(const HugeArray &) = delete;
  void Allocate(size_t n) {
    Reset();
    if (n == 0) return;
    bytes_ = (n * sizeof(T) + kHuge - 1) / kHuge * kHuge;
    void *p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
#ifdef MADV_HUGEPAGE
    madvise(p, bytes_, MADV_HUGEPAGE);
#endif
    data_ = static_cast<T *>(p); // anonymous mappings are zero-filled
    size_ = n;
  }
  void Reset() {
    if (data_) munmap(data_, bytes_);
    data_ = nullptr;
    size_ = bytes_ = 0;
  }
  T &operator[](size_t i) { return data_[i]; }
  const T &operator[](size_t i) const { return data_[i]; }
  T *data() { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

private:
  static constexpr size_t kHuge = size_t{2} << 20;
  T *data_ = nullptr;
  size_t size_ = 0, bytes_ = 0;
};

} // namespace

struct Builder::Impl {
  SecEdgarQ4Cohort cohort;
  SecEdgarQ4TagDicts tdict;
  SecEdgarQ4Labels labels;
  SecEdgarQ4Sud sud;

  // canonical adsh number + 1 -> adsh id (open addressing; 0 = empty)
  std::vector<std::pair<uint64_t, uint64_t>> anum;
  size_t acount = 0;
  void AnumPut(uint64_t k1, uint32_t id) {
    if ((acount + 1) * 2 > anum.size()) {
      std::vector<std::pair<uint64_t, uint64_t>> old(std::move(anum));
      anum.assign(old.empty() ? 1024 : old.size() * 2, {0, 0});
      for (const auto &e : old) {
        if (!e.first) continue;
        size_t j = BHash(e.first) & (anum.size() - 1);
        while (anum[j].first) j = (j + 1) & (anum.size() - 1);
        anum[j] = e;
      }
    }
    size_t j = BHash(k1) & (anum.size() - 1);
    while (anum[j].first && anum[j].first != k1) j = (j + 1) & (anum.size() - 1);
    if (!anum[j].first) ++acount;
    anum[j] = {k1, id};
  }

  // (tag, version) -> tlabel id (kQ4None = NULL label); open addressing.
  std::vector<TagSlot> tslots;
  size_t tcount = 0;

  HugeArray<BEntry> table; // slot = top bits of BHash(key1), linear probing
  struct Side {
    uint64_t stm; // packed (stmt, mult) list or kQ4PreOverflow
    uint64_t tl;  // tlabel id
  };
  HugeArray<Side> side; // per slot
  size_t mask = 0;
  unsigned shift = 64;
  size_t count = 0;

  size_t Home(uint64_t key1) const { return static_cast<size_t>(BHash(key1) >> shift); }
  void Accumulate(uint64_t k1, double x, uint32_t u);
  bool num_started = false;
  std::unordered_map<uint64_t, SecEdgarQ4PreOverflow> over; // by pre key
  core::FlatHashMap<BAcc> extra;                             // (slot << 10 | uom) -> acc

  // joined contributions, radix-partitioned by group key
  std::vector<std::vector<GRec>> gparts;

  // scratch
  std::vector<uint64_t> k1;
  std::vector<uint32_t> home, aux;

  Impl() : gparts(size_t{1} << kGroupPartBits) {}

  void TagGrow() {
    const size_t cap = tslots.empty() ? 1024 : tslots.size() * 2;
    std::vector<TagSlot> old(std::move(tslots));
    tslots.assign(cap, TagSlot{0, 0});
    for (const auto &e : old) {
      if (!e.key1) continue;
      size_t j = BHash(e.key1) & (cap - 1);
      while (tslots[j].key1) j = (j + 1) & (cap - 1);
      tslots[j] = e;
    }
  }
  // Inserts; returns false when the key already exists.
  bool TagPut(int64_t key, uint32_t tl) {
    if ((tcount + 1) * 2 > tslots.size()) TagGrow();
    const uint64_t k = static_cast<uint64_t>(key) + 1;
    const size_t tmask = tslots.size() - 1;
    size_t j = BHash(k) & tmask;
    while (tslots[j].key1) {
      if (tslots[j].key1 == k) return false;
      j = (j + 1) & tmask;
    }
    tslots[j] = TagSlot{k, tl};
    ++tcount;
    return true;
  }
  const TagSlot *TagGet(int64_t key) const {
    if (tslots.empty()) return nullptr;
    const uint64_t k = static_cast<uint64_t>(key) + 1;
    const size_t tmask = tslots.size() - 1;
    size_t j = BHash(k) & tmask;
    while (tslots[j].key1) {
      if (tslots[j].key1 == k) return &tslots[j];
      j = (j + 1) & tmask;
    }
    return nullptr;
  }

  void Alloc(size_t cap) {
    table.Allocate(cap);
    side.Allocate(cap);
    mask = cap - 1;
    shift = 64;
    for (size_t c = cap; c > 1; c >>= 1) --shift;
    count = 0;
  }
  void GrowTable() {
    if (num_started) throw std::logic_error("sec_edgar q4 build: pre keys after num rows");
    const size_t old_size = table.size();
    struct Old {
      BEntry e;
      Side sd;
    };
    std::vector<Old> old;
    old.reserve(count);
    for (size_t j = 0; j < old_size; ++j) {
      if (table[j].key1) old.push_back({table[j], side[j]});
    }
    Alloc(old_size * 2);
    for (const auto &o : old) {
      size_t j = Home(o.e.key1);
      while (table[j].key1) j = (j + 1) & mask;
      table[j] = o.e;
      side[j] = o.sd;
      ++count;
    }
  }
  // Slot of `key1` (pre key + 1), inserted if absent.
  size_t Upsert(uint64_t key1, size_t j) {
    while (true) {
      const uint64_t cur = table[j].key1;
      if (cur == key1) return j;
      if (cur == 0) {
        table[j].key1 = key1;
        ++count;
        return j;
      }
      j = (j + 1) & mask;
    }
  }

  void AddStmt(size_t slot_index, uint32_t s, uint32_t mult, bool set_mult) {
    uint64_t &stmts = side[slot_index].stm;
    const uint64_t key1 = table[slot_index].key1;
    if (stmts != kQ4PreOverflow) {
      for (unsigned i = 0; i < 4; ++i) {
        const uint32_t slot = static_cast<uint32_t>((stmts >> (16 * i)) & 0xFFFF);
        if (slot == 0) {
          if (mult <= 1023) {
            stmts |= static_cast<uint64_t>(((s + 1) << 10) | mult) << (16 * i);
            return;
          }
          break;
        }
        if ((slot >> 10) - 1 == s) {
          if (set_mult) return; // bridge rows repeat (key, stmt) per uom
          if ((slot & 1023u) + mult <= 1023) {
            stmts += static_cast<uint64_t>(mult) << (16 * i);
            return;
          }
          break;
        }
      }
      // spill to the overflow list
      SecEdgarQ4PreOverflow rec;
      for (unsigned i = 0; i < 4; ++i) {
        const uint32_t slot = static_cast<uint32_t>((stmts >> (16 * i)) & 0xFFFF);
        if (slot == 0) break;
        rec.stmt[rec.n] = static_cast<uint8_t>((slot >> 10) - 1);
        rec.mult[rec.n] = slot & 1023u;
        ++rec.n;
      }
      over[key1 - 1] = rec;
      stmts = kQ4PreOverflow;
    }
    auto &rec = over[key1 - 1];
    for (uint32_t i = 0; i < rec.n; ++i) {
      if (rec.stmt[i] == s) {
        if (!set_mult) rec.mult[i] += mult;
        return;
      }
    }
    if (rec.n >= 16) throw std::length_error("sec_edgar q4: too many stmts for one key");
    rec.stmt[rec.n] = static_cast<uint8_t>(s);
    rec.mult[rec.n] = mult;
    ++rec.n;
  }

  uint32_t DecodeB(size_t j, uint32_t *s, uint32_t *m) const {
    uint32_t n = 0;
    if (side[j].stm == kQ4PreOverflow) {
      const auto &rec = over.at(table[j].key1 - 1);
      for (uint32_t i = 0; i < rec.n; ++i) {
        s[n] = rec.stmt[i];
        m[n] = rec.mult[i];
        ++n;
      }
      return n;
    }
    for (uint64_t v = side[j].stm; v != 0; v >>= 16) {
      const uint32_t slot = static_cast<uint32_t>(v & 0xFFFF);
      if (slot == 0) break;
      s[n] = (slot >> 10) - 1;
      m[n] = slot & 1023u;
      ++n;
    }
    return n;
  }

  void Contrib(int64_t gk, uint32_t cik, double hi, double lo, double mult, int64_t rows) {
    if (rows < 0 || rows >= kGRecMaxRows) throw std::length_error("sec_edgar q4: group row count");
    GRec r;
    r.gk = gk;
    r.cik_rows = (static_cast<uint64_t>(rows) << 24) | (cik == kQ4None ? kQ4MaxCik - 1 : cik);
    if (mult == 1.0) {
      r.hi = hi; // already a normalized double-double
      r.lo = lo;
    } else {
      r.hi = 0.0;
      r.lo = 0.0;
      Q4DDAddScaled(r.hi, r.lo, hi, mult);
      if (lo != 0.0) Q4DDAddScaled(r.hi, r.lo, lo, mult);
    }
    gparts[BHash(static_cast<uint64_t>(gk)) >> (64 - kGroupPartBits)].push_back(r);
  }

  uint32_t SubId(std::string_view adsh, bool cik_null, int32_t cik_v, int32_t sic) {
    const uint32_t a = cohort.adsh.Insert(adsh);
    if (a >= kQ4MaxAdsh) throw std::length_error("sec_edgar q4: too many filings");
    if (const uint64_t k1 = AdshNum1(adsh)) AnumPut(k1, a);
    if (cohort.cik_id.size() <= a) {
      cohort.cik_id.resize(a + 1, kQ4None);
      cohort.sic_id.resize(a + 1, kQ4None);
    } else if (cohort.sic_id[a] != kQ4None) {
      return a; // seen
    }
    uint32_t cik = kQ4None;
    if (!cik_null) {
      if (const uint32_t *f = cohort.cik_index.Find(cik_v)) {
        cik = *f;
      } else {
        cik = static_cast<uint32_t>(cohort.ciks.size());
        if (cik >= kQ4MaxCik - 1) throw std::length_error("sec_edgar q4: too many ciks");
        cohort.ciks.push_back(cik_v);
        cohort.cik_index.Upsert(cik_v, cik);
      }
    }
    uint32_t sic_id;
    if (const uint32_t *f = cohort.sic_index.Find(sic)) {
      sic_id = *f;
    } else {
      sic_id = static_cast<uint32_t>(cohort.sics.size());
      if (sic_id >= kQ4MaxSic) throw std::length_error("sec_edgar q4: too many sic values");
      cohort.sics.push_back(sic);
      cohort.sic_index.Upsert(sic, sic_id);
    }
    cohort.cik_id[a] = cik;
    cohort.sic_id[a] = sic_id;
    return a;
  }

  uint32_t LabelId(bool null, std::string_view tlabel) {
    if (null) return kQ4None;
    const uint32_t tl = labels.tlabels.Insert(tlabel);
    if (tl >= kQ4MaxTl) throw std::length_error("sec_edgar q4: too many tlabels");
    return tl;
  }
};

Builder::Builder() : impl_(std::make_unique<Impl>()) {}
Builder::~Builder() = default;

void Builder::AddSub(std::string_view adsh, bool cik_null, int32_t cik, int32_t sic) {
  auto &m = *impl_;
  if (m.cohort.adsh.Find(adsh) != kQ4None) {
    throw std::runtime_error("sec_edgar q4: duplicate sub.adsh");
  }
  m.SubId(adsh, cik_null, cik, sic);
}

void Builder::ReserveTags(size_t rows) {
  auto &m = *impl_;
  m.tdict.tags.Reserve(rows / 8 + 16, rows * 6);
  m.tdict.versions.Reserve(rows / 16 + 16, rows * 2);
  m.labels.tlabels.Reserve(rows / 8 + 16, rows * 8);
  size_t cap = 1024;
  while (cap < rows * 2) cap *= 2;
  if (m.tcount == 0 && cap > m.tslots.size()) m.tslots.assign(cap, TagSlot{0, 0});
}

void Builder::AddTag(std::string_view tag, std::string_view version, bool tlabel_null,
                     std::string_view tlabel) {
  auto &m = *impl_;
  const uint32_t t = m.tdict.tags.Insert(tag);
  const uint32_t v = m.tdict.versions.Insert(version);
  if (t >= kQ4MaxTag || v >= kQ4MaxVer) throw std::length_error("sec_edgar q4: too many tags");
  const uint32_t tl = m.LabelId(tlabel_null, tlabel);
  if (!m.TagPut(Q4TagKey(t, v), tl)) {
    throw std::runtime_error("sec_edgar q4: duplicate tag (tag, version)");
  }
}

void Builder::AddTags(const std::string_view *tag, const std::string_view *ver,
                      const std::string_view *label, const uint8_t *label_null, size_t n) {
  auto &m = *impl_;
  m.k1.resize(n);
  m.home.resize(n);
  m.aux.resize(n);
  uint32_t *t = m.home.data(), *l = m.aux.data();
  std::vector<uint32_t> v(n);
  m.tdict.tags.FindBatch(tag, n, t);
  m.tdict.versions.FindBatch(ver, n, v.data());
  m.labels.tlabels.FindBatch(label, n, l);
  for (size_t i = 0; i < n; ++i) {
    if (t[i] == kQ4None) t[i] = m.tdict.tags.Insert(tag[i]);
    if (v[i] == kQ4None) v[i] = m.tdict.versions.Insert(ver[i]);
    if (t[i] >= kQ4MaxTag || v[i] >= kQ4MaxVer) throw std::length_error("sec_edgar q4: too many tags");
    if (label_null[i]) {
      l[i] = kQ4None;
    } else if (l[i] == kQ4None) {
      l[i] = m.LabelId(false, label[i]);
    }
    m.k1[i] = static_cast<uint64_t>(Q4TagKey(t[i], v[i]));
  }
  if ((m.tcount + n) * 2 > m.tslots.size()) {
    while ((m.tcount + n) * 2 > m.tslots.size()) m.TagGrow();
  }
  const size_t tmask = m.tslots.size() - 1;
  for (size_t i = 0; i < n; ++i) {
    if (i + kAhead < n) __builtin_prefetch(&m.tslots[BHash(m.k1[i + kAhead] + 1) & tmask]);
    if (!m.TagPut(static_cast<int64_t>(m.k1[i]), l[i])) {
      throw std::runtime_error("sec_edgar q4: duplicate tag (tag, version)");
    }
  }
}

uint32_t Builder::AdshId(const char *p, size_t n) const {
  const uint32_t a = impl_->cohort.adsh.Find(p, n);
  return a != kQ4None && impl_->cohort.sic_id[a] != kQ4None ? a : kQ4None;
}
uint32_t Builder::TagId(const char *p, size_t n) const { return impl_->tdict.tags.Find(p, n); }
uint32_t Builder::VerId(const char *p, size_t n) const { return impl_->tdict.versions.Find(p, n); }
void Builder::AdshIds(const std::string_view *s, size_t n, uint32_t *out) const {
  // The cohort holds only sic IS NOT NULL filings.  Canonical accession
  // numbers go through the integer index; anything else through the strings.
  const auto &m = *impl_;
  thread_local std::vector<uint64_t> keys;
  thread_local std::vector<std::string_view> rest;
  thread_local std::vector<uint32_t> rest_at, rest_id;
  if (keys.size() < n) keys.resize(n);
  rest.clear();
  rest_at.clear();
  for (size_t i = 0; i < n; ++i) {
    keys[i] = AdshNum1(s[i]);
    if (!keys[i]) {
      rest_at.push_back(static_cast<uint32_t>(i));
      rest.push_back(s[i]);
    }
  }
  const size_t amask = m.anum.empty() ? 0 : m.anum.size() - 1;
  for (size_t i = 0; i < n; ++i) {
    if (!keys[i]) continue;
    if (i + kAhead < n && keys[i + kAhead] && !m.anum.empty()) {
      __builtin_prefetch(&m.anum[BHash(keys[i + kAhead]) & amask]);
    }
    uint32_t id = kQ4None;
    if (!m.anum.empty()) {
      for (size_t j = BHash(keys[i]) & amask; m.anum[j].first; j = (j + 1) & amask) {
        if (m.anum[j].first == keys[i]) {
          id = static_cast<uint32_t>(m.anum[j].second);
          break;
        }
      }
    }
    out[i] = id;
  }
  if (!rest.empty()) {
    rest_id.resize(rest.size());
    m.cohort.adsh.FindBatch(rest.data(), rest.size(), rest_id.data());
    for (size_t k = 0; k < rest.size(); ++k) out[rest_at[k]] = rest_id[k];
  }
}
void Builder::TagIds(const std::string_view *s, size_t n, uint32_t *out) const {
  impl_->tdict.tags.FindBatch(s, n, out);
}
void Builder::VerIds(const std::string_view *s, size_t n, uint32_t *out) const {
  impl_->tdict.versions.FindBatch(s, n, out);
}
uint32_t Builder::StmtId(const char *p, size_t n) {
  const uint32_t s = impl_->sud.stmts.Insert(p, n);
  if (s >= kQ4MaxStmt) throw std::length_error("sec_edgar q4: too many stmt values");
  return s;
}
uint32_t Builder::UomId(const char *p, size_t n) {
  const uint32_t u = impl_->sud.uoms.Insert(p, n);
  if (u >= kQ4MaxUom) throw std::length_error("sec_edgar q4: too many uom values");
  return u;
}

void Builder::BeginPre(size_t expected_rows) {
  size_t cap = 1024;
  while (cap * 3 < expected_rows * 5) cap *= 2; // load <= 0.6
  impl_->Alloc(cap);
}

void Builder::AddPre(const uint32_t *a, const uint32_t *t, const uint32_t *v, const uint32_t *s,
                     size_t n) {
  auto &m = *impl_;
  if (m.table.empty()) BeginPre(std::max<size_t>(n, 1024));
  while ((m.count + n) * 5 > m.table.size() * 3) m.GrowTable();
  m.k1.resize(n);
  m.home.resize(n);
  m.aux.resize(n);
  if (m.tslots.empty()) return;
  // 1. the (tag, version) must be a tag row with abstract = 0
  size_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    if (a[i] == kQ4None || t[i] == kQ4None || v[i] == kQ4None || s[i] == kQ4None) continue;
    m.k1[c] = static_cast<uint64_t>(Q4TagKey(t[i], v[i])) + 1;
    m.home[c] = static_cast<uint32_t>(i);
    ++c;
  }
  const size_t tmask = m.tslots.size() - 1;
  size_t w = 0;
  for (size_t i = 0; i < c; ++i) {
    if (i + kAhead < c) __builtin_prefetch(&m.tslots[BHash(m.k1[i + kAhead]) & tmask]);
    const uint64_t k = m.k1[i];
    size_t j = BHash(k) & tmask;
    while (m.tslots[j].key1 && m.tslots[j].key1 != k) j = (j + 1) & tmask;
    if (!m.tslots[j].key1) continue;
    const uint32_t r = m.home[i];
    m.aux[w] = static_cast<uint32_t>(m.tslots[j].tl);
    m.k1[w] = static_cast<uint64_t>(Q4PreKey(a[r], t[r], v[r])) + 1;
    m.home[w] = r;
    ++w;
  }
  // 2. upsert the pre keys
  for (size_t i = 0; i < w; ++i) {
    if (i + kAhead < w) {
      const size_t h = m.Home(m.k1[i + kAhead]);
      __builtin_prefetch(&m.table[h]);
      __builtin_prefetch(&m.side[h]);
    }
    const size_t j = m.Upsert(m.k1[i], m.Home(m.k1[i]));
    m.side[j].tl = m.aux[i];
    m.AddStmt(j, s[m.home[i]], 1, false);
  }
}

void Builder::Impl::Accumulate(uint64_t k1, double x, uint32_t u) {
  size_t j = Home(k1);
  while (table[j].key1 && table[j].key1 != k1) j = (j + 1) & mask;
  if (!table[j].key1) return; // no pre row for the key
  auto &e = table[j];
  const uint32_t u1 = u + 1;
  if (e.u1 == u1 || e.u1 == 0) {
    e.u1 = u1;
    Q4DDAdd(e.hi, e.lo, x);
    ++e.nc;
  } else {
    auto &acc = extra[static_cast<int64_t>((static_cast<uint64_t>(j) << 10) | u)];
    Q4DDAdd(acc.hi, acc.lo, x);
    ++acc.nc;
  }
}

void Builder::AddNum(const uint32_t *a, const uint32_t *t, const uint32_t *v, const uint32_t *u,
                     const double *x, size_t n) {
  auto &m = *impl_;
  if (m.table.empty()) return;
  m.num_started = true;
  m.k1.resize(n);
  m.home.resize(n);
  size_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    if (a[i] == kQ4None || t[i] == kQ4None || v[i] == kQ4None || u[i] == kQ4None) continue;
    m.k1[c] = static_cast<uint64_t>(Q4PreKey(a[i], t[i], v[i])) + 1;
    m.home[c] = static_cast<uint32_t>(i);
    ++c;
  }
  // One random cache line per row: prefetch kAhead rows ahead.
  for (size_t i = 0; i < c; ++i) {
    if (i + kAhead < c) __builtin_prefetch(&m.table[m.Home(m.k1[i + kAhead])]);
    const uint32_t r = m.home[i];
    m.Accumulate(m.k1[i], x[r], u[r]);
  }
}


void Builder::AddBridgeRow(const SecEdgarQ4BridgeRow &row) {
  auto &m = *impl_;
  if (row.mult == 0) return;
  if (m.table.empty()) BeginPre(1024);
  while ((m.count + 1) * 5 > m.table.size() * 3) m.GrowTable();
  const uint32_t a = m.SubId(row.adsh, row.cik_null, row.cik, row.sic);
  const uint32_t t = m.tdict.tags.Insert(row.tag);
  const uint32_t v = m.tdict.versions.Insert(row.version);
  if (t >= kQ4MaxTag || v >= kQ4MaxVer) throw std::length_error("sec_edgar q4: too many tags");
  uint32_t tl;
  if (const TagSlot *f = m.TagGet(Q4TagKey(t, v))) {
    tl = static_cast<uint32_t>(f->tl);
  } else {
    tl = m.LabelId(row.tlabel_null, row.tlabel);
    m.TagPut(Q4TagKey(t, v), tl);
  }
  const uint32_t s = StmtId(row.stmt.data(), row.stmt.size());
  const uint64_t k1 = static_cast<uint64_t>(Q4PreKey(a, t, v)) + 1;
  const size_t j = m.Upsert(k1, m.Home(k1));
  m.side[j].tl = tl;
  m.AddStmt(j, s, row.mult, true);
  if (!row.has_num || row.nc <= 0) return;
  const uint32_t u = UomId(row.uom.data(), row.uom.size());
  m.Contrib(Q4GroupKey(m.cohort.sic_id[a], tl, s, u), m.cohort.cik_id[a], row.ns, row.ns_lo,
            static_cast<double>(row.mult), static_cast<int64_t>(row.mult) * row.nc);
}

SecEdgarQ4State Builder::Finish() {
  auto &m = *impl_;
  auto t0 = std::chrono::steady_clock::now();
  const bool profile = std::getenv("Q4_BUILD_PROFILE") != nullptr;
  const auto lap = [&](const char *what) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    fprintf(stderr, "q4 finish %s: %.1f ms\n", what,
            std::chrono::duration<double, std::milli>(now - t0).count());
    t0 = now;
  };
  // 1. one pass over the keys: the pre map (partitioned by its shards) and
  //    every (key, stmt, uom) contribution (partitioned by group key)
  std::vector<std::vector<std::pair<int64_t, SecEdgarQ4PreVal>>> pre_parts(SecEdgarQ4PreMap::kShards);
  {
    const size_t per = m.count / SecEdgarQ4PreMap::kShards + 8;
    for (auto &p : pre_parts) p.reserve(per + per / 4);
    const size_t gper = m.count / m.gparts.size() + 8;
    for (auto &p : m.gparts) p.reserve(p.size() + gper + gper / 4);
  }
  std::vector<std::pair<int64_t, SecEdgarQ4PreOverflow>> over_entries;
  uint32_t ss[64], ms[64];
  for (size_t j = 0; j < m.table.size(); ++j) {
    const auto &e = m.table[j];
    if (!e.key1) continue;
    const int64_t pk = static_cast<int64_t>(e.key1 - 1);
    pre_parts[SecEdgarQ4PreMap::ShardOf(pk)].emplace_back(
        pk, SecEdgarQ4PreVal{m.side[j].stm, static_cast<uint32_t>(m.side[j].tl), 0});
    if (m.side[j].stm == kQ4PreOverflow) over_entries.emplace_back(pk, m.over.at(e.key1 - 1));
    if (e.u1 == 0 || e.nc == 0) continue;
    const uint32_t a = Q4PreKeyAdsh(pk);
    const uint32_t sic_id = m.cohort.sic_id[a], cik = m.cohort.cik_id[a];
    const uint32_t ns = m.DecodeB(j, ss, ms);
    for (uint32_t i = 0; i < ns; ++i) {
      m.Contrib(Q4GroupKey(sic_id, static_cast<uint32_t>(m.side[j].tl), ss[i], e.u1 - 1), cik, e.hi, e.lo,
                static_cast<double>(ms[i]), static_cast<int64_t>(ms[i]) * e.nc);
    }
  }
  for (auto kv : m.extra) {
    const size_t j = static_cast<size_t>(static_cast<uint64_t>(kv.first) >> 10);
    const uint32_t u = static_cast<uint32_t>(kv.first & 1023);
    const auto &e = m.table[j];
    const uint32_t a = Q4PreKeyAdsh(static_cast<int64_t>(e.key1 - 1));
    const uint32_t ns = m.DecodeB(j, ss, ms);
    for (uint32_t i = 0; i < ns; ++i) {
      m.Contrib(Q4GroupKey(m.cohort.sic_id[a], static_cast<uint32_t>(m.side[j].tl), ss[i], u), m.cohort.cik_id[a],
                kv.second.hi, kv.second.lo, static_cast<double>(ms[i]),
                static_cast<int64_t>(ms[i]) * kv.second.nc);
    }
  }
  m.table.Reset();
  m.side.Reset();
  m.extra.Clear();
  lap("expand");

  // 2. aggregate each group partition (its groups and pairs fit in cache)
  std::vector<int64_t> gkey;
  std::vector<double> ghi, glo;
  std::vector<int64_t> gcnt;
  std::vector<uint32_t> dist;
  struct PairRec {
    uint32_t g;
    uint32_t cik;
    int64_t rows;
  };
  std::vector<PairRec> prs;
  {
    core::FlatHashMap<uint32_t> local;
    core::FlatHashMap<int64_t> lpairs;
    for (auto &part : m.gparts) {
      if (part.empty()) continue;
      local = core::FlatHashMap<uint32_t>(part.size() / 4 + 16);
      lpairs = core::FlatHashMap<int64_t>(part.size() / 2 + 16);
      const uint32_t base = static_cast<uint32_t>(gkey.size());
      for (const auto &r : part) {
        const auto ins = local.Insert(r.gk, static_cast<uint32_t>(gkey.size()));
        const uint32_t g = *ins.first;
        if (ins.second) {
          gkey.push_back(r.gk);
          ghi.push_back(0.0);
          glo.push_back(0.0);
          gcnt.push_back(0);
        }
        Q4DDAdd(ghi[g], glo[g], r.hi);
        if (r.lo != 0.0) Q4DDAdd(ghi[g], glo[g], r.lo);
        const int64_t rows = static_cast<int64_t>(r.cik_rows >> 24);
        const uint32_t cik = static_cast<uint32_t>(r.cik_rows & (kQ4MaxCik - 1));
        gcnt[g] += rows;
        if (cik != kQ4MaxCik - 1) {
          lpairs[static_cast<int64_t>((static_cast<uint64_t>(g - base) << 24) | cik)] += rows;
        }
      }
      dist.resize(gkey.size(), 0);
      for (auto kv : lpairs) {
        if (kv.second == 0) continue;
        const uint32_t g = base + static_cast<uint32_t>(static_cast<uint64_t>(kv.first) >> 24);
        ++dist[g];
        prs.push_back({g, static_cast<uint32_t>(kv.first & (kQ4MaxCik - 1)), kv.second});
      }
      std::vector<GRec>().swap(part);
    }
  }
  lap("aggregate");

  // 3. groups: per (stmt, uom) slot, ordered by sic, then tlabel id
  const size_t ng = gkey.size();
  std::vector<uint32_t> order(ng);
  std::iota(order.begin(), order.end(), 0u);
  const auto su_of = [&](uint32_t g) {
    const uint64_t k = static_cast<uint64_t>(gkey[g]);
    return static_cast<uint32_t>(((k >> 10) & 63) << 16 | (k & 1023));
  };
  const auto sic_of = [&](uint32_t g) { return m.cohort.sics[static_cast<uint64_t>(gkey[g]) >> 40]; };
  std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
    const uint32_t sx = su_of(x), sy = su_of(y);
    if (sx != sy) return sx < sy;
    const int32_t cx = sic_of(x), cy = sic_of(y);
    if (cx != cy) return cx < cy;
    return gkey[x] < gkey[y];
  });
  auto groups = std::make_shared<SecEdgarQ4Groups>();
  auto su_slot = std::make_shared<core::FlatHashMap<uint32_t>>();
  std::vector<uint64_t> final_gid(ng);
  std::vector<std::pair<int64_t, uint64_t>> gindex_entries;
  gindex_entries.reserve(ng);
  SecEdgarQ4SuGroups *cur = nullptr;
  uint32_t cur_su = kQ4None;
  for (const uint32_t g : order) {
    const uint32_t su = su_of(g);
    if (!cur || su != cur_su) {
      const uint32_t slot = static_cast<uint32_t>(groups->slots.size());
      if (slot >= kQ4MaxSlot) throw std::length_error("sec_edgar q4: (stmt, uom) slot space exhausted");
      auto sg = std::make_shared<SecEdgarQ4SuGroups>();
      sg->stmt = su >> 16;
      sg->uom = su & 0xFFFF;
      su_slot->Upsert(su, slot);
      cur = sg.get();
      cur_su = su;
      groups->slots.push_back(std::move(sg));
    }
    const uint32_t local = cur->n++;
    if (local >= kQ4MaxLocal) throw std::length_error("sec_edgar q4: group id space exhausted");
    const uint32_t idx = local >> kQ4GroupShardBits, i = local & (kQ4GroupShardSize - 1);
    const uint32_t c = idx >> kQ4ChunkBits, ci = idx & (kQ4ChunkShards - 1);
    if (c == cur->chunks.size()) cur->chunks.push_back(std::make_shared<SecEdgarQ4ShardChunk>());
    auto &chunk = *cur->chunks[c];
    if (!chunk.shard[ci]) chunk.shard[ci] = std::make_shared<SecEdgarQ4GroupShard>();
    auto &sh = *chunk.shard[ci];
    const uint64_t k = static_cast<uint64_t>(gkey[g]);
    const int32_t sic = sic_of(g);
    const uint32_t tl = static_cast<uint32_t>((k >> 16) & 0xFFFFFF);
    sh.sic[i] = sic;
    sh.tl[i] = tl == kQ4MaxTl ? kQ4None : tl;
    sh.sum_hi[i] = ghi[g];
    sh.sum_lo[i] = glo[g];
    sh.count[i] = gcnt[g];
    sh.distinct[i] = dist[g];
    chunk.min_sic[ci] = std::min(chunk.min_sic[ci], sic);
    chunk.max_sic[ci] = std::max(chunk.max_sic[ci], sic);
    final_gid[g] = Q4Gid(static_cast<uint32_t>(groups->slots.size() - 1), local);
    gindex_entries.emplace_back(gkey[g], final_gid[g]);
  }
  std::vector<std::pair<int64_t, int64_t>> pair_entries;
  pair_entries.reserve(prs.size());
  for (const auto &p : prs) pair_entries.emplace_back(Q4PairKey(final_gid[p.g], p.cik), p.rows);
  std::vector<PairRec>().swap(prs);
  lap("groups");

  auto gindex = std::make_shared<SecEdgarQ4GroupIndex>();
  gindex->BulkLoad(gindex_entries);
  auto pairs = std::make_shared<SecEdgarQ4PairMap>();
  pairs->BulkLoad(pair_entries);
  lap("gindex+pairs");
  auto pre = std::make_shared<SecEdgarQ4PreMap>();
  pre->BulkLoadParts(pre_parts);
  auto pre_over = std::make_shared<SecEdgarQ4PreOverflowMap>();
  pre_over->BulkLoad(over_entries);
  lap("pre map");
  std::vector<std::pair<int64_t, uint32_t>> tag_entries;
  tag_entries.reserve(m.tcount);
  for (const auto &e : m.tslots) {
    if (e.key1) tag_entries.emplace_back(static_cast<int64_t>(e.key1 - 1), static_cast<uint32_t>(e.tl));
  }
  auto tags = std::make_shared<SecEdgarQ4TagMap>();
  tags->BulkLoad(tag_entries);
  lap("tags");

  SecEdgarQ4State out;
  out.cohort = std::make_shared<const SecEdgarQ4Cohort>(std::move(m.cohort));
  out.labels = std::make_shared<const SecEdgarQ4Labels>(std::move(m.labels));
  auto bridge = std::make_shared<SecEdgarQ4Bridge>();
  bridge->tdict = std::make_shared<const SecEdgarQ4TagDicts>(std::move(m.tdict));
  bridge->tags = std::move(tags);
  bridge->pre = std::move(pre);
  bridge->pre_over = std::move(pre_over);
  out.bridge = std::move(bridge);
  groups->labels = out.labels;
  groups->sud = std::make_shared<const SecEdgarQ4Sud>(std::move(m.sud));
  groups->su_slot = std::move(su_slot);
  groups->pairs = std::move(pairs);
  groups->gindex = std::move(gindex);
  groups->cohort = out.cohort;
  groups->bridge = out.bridge;
  out.groups = std::move(groups);
  return out;
}

// ---------------------------------------------------------------- Top-K

std::shared_ptr<const SecEdgarQ4Result>
ComputeResult(const SecEdgarQ4Groups &groups, const SecEdgarQ4Params &params) {
  struct Cand {
    double sum;
    int32_t sic;
    uint32_t tl;
    uint32_t shard;
    uint32_t i;
  };
  const auto tp0 = std::chrono::steady_clock::now();
  auto result = std::make_shared<SecEdgarQ4Result>();
  if (params.empty || params.sic_lo > params.sic_hi || !groups.sud || !groups.labels ||
      !groups.su_slot) {
    return result;
  }
  const uint32_t stmt = groups.sud->stmts.Find(params.stmt);
  const uint32_t uom = groups.sud->uoms.Find(params.uom);
  if (stmt == kQ4None || uom == kQ4None) {
    return result;
  }
  const uint32_t *slot = groups.su_slot->Find(Q4SuKey(stmt, uom));
  if (!slot) return result;
  const SecEdgarQ4SuGroups &sg = *groups.slots[*slot];
  const int32_t lo = params.sic_lo, hi = params.sic_hi;

  std::vector<Cand> cands;
  const uint32_t nshards = sg.Shards();
  for (uint32_t k = 0; k < nshards; ++k) {
    const auto &chunk = *sg.chunks[k >> kQ4ChunkBits];
    const uint32_t ci = k & (kQ4ChunkShards - 1);
    if (chunk.max_sic[ci] < lo || chunk.min_sic[ci] > hi) continue;
    const auto &sh = *chunk.shard[ci];
    const uint32_t limit = std::min<uint32_t>(kQ4GroupShardSize, sg.n - k * kQ4GroupShardSize);
    for (uint32_t i = 0; i < limit; ++i) {
      if (sh.distinct[i] >= static_cast<uint32_t>(kMinDistinctCik) && sh.count[i] > 0 &&
          sh.sic[i] >= lo && sh.sic[i] <= hi) {
        cands.push_back({sh.sum_hi[i] + sh.sum_lo[i], sh.sic[i], sh.tl[i], k, i});
      }
    }
  }
  const auto tp1 = std::chrono::steady_clock::now();
  const auto &tls = groups.labels->tlabels;
  const auto better = [&](const Cand &a, const Cand &b) {
    const bool an = std::isnan(a.sum), bn = std::isnan(b.sum);
    if (an != bn) return an; // NaN sorts as the largest value (DuckDB)
    if (!an && a.sum != b.sum) return a.sum > b.sum;
    if (a.sic != b.sic) return a.sic < b.sic;
    if (a.tl == b.tl) return false;
    if (a.tl == kQ4None) return true;
    if (b.tl == kQ4None) return false;
    return tls.Get(a.tl) < tls.Get(b.tl);
  };
  if (cands.size() > kOutputLimit) {
    std::nth_element(cands.begin(), cands.begin() + kOutputLimit, cands.end(), better);
    cands.resize(kOutputLimit);
  }
  const size_t ncands = cands.size();
  std::sort(cands.begin(), cands.end(), better);
  const auto tp2 = std::chrono::steady_clock::now();

  result->rows.reserve(cands.size());
  for (const auto &c : cands) {
    const auto &sh = sg.ShardAt(c.shard);
    SecEdgarQ4ResultRow row;
    row.sic = c.sic;
    row.tl = c.tl;
    row.distinct = sh.distinct[c.i];
    row.total = c.sum;
    row.avg = c.sum / static_cast<double>(sh.count[c.i]);
    result->rows.push_back(row);
  }
  if (std::getenv("Q4_APPLY_PROFILE")) {
    const auto tp3 = std::chrono::steady_clock::now();
    fprintf(stderr, "q4 compute scan=%.1f sort=%.1f rows=%.1f us cands=%zu shards=%u\n",
            std::chrono::duration<double, std::micro>(tp1 - tp0).count(),
            std::chrono::duration<double, std::micro>(tp2 - tp1).count(),
            std::chrono::duration<double, std::micro>(tp3 - tp2).count(), ncands, nshards);
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
