#include "tpch/q13/duckdb/aux/module.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/q13/calibration_writes.hpp"
#include "tpch/q13/layout.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace accelerator_codegen::aux_state::query {
namespace {

// Shards per slice of the bulk token index and match counts (~3000 us at SF 1).
constexpr std::size_t kFinishGrain = 32;
// Shards per slice when histogramming customers (untimed).
constexpr std::size_t kHistGrain = 64;
// Shards per slice of the untracked-pattern read (untimed).
constexpr std::size_t kEvalGrain = 1024;
// Shard runs per maintenance slice (untimed); RF-sized commits stay inline.
constexpr std::size_t kMaintGrain = 4096;

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

std::shared_ptr<const TpchQ13StateImage>
MakeStateImage(TpchQ13ImageKind kind,
               const std::shared_ptr<const TpchQ13State> &state) {
  auto image = std::make_shared<TpchQ13StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const TpchQ13StateImage *AsQ13Image(const core::StateImage &image,
                                    TpchQ13ImageKind kind) {
  const auto *payload = image.As<TpchQ13StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

bool Q13OutputRowBetter(const TpchQ13OutputRow &lhs,
                        const TpchQ13OutputRow &rhs) {
  if (lhs.custdist != rhs.custdist)
    return lhs.custdist > rhs.custdist;
  return lhs.c_count > rhs.c_count;
}

constexpr int64_t kMaxKey = int64_t{1} << 36;  // customer keys beyond fail the build
constexpr std::size_t kShardMask = kTpchQ13Shards - 1;

bool KeyOk(int64_t ck) { return ck >= 0 && ck < kMaxKey; }

// Leftmost occurrence of nd[0..m) in h[0..n), or nullptr.  AVX2: compare the
// first and last needle byte at 32 positions at once, then verify the middle.
const char *FindLit(const char *h, std::size_t n, const char *nd, std::size_t m) {
  if (m == 0)
    return h;
  if (n < m)
    return nullptr;
  if (m == 1)
    return static_cast<const char *>(std::memchr(h, nd[0], n));
#if defined(__AVX2__)
  if (n >= m + 32) {
    const __m256i first = _mm256_set1_epi8(nd[0]);
    const __m256i last = _mm256_set1_epi8(nd[m - 1]);
    const std::size_t lastpos = n - m;  // candidate positions 0..lastpos
    std::size_t i = 0;
    for (; i + 32 <= lastpos + 1; i += 32) {
      const __m256i a =
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(h + i));
      const __m256i b =
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(h + i + m - 1));
      uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(
          _mm256_and_si256(_mm256_cmpeq_epi8(a, first),
                           _mm256_cmpeq_epi8(b, last))));
      while (mask) {
        const unsigned bit = static_cast<unsigned>(__builtin_ctz(mask));
        if (m <= 2 || std::memcmp(h + i + bit + 1, nd + 1, m - 2) == 0)
          return h + i + bit;
        mask &= mask - 1;
      }
    }
    for (; i <= lastpos; ++i)
      if (h[i] == nd[0] && h[i + m - 1] == nd[m - 1] &&
          (m <= 2 || std::memcmp(h + i + 1, nd + 1, m - 2) == 0))
        return h + i;
    return nullptr;
  }
#endif
  return static_cast<const char *>(memmem(h, n, nd, m));
}

bool SegAt(const char *t, const TpchQ13Like::Seg &s) {
  const std::size_t m = s.text.size();
  if (!s.wild)
    return std::memcmp(t, s.text.data(), m) == 0;
  for (std::size_t k = 0; k < m; ++k)
    if (s.text[k] != '_' && t[k] != s.text[k])
      return false;
  return true;
}

const char *SegFind(const char *t, std::size_t n, const TpchQ13Like::Seg &s) {
  const std::size_t m = s.text.size();
  if (m == 0)
    return t;
  if (n < m)
    return nullptr;
  if (!s.wild)
    return FindLit(t, n, s.text.data(), m);
  for (std::size_t i = 0; i + m <= n; ++i)
    if (SegAt(t + i, s))
      return t + i;
  return nullptr;
}

// Count changes of a batch of order/customer updates, kept apart from the
// state so slices owning disjoint shards can run concurrently; summed in
// slice order into the state by Mut::Absorb.
struct Tally {
  std::vector<int64_t> base;
  std::vector<std::vector<int64_t>> hist;
  std::vector<int32_t> df;
  int64_t live_orders = 0;
  bool ok = true; // false: an update found no row to delete

  void DfAdd(uint16_t tok, int64_t d) {
    if (tok >= df.size())
      df.resize(std::size_t{tok} + 1, 0);
    df[tok] += static_cast<int32_t>(d);
  }
  void Merge(const Tally &from) {
    MergeCounts(base, from.base);
    if (hist.size() < from.hist.size())
      hist.resize(from.hist.size());
    for (std::size_t p = 0; p < from.hist.size(); ++p)
      MergeCounts(hist[p], from.hist[p]);
    if (df.size() < from.df.size())
      df.resize(from.df.size(), 0);
    for (std::size_t t = 0; t < from.df.size(); ++t)
      df[t] += from.df[t];
    live_orders += from.live_orders;
    ok = ok && from.ok;
  }
  template <class T>
  static void MergeCounts(std::vector<T> &into, const std::vector<T> &from) {
    if (into.size() < from.size())
      into.resize(from.size(), 0);
    for (std::size_t i = 0; i < from.size(); ++i)
      into[i] += from[i];
  }
};

void HistAdd(std::vector<int64_t> &h, int64_t eff, int64_t delta) {
  if (eff < 0)
    eff = 0;
  if (static_cast<std::size_t>(eff) >= h.size())
    h.resize(static_cast<std::size_t>(eff) + 1, 0);
  h[static_cast<std::size_t>(eff)] += delta;
}

// Assigns each tracked pattern the bitmask of literals an order must contain.
void AssignLits(TpchQ13State &st) {
  auto table = std::make_shared<TpchQ13LitTable>();
  for (auto &p : st.patterns) {
    p.need = 0;
    if (p.like.chunks.empty())
      continue;
    uint64_t need = 0;
    bool ok = true;
    for (const auto &chunk : p.like.chunks) {
      std::size_t idx = 0;
      while (idx < table->lits.size() && table->lits[idx] != chunk)
        ++idx;
      if (idx == table->lits.size()) {
        if (idx >= 64) {
          ok = false;
          break;
        }
        table->lits.push_back(chunk);
      }
      need |= uint64_t{1} << idx;
    }
    if (ok)
      p.need = need;
  }
  st.lits = std::move(table);
}

uint64_t LitMask(const TpchQ13LitTable &t, const char *d, std::size_t n) {
  uint64_t mask = 0;
  for (std::size_t i = 0; i < t.lits.size(); ++i)
    if (FindLit(d, n, t.lits[i].data(), t.lits[i].size()))
      mask |= uint64_t{1} << i;
  return mask;
}

bool PatMatch(const TpchQ13Pattern &p, uint64_t mask, const char *d,
              std::size_t n) {
  if (p.need && (mask & p.need) != p.need)
    return false;
  return p.like.Matches(d, n);
}

std::vector<TpchQ13OutputRow> SortedRows(const std::vector<int64_t> &h) {
  std::vector<TpchQ13OutputRow> out;
  for (std::size_t e = 0; e < h.size(); ++e)
    if (h[e] > 0)
      out.push_back({static_cast<int64_t>(e), h[e]});
  std::sort(out.begin(), out.end(), Q13OutputRowBetter);
  return out;
}

constexpr std::size_t kMaxTokLen = 64;
constexpr std::size_t kMaxVocab = 60000;
constexpr std::size_t kMaxIndexedOrders = 65535;  // postings are uint16

const bool *TokTable() {
  static const std::array<bool, 256> table = [] {
    std::array<bool, 256> t{};
    for (int c = 0; c < 256; ++c)
      t[static_cast<std::size_t>(c)] = (c >= '0' && c <= '9') ||
                                       (c >= 'a' && c <= 'z') ||
                                       (c >= 'A' && c <= 'Z');
    return t;
  }();
  return table.data();
}

// Distinct token ids of one comment (sorted).  False when some token could not
// be indexed (too long, or the vocabulary is full): the order is then verified
// by every query instead.  intern(string_view) returns an id or -1.
template <class Intern>
bool TokenizeOrder(const char *d, std::size_t n, Intern &intern,
                   std::vector<uint16_t> &ids) {
  const bool *tok = TokTable();
  ids.clear();
  bool ok = true;
  std::size_t i = 0;
  while (i < n) {
    if (!tok[static_cast<unsigned char>(d[i])]) {
      ++i;
      continue;
    }
    std::size_t j = i + 1;
    while (j < n && tok[static_cast<unsigned char>(d[j])])
      ++j;
    const int id = j - i > kMaxTokLen ? -1 : intern(std::string_view(d + i, j - i));
    if (id < 0)
      ok = false;
    else
      ids.push_back(static_cast<uint16_t>(id));
    i = j;
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ok;
}

template <class DfAdd>
void ForgetDf(const TpchQ13TokIndex &ix, DfAdd &df_add) {
  for (std::size_t k = 0; k < ix.toks.size(); ++k)
    df_add(ix.toks[k], -static_cast<int64_t>(ix.off[k + 1] - ix.off[k]));
}

template <class DfAdd>
void ForgetIndex(TpchQ13TokIndex &ix, DfAdd &df_add) {
  ForgetDf(ix, df_add);
  ix = TpchQ13TokIndex{};
}

// Indexes the orders [ix.covered, N) of a segment (all of it after a
// compaction): merges their (token, order) pairs into the segment's postings.
// `alive` is the liveness of the segment's orders (dead ones are not indexed).
template <class Intern, class DfAdd>
void IndexSeg(TpchQ13OrderSeg &o, const uint8_t *alive, Intern &intern,
              DfAdd &df_add) {
  TpchQ13TokIndex &ix = o.ix;
  const std::size_t N = o.custkey.size();
  if (ix.disabled || ix.covered >= N)
    return;
  if (N > kMaxIndexedOrders) {
    ForgetIndex(ix, df_add);
    ix.disabled = true;
    return;
  }
  std::vector<uint32_t> keys;
  std::vector<uint16_t> ids;
  for (std::size_t j = ix.covered; j < N; ++j) {
    if (!alive[j])
      continue;
    if (!TokenizeOrder(o.blob.data() + o.start[j], o.len[j], intern, ids))
      ix.always.push_back(static_cast<uint16_t>(j));
    for (const uint16_t id : ids) {
      keys.push_back((uint32_t{id} << 16) | static_cast<uint32_t>(j));
      df_add(id, 1);
    }
  }
  std::sort(keys.begin(), keys.end());
  if (!ix.toks.empty()) {
    std::vector<uint32_t> merged;
    merged.reserve(ix.ords.size() + keys.size());
    std::size_t kk = 0;
    for (std::size_t k = 0; k < ix.toks.size(); ++k)
      for (uint32_t p = ix.off[k]; p < ix.off[k + 1]; ++p) {
        const uint32_t key = (uint32_t{ix.toks[k]} << 16) | ix.ords[p];
        while (kk < keys.size() && keys[kk] < key)
          merged.push_back(keys[kk++]);
        merged.push_back(key);
      }
    while (kk < keys.size())
      merged.push_back(keys[kk++]);
    keys.swap(merged);
  }
  ix.toks.clear();
  ix.off.clear();
  ix.ords.clear();
  ix.ords.reserve(keys.size());
  for (const uint32_t key : keys) {
    const uint16_t tok = static_cast<uint16_t>(key >> 16);
    if (ix.toks.empty() || ix.toks.back() != tok) {
      ix.toks.push_back(tok);
      ix.off.push_back(static_cast<uint32_t>(ix.ords.size()));
    }
    ix.ords.push_back(static_cast<uint16_t>(key & 0xFFFF));
  }
  ix.off.push_back(static_cast<uint32_t>(ix.ords.size()));
  ix.toks.shrink_to_fit();
  ix.off.shrink_to_fit();
  ix.ords.shrink_to_fit();
  ix.always.shrink_to_fit();
  ix.covered = static_cast<uint32_t>(N);
}

// Folds the (fully indexed) tail into the base: a new immutable segment with
// the concatenated orders and the merged postings (no re-tokenizing).  The
// shard's `alive` is unchanged (base orders then tail orders, same order).
template <class DfAdd>
void MergeTail(TpchQ13OrderShard &o, DfAdd &df_add) {
  if (o.tail.custkey.empty())
    return;
  if (!o.base) {
    o.base = std::make_shared<const TpchQ13OrderSeg>(std::move(o.tail));
    o.tail = TpchQ13OrderSeg{};
    return;
  }
  const TpchQ13OrderSeg &b = *o.base;
  TpchQ13OrderSeg &t = o.tail;
  auto n = std::make_shared<TpchQ13OrderSeg>();
  const uint32_t nb = static_cast<uint32_t>(b.custkey.size());
  const std::size_t total = b.custkey.size() + t.custkey.size();
  n->custkey.reserve(total);
  n->start.reserve(total);
  n->len.reserve(total);
  n->custkey.insert(n->custkey.end(), b.custkey.begin(), b.custkey.end());
  n->custkey.insert(n->custkey.end(), t.custkey.begin(), t.custkey.end());
  n->start.insert(n->start.end(), b.start.begin(), b.start.end());
  const uint32_t boff = static_cast<uint32_t>(b.blob.size());
  for (const uint32_t st : t.start)
    n->start.push_back(st + boff);
  n->len.insert(n->len.end(), b.len.begin(), b.len.end());
  n->len.insert(n->len.end(), t.len.begin(), t.len.end());
  n->blob.reserve(b.blob.size() + t.blob.size());
  n->blob.append(b.blob);
  n->blob.append(t.blob);
  TpchQ13TokIndex &ix = n->ix;
  if (b.ix.disabled || t.ix.disabled || total > kMaxIndexedOrders ||
      b.ix.covered != b.custkey.size() || t.ix.covered != t.custkey.size()) {
    ForgetDf(b.ix, df_add);
    ForgetDf(t.ix, df_add);
    ix.disabled = true;
  } else {
    const TpchQ13TokIndex &bi = b.ix, &ti = t.ix;
    ix.ords.reserve(bi.ords.size() + ti.ords.size());
    std::size_t i = 0, j = 0;
    while (i < bi.toks.size() || j < ti.toks.size()) {
      const bool hb = i < bi.toks.size(), ht = j < ti.toks.size();
      const uint16_t tok = !ht || (hb && bi.toks[i] <= ti.toks[j]) ? bi.toks[i]
                                                                  : ti.toks[j];
      ix.toks.push_back(tok);
      ix.off.push_back(static_cast<uint32_t>(ix.ords.size()));
      if (hb && bi.toks[i] == tok) {
        ix.ords.insert(ix.ords.end(), bi.ords.begin() + bi.off[i],
                       bi.ords.begin() + bi.off[i + 1]);
        ++i;
      }
      if (ht && ti.toks[j] == tok) {
        for (uint32_t p = ti.off[j]; p < ti.off[j + 1]; ++p)
          ix.ords.push_back(static_cast<uint16_t>(ti.ords[p] + nb));
        ++j;
      }
    }
    ix.off.push_back(static_cast<uint32_t>(ix.ords.size()));
    ix.always = bi.always;
    for (const uint16_t a : ti.always)
      ix.always.push_back(static_cast<uint16_t>(a + nb));
    ix.covered = static_cast<uint32_t>(total);
  }
  o.base = std::move(n);
  o.tail = TpchQ13OrderSeg{};
}

// Tail orders needed before a tail is folded into the base (testing knob).
std::size_t TailMergeMin() {
  static const std::size_t v = [] {
    const char *e = std::getenv("ACCEL_Q13_TAIL_MERGE_MIN");
    return e ? static_cast<std::size_t>(std::atoll(e)) : std::size_t{16};
  }();
  return v;
}

// Mutable working copy of a state.  Only shards touched through O()/C() are
// cloned; every other shard stays shared with the published version.
struct Mut {
  TpchQ13State st;
  std::vector<std::shared_ptr<TpchQ13OrderShard>> mo;
  std::vector<std::shared_ptr<TpchQ13CustShard>> mc;

  Mut() {
    st.orders.assign(kTpchQ13Shards, nullptr);
    st.cust.assign(kTpchQ13Shards, nullptr);
    st.vocab = std::make_shared<const TpchQ13Vocab>();
    mo.resize(kTpchQ13Shards);
    mc.resize(kTpchQ13Shards);
  }
  explicit Mut(const TpchQ13State &base) : st(base) {
    mo.resize(kTpchQ13Shards);
    mc.resize(kTpchQ13Shards);
  }
  // Call after st.patterns is filled (fresh states only).
  void InitPatterns() {
    st.hist.assign(st.patterns.size(), {});
    AssignLits(st);
  }
  TpchQ13OrderShard &O(std::size_t s) {
    if (!mo[s])
      mo[s] = st.orders[s] ? std::make_shared<TpchQ13OrderShard>(*st.orders[s])
                           : std::make_shared<TpchQ13OrderShard>();
    return *mo[s];
  }
  TpchQ13CustShard &C(std::size_t s) {
    if (!mc[s])
      mc[s] = st.cust[s] ? std::make_shared<TpchQ13CustShard>(*st.cust[s])
                         : std::make_shared<TpchQ13CustShard>();
    return *mc[s];
  }
  const TpchQ13OrderShard *ReadO(std::size_t s) const {
    return mo[s] ? mo[s].get() : st.orders[s].get();
  }
  const TpchQ13CustShard *ReadC(std::size_t s) const {
    return mc[s] ? mc[s].get() : st.cust[s].get();
  }
  // Writable vocabulary (cloned on first use; shared otherwise).
  std::shared_ptr<TpchQ13Vocab> vw;
  const TpchQ13Vocab &Vocab() const { return vw ? *vw : *st.vocab; }
  int Intern(std::string_view t) {
    const auto &v = Vocab();
    const auto it = v.id.find(std::string(t));
    if (it != v.id.end())
      return it->second;
    if (v.toks.size() >= kMaxVocab)
      return -1;
    if (!vw)
      vw = std::make_shared<TpchQ13Vocab>(*st.vocab);
    const uint16_t id = static_cast<uint16_t>(vw->toks.size());
    vw->toks.emplace_back(t);
    vw->id.emplace(vw->toks.back(), id);
    return id;
  }
  void DfAdd(uint16_t tok, int64_t d) {
    if (tok >= st.df.size())
      st.df.resize(std::size_t{tok} + 1, 0);
    st.df[tok] += static_cast<int32_t>(d);
  }
  // Folds a batch's count changes into the state.
  void Absorb(const Tally &tally) {
    Tally::MergeCounts(st.base, tally.base);
    for (std::size_t p = 0; p < tally.hist.size(); ++p)
      Tally::MergeCounts(st.hist[p], tally.hist[p]);
    Tally::MergeCounts(st.df, tally.df);
    st.live_orders += tally.live_orders;
  }
  TpchQ13State Finish() {
    {  // index the orders appended since the shards were last indexed
      auto intern = [&](std::string_view t) { return Intern(t); };
      auto df_add = [&](uint16_t t, int64_t d) { DfAdd(t, d); };
      for (std::size_t s = 0; s < kTpchQ13Shards; ++s) {
        if (!mo[s])
          continue;
        TpchQ13OrderShard &o = *mo[s];
        IndexSeg(o.tail, o.alive.data() + o.BaseSize(), intern, df_add);
        const std::size_t tn = o.tail.custkey.size();
        if (tn > 0 && (!o.base || (tn > TailMergeMin() && tn * 8 > o.BaseSize())))
          MergeTail(o, df_add);
      }
      if (vw)
        st.vocab = std::move(vw);
    }
    st.sorted.assign(st.hist.size(), {});
    for (std::size_t p = 0; p < st.hist.size(); ++p)
      st.sorted[p] = SortedRows(st.hist[p]);
    for (std::size_t s = 0; s < kTpchQ13Shards; ++s) {
      if (mo[s])
        st.orders[s] = std::move(mo[s]);
      if (mc[s])
        st.cust[s] = std::move(mc[s]);
    }
    return std::move(st);
  }
};

// A shard keeps the identity layout while its local ids stay within twice its
// slot count plus this slack, and lists them explicitly beyond.
constexpr std::size_t kIdentitySlack = 1024;

// The slot of local id `lc`, created (all counts zero) when new.
std::size_t EnsureCust(TpchQ13CustShard &c, std::size_t lc, std::size_t np) {
  if (c.match.size() < np)
    c.match.resize(np);
  std::size_t slot = lc;
  if (c.ids.empty() && lc < 2 * c.total.size() + kIdentitySlack) {
    if (lc >= c.total.size()) {
      c.total.resize(lc + 1, 0);
      c.mult.resize(lc + 1, 0);
    }
  } else {
    if (c.ids.empty()) {
      c.ids.resize(c.total.size());
      std::iota(c.ids.begin(), c.ids.end(), 0u);
    }
    const auto it = std::lower_bound(c.ids.begin(), c.ids.end(), static_cast<uint32_t>(lc));
    slot = static_cast<std::size_t>(it - c.ids.begin());
    if (it == c.ids.end() || *it != lc) {
      for (auto &m : c.match)
        if (m.size() == c.total.size())
          m.insert(m.begin() + static_cast<std::ptrdiff_t>(slot), 0);
      c.total.insert(c.total.begin() + static_cast<std::ptrdiff_t>(slot), 0);
      c.mult.insert(c.mult.begin() + static_cast<std::ptrdiff_t>(slot), 0);
      c.ids.insert(c.ids.begin() + static_cast<std::ptrdiff_t>(slot), static_cast<uint32_t>(lc));
    }
  }
  for (auto &m : c.match)
    if (m.size() < c.total.size())
      m.resize(c.total.size(), 0);
  return slot;
}

// Rewrites the live orders of base and tail into a fresh tail (no base); the
// caller has forgotten the old indexes and Finish() re-indexes/folds the tail.
void CompactOrders(TpchQ13OrderShard &o) {
  TpchQ13OrderSeg n;
  std::vector<uint8_t> alive;
  const std::size_t live = o.Size() - o.dead;
  n.custkey.reserve(live);
  n.start.reserve(live);
  n.len.reserve(live);
  alive.assign(live, 1);
  n.blob.reserve(o.base ? o.base->blob.size() / 2 + o.tail.blob.size() + 64 : 64);
  std::size_t g = 0;
  auto take = [&](const TpchQ13OrderSeg &sg) {
    for (std::size_t i = 0; i < sg.custkey.size(); ++i, ++g) {
      if (!o.alive[g])
        continue;
      n.custkey.push_back(sg.custkey[i]);
      n.start.push_back(static_cast<uint32_t>(n.blob.size()));
      n.len.push_back(sg.len[i]);
      n.blob.append(sg.blob, sg.start[i], sg.len[i]);
    }
  };
  if (o.base)
    take(*o.base);
  take(o.tail);
  o.base.reset();
  o.tail = std::move(n);
  o.alive = std::move(alive);
  o.dead = 0;
}

// c_count is (#customer rows with this key) * (#non-matching orders): the LEFT
// JOIN multiplies rows for duplicated customer keys.  Adds `delta` customers to
// the histograms at the customer's current counts.
void HistAll(Tally &tally, std::size_t np, const TpchQ13CustShard &c,
             std::size_t slot, int64_t delta) {
  const int64_t mult = c.mult[slot];
  if (tally.hist.size() < np)
    tally.hist.resize(np);
  HistAdd(tally.base, mult * c.total[slot], delta);
  for (std::size_t p = 0; p < np; ++p)
    HistAdd(tally.hist[p], mult * (c.total[slot] - c.match[p][slot]), delta);
}

enum class Apply { Live, Raw };

// A key outside the supported range: the initial load fails loudly; a live
// update asks for a rebuild (which then fails the same way).
bool RejectKey(Apply mode) {
  if (mode == Apply::Raw)
    throw std::runtime_error("TPC-H Q13: customer key outside [0, 2^36)");
  return false;
}

// Applies one order insert (sign>0) or delete (sign<0).  Raw (initial load)
// only stores the order and counts it; matches/histograms come from BulkFinish.
// Returns false when a delete finds no matching order (caller rebuilds).
bool ApplyOrder(Mut &m, Tally &tally, int64_t ck, std::string_view comment,
                int sign, Apply mode) {
  if (!KeyOk(ck))
    return RejectKey(mode);
  const std::size_t s = static_cast<std::size_t>(ck) & kShardMask;
  const std::size_t lc = static_cast<std::size_t>(ck) >> kTpchQ13ShardBits;
  const std::size_t np = m.st.patterns.size();
  TpchQ13OrderShard &o = m.O(s);
  tally.live_orders += sign;
  if (sign > 0) {
    TpchQ13OrderSeg &t = o.tail;
    if (t.blob.size() + comment.size() > std::numeric_limits<uint32_t>::max())
      throw std::runtime_error("TPC-H Q13: order comments of one shard exceed 4 GiB");
    t.custkey.push_back(ck);
    t.start.push_back(static_cast<uint32_t>(t.blob.size()));
    t.len.push_back(static_cast<uint32_t>(comment.size()));
    t.blob.append(comment.data(), comment.size());
    o.alive.push_back(1);
  } else {
    bool found = false;
    // Newest first: the tail, then the base.  `goff` = global index of sg[0].
    auto find_in = [&](const TpchQ13OrderSeg &sg, std::size_t goff) {
      const int64_t *ckv = sg.custkey.data();
      for (std::size_t i = sg.custkey.size(); i > 0; --i) {
        const std::size_t idx = i - 1;
        if (ckv[idx] != ck || !o.alive[goff + idx] || sg.len[idx] != comment.size())
          continue;
        if (std::memcmp(sg.blob.data() + sg.start[idx], comment.data(),
                        comment.size()) != 0)
          continue;
        o.alive[goff + idx] = 0;
        ++o.dead;
        return true;
      }
      return false;
    };
    found = find_in(o.tail, o.BaseSize()) ||
            (o.base && find_in(*o.base, 0));
    if (!found) {
      tally.live_orders -= sign;
      return false;
    }
    if (o.dead > 32 && static_cast<std::size_t>(o.dead) * 2 > o.Size()) {
      auto df_add = [&](uint16_t t, int64_t d) { tally.DfAdd(t, d); };
      if (o.base)
        ForgetDf(o.base->ix, df_add);
      ForgetIndex(o.tail.ix, df_add);
      CompactOrders(o);  // re-indexed from scratch by Finish
    }
  }
  TpchQ13CustShard &c = m.C(s);
  const std::size_t slot = EnsureCust(c, lc, np);
  if (mode == Apply::Raw) {
    c.total[slot] += sign;
    return true;
  }
  const bool valid = c.mult[slot] > 0;
  if (valid)
    HistAll(tally, np, c, slot, -1);
  c.total[slot] += sign;
  const uint64_t mask = LitMask(*m.st.lits, comment.data(), comment.size());
  for (std::size_t p = 0; p < np; ++p)
    if (PatMatch(m.st.patterns[p], mask, comment.data(), comment.size()))
      c.match[p][slot] += sign;
  if (valid)
    HistAll(tally, np, c, slot, +1);
  return true;
}

// Customer row insert (sign>0) / delete (sign<0).  False on delete of a
// non-existent customer.
bool ApplyCustomer(Mut &m, Tally &tally, int64_t ck, int sign, Apply mode) {
  if (!KeyOk(ck))
    return RejectKey(mode);
  const std::size_t s = static_cast<std::size_t>(ck) & kShardMask;
  const std::size_t lc = static_cast<std::size_t>(ck) >> kTpchQ13ShardBits;
  TpchQ13CustShard &c = m.C(s);
  const std::size_t np = m.st.patterns.size();
  const std::size_t slot = EnsureCust(c, lc, np);
  if (sign < 0 && c.mult[slot] <= 0)
    return false;
  if (mode == Apply::Live && c.mult[slot] > 0)
    HistAll(tally, np, c, slot, -1);
  c.mult[slot] += sign;
  if (mode == Apply::Live && c.mult[slot] > 0)
    HistAll(tally, np, c, slot, +1);
  return true;
}

// Calls hit(order) once per order that contains `lit` wholly inside its own
// bytes (occurrences straddling two orders are ignored).  AVX2: first/last
// needle byte compared at 32 positions per step over the whole shard blob; the
// scan skips to the end of an order after its first hit.
template <class F>
void ForOrdersWithLit(const TpchQ13OrderSeg &o, const std::string &lit,
                      F &&hit) {
  const char *b = o.blob.data();
  const std::size_t n = o.blob.size(), N = o.custkey.size(), m = lit.size();
  if (m == 0 || n < m)
    return;
  const uint32_t *start = o.start.data();
  const uint32_t *len = o.len.data();
  std::size_t j = 0;
  std::size_t floor = 0;  // first position that may still start a useful hit
  const std::size_t lastpos = n - m;  // candidate start positions 0..lastpos
  // Candidate at `pos`: true once the order is hit (floor moves past it).
  auto candidate = [&](std::size_t pos) {
    if (m > 2 && std::memcmp(b + pos + 1, lit.data() + 1, m - 2) != 0)
      return;
    while (j < N && std::size_t{start[j]} + len[j] <= pos)
      ++j;
    if (j >= N)
      return;
    const std::size_t end = std::size_t{start[j]} + len[j];
    if (pos + m <= end) {
      hit(j);
      floor = end;
    }
  };
  std::size_t i = 0;
#if defined(__AVX2__)
  const __m256i first = _mm256_set1_epi8(lit[0]);
  const __m256i last = _mm256_set1_epi8(lit[m - 1]);
  while (i + 32 <= lastpos + 1) {
    if (floor >= i + 32) {
      i = floor;
      continue;
    }
    const __m256i x =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i));
    __m256i eq = _mm256_cmpeq_epi8(x, first);
    if (m > 1) {
      const __m256i y = _mm256_loadu_si256(
          reinterpret_cast<const __m256i *>(b + i + m - 1));
      eq = _mm256_and_si256(eq, _mm256_cmpeq_epi8(y, last));
    }
    uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(eq));
    if (floor > i)
      mask &= ~uint32_t{0} << (floor - i);
    while (mask) {
      const std::size_t pos = i + static_cast<unsigned>(__builtin_ctz(mask));
      const std::size_t before = floor;
      candidate(pos);
      if (floor != before) {  // order hit: drop positions inside it
        if (floor >= i + 32)
          break;
        mask &= ~uint32_t{0} << (floor - i);
      } else {
        mask &= mask - 1;
      }
    }
    i += 32;
  }
#endif
  for (i = std::max(i, floor); i <= lastpos; ++i) {
    if (floor > i)
      continue;
    if (b[i] == lit[0] && b[i + m - 1] == lit[m - 1])
      candidate(i);
  }
}

// Vocabulary shared by the parallel bulk indexers.  Each thread interns through
// a private cache, so the lock is taken only the first time a thread sees a
// token.
struct SharedVocab {
  std::mutex mu;
  TpchQ13Vocab v;
};

struct LocalIntern {
  SharedVocab *shared;
  std::unordered_map<std::string, int> cache;
  int operator()(std::string_view t) {
    const std::string key(t);
    const auto it = cache.find(key);
    if (it != cache.end())
      return it->second;
    int id;
    {
      std::lock_guard<std::mutex> lock(shared->mu);
      const auto g = shared->v.id.find(key);
      if (g != shared->v.id.end()) {
        id = g->second;
      } else if (shared->v.toks.size() >= kMaxVocab) {
        id = -1;
      } else {
        id = static_cast<int>(shared->v.toks.size());
        shared->v.toks.push_back(key);
        shared->v.id.emplace(key, static_cast<uint16_t>(id));
      }
    }
    cache.emplace(key, id);
    return id;
  }
};

// Initial load, per shard (in parallel slices): match counts of every tracked pattern,
// the token index, then trim the shard's vectors to size.
void BulkFinish(Mut &m, const core::AuxEngineServices &services) {
  const std::size_t np = m.st.patterns.size();
  const std::size_t slices = core::SliceCount(services, kTpchQ13Shards, kFinishGrain);
  SharedVocab shared;
  shared.v = *m.st.vocab;
  std::vector<std::vector<int32_t>> dfs(slices);
  core::RunSlices(services, slices, kTpchQ13Shards,
           [&](std::size_t slice, std::size_t b, std::size_t e) {
             std::vector<uint64_t> mask;
             LocalIntern intern{&shared, {}};
             auto df_add = [&](uint16_t t, int64_t d) {
               auto &df = dfs[slice];
               if (t >= df.size())
                 df.resize(std::size_t{t} + 1, 0);
               df[t] += static_cast<int32_t>(d);
             };
             for (std::size_t s = b; s < e; ++s) {
               TpchQ13OrderShard *os = m.mo[s].get();
               TpchQ13CustShard *c = m.mc[s].get();
               if (!os || !c)
                 continue;
               TpchQ13OrderSeg *o = &os->tail;  // initial load: all in the tail
               const std::size_t N = o->custkey.size();
               mask.assign(N, 0);
               const auto &lits = m.st.lits->lits;
               for (std::size_t i = 0; i < lits.size(); ++i) {
                 const uint64_t bit = uint64_t{1} << i;
                 ForOrdersWithLit(*o, lits[i],
                                  [&](std::size_t j) { mask[j] |= bit; });
               }
               for (std::size_t j = 0; j < N; ++j) {
                 if (!os->alive[j])
                   continue;
                 const char *d = o->blob.data() + o->start[j];
                 const std::size_t slot = c->SlotOf(static_cast<uint32_t>(
                     static_cast<std::size_t>(o->custkey[j]) >> kTpchQ13ShardBits));
                 for (std::size_t p = 0; p < np; ++p)
                   if (PatMatch(m.st.patterns[p], mask[j], d, o->len[j]))
                     ++c->match[p][slot];
               }
               IndexSeg(*o, os->alive.data(), intern, df_add);
               o->custkey.shrink_to_fit();
               o->start.shrink_to_fit();
               o->len.shrink_to_fit();
               os->alive.shrink_to_fit();
               o->blob.shrink_to_fit();
               MergeTail(*os, df_add);
               c->ids.shrink_to_fit();
               c->total.shrink_to_fit();
               c->mult.shrink_to_fit();
               for (auto &v : c->match)
                 v.shrink_to_fit();
             }
           });
  m.st.vocab = std::make_shared<const TpchQ13Vocab>(std::move(shared.v));
  m.st.df.assign(m.st.vocab->toks.size(), 0);
  for (const auto &df : dfs)
    for (std::size_t t = 0; t < df.size() && t < m.st.df.size(); ++t)
      m.st.df[t] += df[t];
}

// Histograms (tracked patterns and the no-match base) from the customer arrays.
void RecomputeHist(Mut &m, const core::AuxEngineServices &services) {
  const std::size_t np = m.st.patterns.size();
  const std::size_t slices = core::SliceCount(services, kTpchQ13Shards, kHistGrain);
  std::vector<std::vector<std::vector<int64_t>>> part(
      slices, std::vector<std::vector<int64_t>>(np + 1));
  core::RunSlices(services, slices, kTpchQ13Shards,
           [&](std::size_t slice, std::size_t b, std::size_t e) {
             auto &h = part[slice];
             for (std::size_t s = b; s < e; ++s) {
               const TpchQ13CustShard *c = m.ReadC(s);
               if (!c)
                 continue;
               for (std::size_t slot = 0; slot < c->total.size(); ++slot) {
                 if (c->mult[slot] <= 0)
                   continue;
                 const int64_t mult = c->mult[slot];
                 HistAdd(h[np], mult * c->total[slot], 1);
                 for (std::size_t p = 0; p < np; ++p)
                   HistAdd(h[p],
                           mult * (c->total[slot] -
                                   (p < c->match.size() ? c->match[p][slot] : 0)),
                           1);
               }
             }
           });
  m.st.hist.assign(np, {});
  m.st.base.clear();
  for (std::size_t t = 0; t < slices; ++t) {
    for (std::size_t p = 0; p <= np; ++p) {
      auto &dst = p < np ? m.st.hist[p] : m.st.base;
      if (part[t][p].size() > dst.size())
        dst.resize(part[t][p].size(), 0);
      for (std::size_t i = 0; i < part[t][p].size(); ++i)
        dst[i] += part[t][p][i];
    }
  }
}

TpchQ13Pattern MakePattern(const std::string &text) {
  TpchQ13Pattern p;
  p.text = text;
  p.like.Build(text);
  return p;
}

// The 16 TPC-H Q13 parameter patterns '%word1%word2%'.
std::vector<std::string> DomainPatterns() {
  static const char *w1[] = {"special", "pending", "unusual", "express"};
  static const char *w2[] = {"packages", "requests", "accounts", "deposits"};
  std::vector<std::string> out;
  for (const char *a : w1)
    for (const char *b : w2)
      out.push_back(std::string("%") + a + "%" + b + "%");
  return out;
}

const core::OutputSchema &Q13Schema() {
  static const core::OutputSchema schema{{{"c_count", core::AuxType::Int64, false},
                                          {"custdist", core::AuxType::Int64, false}}};
  return schema;
}

// Emits rows that are already in output order, reusing one row buffer.
void EmitSorted(const std::vector<TpchQ13OutputRow> &rows,
                core::AuxRowSink &sink) {
  thread_local core::AuxRow row = [] {
    core::AuxRow r;
    r.values.resize(2);
    for (auto &v : r.values) {
      v.type = core::AuxType::Int64;
      v.is_null = false;
      v.payload = int64_t{0};
    }
    return r;
  }();
  sink.Begin(Q13Schema());
  for (const auto &r : rows) {
    row.values[0].payload.emplace<int64_t>(r.c_count);
    row.values[1].payload.emplace<int64_t>(r.custdist);
    sink.WriteRow(row);
  }
  sink.End();
}

// Candidate orders for an untracked pattern from the token index: for each of
// a few of the pattern's literal pieces, the vocabulary tokens containing it
// (an order that matches must contain such a token for every piece).
struct IndexPlan {
  struct Piece {
    std::vector<uint8_t> member;  // token id -> in set
    std::vector<uint16_t> ids;    // the same set, sorted
  };
  std::vector<Piece> pieces;
  // "%word%" made only of token bytes: an order holding a token that contains
  // the word matches, so candidates need no verification (except `always`).
  bool exact = false;
};

double IndexFraction() {
  if (const char *v = std::getenv("ACCEL_Q13_IDX_FRAC"))
    return std::atof(v);
  return 0.15;
}

bool PlanIndex(const TpchQ13State &st, const TpchQ13Like &like,
               IndexPlan &plan) {
  const bool *tok = TokTable();
  std::vector<std::string> pieces;
  for (const auto &chunk : like.chunks) {
    std::size_t i = 0;
    while (i < chunk.size()) {
      if (!tok[static_cast<unsigned char>(chunk[i])]) {
        ++i;
        continue;
      }
      std::size_t j = i + 1;
      while (j < chunk.size() && tok[static_cast<unsigned char>(chunk[j])])
        ++j;
      std::string piece = chunk.substr(i, j - i);
      if (std::find(pieces.begin(), pieces.end(), piece) == pieces.end())
        pieces.push_back(std::move(piece));
      i = j;
    }
  }
  if (pieces.empty() || !st.vocab)
    return false;
  const auto &voc = *st.vocab;
  struct Cand {
    int64_t est = 0;
    IndexPlan::Piece piece;
  };
  std::vector<Cand> cands;
  for (const auto &piece : pieces) {
    Cand c;
    c.piece.member.assign(voc.toks.size(), 0);
    for (std::size_t t = 0; t < voc.toks.size(); ++t) {
      if (FindLit(voc.toks[t].data(), voc.toks[t].size(), piece.data(),
                  piece.size())) {
        c.piece.member[t] = 1;
        c.piece.ids.push_back(static_cast<uint16_t>(t));
        if (t < st.df.size())
          c.est += st.df[t];
      }
    }
    cands.push_back(std::move(c));
  }
  std::sort(cands.begin(), cands.end(),
            [](const Cand &a, const Cand &b) { return a.est < b.est; });
  const double live = static_cast<double>(std::max<int64_t>(st.live_orders, 1));
  const bool exact =
      like.single_lit && pieces.size() == 1 && pieces[0] == like.anchor;
  // Expected candidates (pieces assumed independent).  Verifying a candidate
  // costs several times a scan step; counting an exact one costs much less.
  double frac = 1.0;
  std::size_t used = 0;
  for (std::size_t i = 0; i < cands.size() && used < 3; ++i) {
    const double f = static_cast<double>(cands[i].est) / live;
    if (i > 0 && f > 0.5)
      break;
    frac *= std::min(1.0, f);
    ++used;
  }
  const double limit = exact ? 0.5 : IndexFraction();
  if (static_cast<double>(cands[0].est) > 0.6 * live || frac > limit)
    return false;
  for (std::size_t i = 0; i < used; ++i)
    plan.pieces.push_back(std::move(cands[i].piece));
  plan.exact = exact;
  return true;
}

// Customers owning matching orders of one shard: move them between buckets.
// `m` is zero scratch (left zero), `touched` is empty scratch.
void QuerySeg(const TpchQ13OrderSeg *o, const TpchQ13CustShard &c, const uint8_t *alive,
              const TpchQ13Like &like, bool anchored_scan, const IndexPlan *plan,
              std::vector<int32_t> &m, std::vector<uint32_t> &touched) {
  const char *blob = o->blob.data();
  auto count = [&](std::size_t j) {
    const std::size_t slot = c.SlotOf(static_cast<uint32_t>(
        static_cast<std::size_t>(o->custkey[j]) >> kTpchQ13ShardBits));
    if (slot == TpchQ13CustShard::kNone)
      return;
    if (m[slot]++ == 0)
      touched.push_back(static_cast<uint32_t>(slot));
  };
  auto check = [&](std::size_t j) {
    if (alive[j] && like.Matches(blob + o->start[j], o->len[j]))
      count(j);
  };
  const std::size_t N = o->custkey.size();
  if (plan && !o->ix.disabled && o->ix.covered == N) {
    const TpchQ13TokIndex &ix = o->ix;
    thread_local std::vector<uint64_t> cand, tmp;
    const std::size_t W = (N + 63) >> 6;
    cand.assign(W, 0);
    for (std::size_t pi = 0; pi < plan->pieces.size(); ++pi) {
      const auto &piece = plan->pieces[pi];
      std::vector<uint64_t> &dst = pi == 0 ? cand : tmp;
      if (pi > 0)
        tmp.assign(W, 0);
      auto mark = [&](std::size_t k) {
        for (uint32_t p = ix.off[k]; p < ix.off[k + 1]; ++p)
          dst[ix.ords[p] >> 6] |= uint64_t{1} << (ix.ords[p] & 63);
      };
      if (piece.ids.size() * 8 < ix.toks.size()) {
        for (const uint16_t id : piece.ids) {
          const auto it = std::lower_bound(ix.toks.begin(), ix.toks.end(), id);
          if (it != ix.toks.end() && *it == id)
            mark(static_cast<std::size_t>(it - ix.toks.begin()));
        }
      } else {
        for (std::size_t k = 0; k < ix.toks.size(); ++k)
          if (ix.toks[k] < piece.member.size() && piece.member[ix.toks[k]])
            mark(k);
      }
      if (pi > 0)
        for (std::size_t w = 0; w < W; ++w)
          cand[w] &= tmp[w];
    }
    if (plan->exact) {
      thread_local std::vector<uint64_t> alw;
      alw.assign(W, 0);
      for (const uint16_t a : ix.always)
        alw[a >> 6] |= uint64_t{1} << (a & 63);
      for (std::size_t w = 0; w < W; ++w) {
        uint64_t bits = cand[w] & ~alw[w];
        while (bits) {
          const std::size_t j =
              w * 64 + static_cast<unsigned>(__builtin_ctzll(bits));
          bits &= bits - 1;
          if (j < N && alive[j])
            count(j);
        }
        bits = alw[w];
        while (bits) {
          const std::size_t j =
              w * 64 + static_cast<unsigned>(__builtin_ctzll(bits));
          bits &= bits - 1;
          if (j < N)
            check(j);
        }
      }
    } else {
      for (const uint16_t a : ix.always)
        cand[a >> 6] |= uint64_t{1} << (a & 63);
      for (std::size_t w = 0; w < W; ++w) {
        uint64_t bits = cand[w];
        while (bits) {
          const std::size_t j =
              w * 64 + static_cast<unsigned>(__builtin_ctzll(bits));
          bits &= bits - 1;
          if (j < N)
            check(j);
        }
      }
    }
  } else if (anchored_scan && like.single_lit) {  // a hit is the match
    ForOrdersWithLit(*o, like.anchor, [&](std::size_t j) {
      if (alive[j])
        count(j);
    });
  } else if (anchored_scan) {
    ForOrdersWithLit(*o, like.anchor, check);
  } else {
    for (std::size_t j = 0; j < N; ++j)
      check(j);
  }
}

void QueryShard(const TpchQ13State &st, std::size_t s, const TpchQ13Like &like,
                bool anchored_scan, const IndexPlan *plan,
                std::vector<int32_t> &m, std::vector<uint32_t> &touched,
                std::vector<int64_t> &delta) {
  const TpchQ13CustShard *c = st.cust[s].get();
  const TpchQ13OrderShard *o = st.orders[s].get();
  if (!c || !o)
    return;
  if (m.size() < c->total.size())
    m.resize(c->total.size(), 0);
  if (o->base)
    QuerySeg(o->base.get(), *c, o->alive.data(), like, anchored_scan, plan, m,
             touched);
  QuerySeg(&o->tail, *c, o->alive.data() + o->BaseSize(), like, anchored_scan, plan,
           m, touched);
  for (const uint32_t slot : touched) {
    if (c->mult[slot] > 0) {
      const int64_t mult = c->mult[slot];
      HistAdd(delta, mult * c->total[slot], -1);
      HistAdd(delta, mult * (c->total[slot] - m[slot]), +1);
    }
    m[slot] = 0;
  }
  touched.clear();
}

// Exact answer for a pattern that is not tracked: the base histogram adjusted
// only for customers that own matching orders.  Read-only; shards are
// independent, so slices fill private deltas that are summed (exact integers).
std::vector<TpchQ13OutputRow> EvalUntracked(const TpchQ13State &st,
                                            const std::string &pattern,
                                            const core::AuxEngineServices &services) {
  TpchQ13Like like;
  like.Build(pattern);
  if (like.all_pct) {  // every order matches: all customers have c_count 0
    int64_t cust = 0;
    for (const int64_t v : st.base)
      cust += v;
    std::vector<int64_t> h;
    if (cust > 0)
      h.push_back(cust);
    return SortedRows(h);
  }
  // Candidate orders come from the token index when the pattern has a literal
  // piece that few orders contain; otherwise from the blob (search for the
  // longest literal when the pattern starts and ends with '%', else every
  // order, which an anchored pattern rejects at its first or last bytes).
  IndexPlan plan;
  const bool use_index = PlanIndex(st, like, plan);
  const bool anchored_scan = !like.anchor.empty() && !like.parts.empty() &&
                             like.parts.front().text.empty() &&
                             like.parts.back().text.empty() && like.has_pct;
  const std::size_t slices = core::SliceCount(services, kTpchQ13Shards, kEvalGrain);
  std::vector<std::vector<int64_t>> deltas(slices);
  core::RunSlices(services, slices, kTpchQ13Shards,
           [&](std::size_t slice, std::size_t b, std::size_t e) {
             thread_local std::vector<int32_t> m;
             thread_local std::vector<uint32_t> touched;
             for (std::size_t s = b; s < e; ++s)
               QueryShard(st, s, like, anchored_scan, use_index ? &plan : nullptr,
                          m, touched, deltas[slice]);
           });
  std::vector<int64_t> h = st.base;
  for (const auto &d : deltas) {
    if (d.size() > h.size())
      h.resize(d.size(), 0);
    for (std::size_t i = 0; i < d.size(); ++i)
      h[i] += d[i];
  }
  return SortedRows(h);
}

// Rows of one table kept by a scan partition, bucketed by shard group: O(rows
// seen), never sized by the shard or customer domains. Merging collects the runs
// in table order; the shards are built from them afterwards, one bucket per task.
constexpr std::size_t kBuildBuckets = 1024;
static_assert(kTpchQ13Shards % kBuildBuckets == 0);

std::size_t BuildBucketOf(int64_t custkey) {
  return static_cast<std::size_t>(custkey) & (kBuildBuckets - 1);
}

struct OrderRun {
  std::vector<int64_t> custkey;
  std::vector<uint32_t> len;
  std::string blob;
};

struct BuildRuns {
  struct Run {
    std::vector<OrderRun> orders = std::vector<OrderRun>(kBuildBuckets);
    std::vector<std::vector<int64_t>> customers =
        std::vector<std::vector<int64_t>>(kBuildBuckets);
  };
  std::vector<Run> runs = std::vector<Run>(1);
  int64_t order_count = 0;

  void AddOrder(int64_t custkey, std::string_view comment) {
    if (!KeyOk(custkey))
      RejectKey(Apply::Raw);
    auto &o = runs.back().orders[BuildBucketOf(custkey)];
    o.custkey.push_back(custkey);
    o.len.push_back(static_cast<uint32_t>(comment.size()));
    o.blob.append(comment.data(), comment.size());
    ++order_count;
  }
  void AddCustomer(int64_t custkey) {
    if (!KeyOk(custkey))
      RejectKey(Apply::Raw);
    runs.back().customers[BuildBucketOf(custkey)].push_back(custkey);
  }
  void MergeFrom(BuildRuns &&from) {
    runs.insert(runs.end(), std::make_move_iterator(from.runs.begin()),
                std::make_move_iterator(from.runs.end()));
    order_count += from.order_count;
  }
};

// Appends the bucket's orders of every run (in table order) to their shard tails.
void AppendBucketOrders(Mut &m, BuildRuns &orders, std::size_t bucket) {
  for (auto &run : orders.runs) {
    OrderRun &src = run.orders[bucket];
    std::size_t at = 0;
    for (std::size_t i = 0; i < src.custkey.size(); ++i) {
      TpchQ13OrderShard &o = m.O(static_cast<std::size_t>(src.custkey[i]) & kShardMask);
      TpchQ13OrderSeg &t = o.tail;
      if (t.blob.size() + src.len[i] > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("TPC-H Q13: order comments of one shard exceed 4 GiB");
      t.custkey.push_back(src.custkey[i]);
      t.start.push_back(static_cast<uint32_t>(t.blob.size()));
      t.len.push_back(src.len[i]);
      t.blob.append(src.blob, at, src.len[i]);
      o.alive.push_back(1);
      at += src.len[i];
    }
    src = OrderRun{};
  }
}

// The customer slots of a shard: one per distinct local id among its orders and
// customer rows, in the identity layout when the ids are dense enough.
void LayOutCustomers(TpchQ13CustShard &c, std::vector<uint32_t> &local_ids, std::size_t np) {
  std::sort(local_ids.begin(), local_ids.end());
  local_ids.erase(std::unique(local_ids.begin(), local_ids.end()), local_ids.end());
  const std::size_t slots = local_ids.empty() ? 0 : std::size_t{local_ids.back()} + 1;
  if (slots < 2 * local_ids.size() + kIdentitySlack) {
    c.total.assign(slots, 0);
    c.mult.assign(slots, 0);
  } else {
    c.ids = local_ids;
    c.total.assign(local_ids.size(), 0);
    c.mult.assign(local_ids.size(), 0);
  }
  c.match.assign(np, std::vector<int32_t>(c.total.size(), 0));
}

// Builds the shards of one bucket: orders appended, customer slots laid out and
// the order and customer-row counts filled in.
void BuildBucketShards(Mut &m, const BuildRuns &customers, BuildRuns &orders,
                       std::size_t bucket) {
  AppendBucketOrders(m, orders, bucket);
  const std::size_t np = m.st.patterns.size();
  constexpr std::size_t kPerBucket = kTpchQ13Shards / kBuildBuckets;
  std::array<std::vector<int64_t>, kPerBucket> customer_keys;
  for (const auto &run : customers.runs)
    for (const int64_t ck : run.customers[bucket])
      customer_keys[(static_cast<std::size_t>(ck) & kShardMask) / kBuildBuckets].push_back(ck);
  const auto local_of = [](int64_t ck) {
    return static_cast<uint32_t>(static_cast<std::size_t>(ck) >> kTpchQ13ShardBits);
  };
  for (std::size_t j = 0; j < kPerBucket; ++j) {
    const std::size_t s = bucket + j * kBuildBuckets;
    const TpchQ13OrderShard *o = m.ReadO(s);
    if (!o && customer_keys[j].empty())
      continue;
    std::vector<uint32_t> local_ids;
    if (o)
      for (const int64_t ck : o->tail.custkey)
        local_ids.push_back(local_of(ck));
    for (const int64_t ck : customer_keys[j])
      local_ids.push_back(local_of(ck));
    TpchQ13CustShard &c = m.C(s);
    LayOutCustomers(c, local_ids, np);
    if (o)
      for (const int64_t ck : o->tail.custkey)
        ++c.total[c.SlotOf(local_of(ck))];
    for (const int64_t ck : customer_keys[j])
      ++c.mult[c.SlotOf(local_of(ck))];
  }
}

} // namespace

void TpchQ13Like::Build(const std::string &p) {
  pattern = p;
  parts.clear();
  chunks.clear();
  anchor.clear();
  has_pct = p.find('%') != std::string::npos;
  all_pct = !p.empty() && p.find_first_not_of('%') == std::string::npos;
  std::size_t i = 0;
  for (;;) {
    const std::size_t j = p.find('%', i);
    Seg s;
    s.text = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
    s.wild = s.text.find('_') != std::string::npos;
    parts.push_back(std::move(s));
    if (j == std::string::npos)
      break;
    i = j + 1;
  }
  for (const auto &s : parts) {
    std::size_t a = 0;
    while (a <= s.text.size()) {
      std::size_t b = s.text.find('_', a);
      if (b == std::string::npos)
        b = s.text.size();
      if (b > a)
        chunks.push_back(s.text.substr(a, b - a));
      a = b + 1;
    }
  }
  for (const auto &c : chunks)
    if (c.size() > anchor.size())
      anchor = c;
  single_lit = parts.size() == 3 && parts[0].text.empty() &&
               parts[2].text.empty() && !parts[1].wild &&
               !parts[1].text.empty();
}

bool TpchQ13Like::Matches(const char *data, std::size_t len) const {
  if (!has_pct)
    return len == parts[0].text.size() && SegAt(data, parts[0]);
  const Seg &first = parts.front();
  const Seg &last = parts.back();
  if (first.text.size() > len || !SegAt(data, first))
    return false;
  std::size_t pos = first.text.size();
  if (last.text.size() > len - pos)
    return false;
  const std::size_t end = len - last.text.size();
  if (!SegAt(data + end, last))
    return false;
  for (std::size_t k = 1; k + 1 < parts.size(); ++k) {
    const Seg &s = parts[k];
    const std::size_t m = s.text.size();
    if (m == 0)
      continue;
    if (end - pos < m)
      return false;
    const char *f = SegFind(data + pos, end - pos, s);
    if (!f)
      return false;
    pos = static_cast<std::size_t>(f - data) + m;
  }
  return true;
}

// Layout: header, patterns, customer rows (key, multiplicity), live orders.

bool TpchQ13TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ13TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ13TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ13Delta> &TpchQ13TxnBuffer::Deltas() const { return deltas; }
void TpchQ13TxnBuffer::MarkRebuild() {
  requires_rebuild = true;
  pending = true;
}
void TpchQ13TxnBuffer::AddDelta(TpchQ13Delta delta) {
  pending = true;
  deltas.push_back(std::move(delta));
}

core::AcceleratorId TpchQ13Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ13Module::Dependencies() const {
  return {
      {config.customer_table, {0}, {0}},
      {config.orders_table, {0, 1}, {1, 8}},
  };
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ13Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ13TxnBuffer>();
}

void TpchQ13Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = std::make_shared<TpchQ13State>(BuildState(services));
  builder.Write(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, 0,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts, state)));
  builder.Finish();
}

TpchQ13State TpchQ13Module::BuildState(
    core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q13 requires a typed initial scan source");

  const auto scan_runs = [&](auto &&scan, auto &&add) {
    return core::PartitionedBuild<BuildRuns>(
        services, [] { return BuildRuns{}; }, scan, add,
        [](BuildRuns &into, BuildRuns &&from) { into.MergeFrom(std::move(from)); });
  };
  const auto customers = scan_runs(
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanCustomers(services, parts,
                                    [&](std::size_t p, const TpchQ13CustomerInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ13CustomerInput>{input});
                                    });
      },
      [](BuildRuns &partial, const TpchQ13CustomerInput &input) {
        partial.AddCustomer(input.key);
      });

  struct OrderRow {
    int64_t custkey;
    std::string_view comment;
  };
  auto orders = scan_runs(
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(services, parts,
                                 [&](std::size_t p, int64_t custkey, std::string_view comment) {
                                   const OrderRow row{custkey, comment};
                                   sink(p, core::SingleRowBatch<OrderRow>{row});
                                 });
      },
      [](BuildRuns &partial, const OrderRow &row) {
        partial.AddOrder(row.custkey, row.comment);
      });

  // Every TPC-H-domain pattern is maintained from the start.
  Mut m;
  for (const auto &text : DomainPatterns())
    m.st.patterns.push_back(MakePattern(text));
  m.InitPatterns();
  m.st.live_orders = orders.order_count;
  core::ParallelEach(services, kBuildBuckets, 1, [&](std::size_t bucket) {
    BuildBucketShards(m, customers, orders, bucket);
  });
  BulkFinish(m, services);
  RecomputeHist(m, services);
  return m.Finish();
}

core::InvalidationPlan
TpchQ13Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.customer_table)
    return All();
  if (mutation.table == config.orders_table)
    return All();
  return {};
}

void TpchQ13Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ13TxnBuffer &>(base_buffer);

  if (mutation.table == config.customer_table) {
    auto addc = [&](const core::AuxRow &row, int8_t sign) {
      int64_t custkey = 0;
      if (!core::IntCell(row, 0, custkey)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddDelta({sign, custkey, std::string(), true});
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        addc(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        addc(row, -1);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, nk = 0;
        if (core::IntCell(mutation.old_rows[i], 0, ok) &&
            core::IntCell(mutation.new_rows[i], 0, nk) && ok == nk)
          continue;  // key unchanged: no effect on Q13
        addc(mutation.old_rows[i], -1);
        addc(mutation.new_rows[i], 1);
      }
      break;
    }
    return;
  }
  if (mutation.table != config.orders_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t custkey = 0;
    std::string comment;
    if (!core::IntCell(row, 0, custkey) || !core::StringCell(row, 1, comment)) {
      buffer.MarkRebuild();
      return;
    }
    if (custkey < 0)
      return;
    buffer.AddDelta({sign, custkey, std::move(comment), false});
  };

  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows)
      add(row, 1);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows)
      add(row, -1);
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ13Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ13TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  const auto request_rebuild = [&] {
    core::FinishRebuildAll(outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                                        services.ExpectedQueriesBeforeNextWrite())));
  };

  if (buffer.RequiresRebuild()) {
    request_rebuild();
    return outcome;
  }
  if (buffer.Deltas().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
      buffer.Deltas().size(), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ13Layout::kCustomerCounts});

  bool found = false;
  std::shared_ptr<const TpchQ13State> base;
  core::Version existing_version = 0;

  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ13Layout::kCustomerCounts)
      continue;
    const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
    if (!typed)
      return outcome;
    base = typed->state;
    existing_version = image.version;
    found = true;
    break;
  }
  if (!found)
    return outcome;

  // Copy-on-write: copies the shard pointer tables and clones only the shards
  // touched by the deltas; the published base is never mutated.
  Mut m(*base);

  // Shards are independent: slices own whole shards (updates of one shard
  // keep their arrival order) and tally their count changes apart.
  const auto &deltas = buffer.Deltas();
  const auto shard_of = [&](uint32_t i) {
    return static_cast<std::size_t>(deltas[i].custkey) & kShardMask;
  };
  std::vector<uint32_t> by_shard(deltas.size());
  std::iota(by_shard.begin(), by_shard.end(), 0u);
  std::stable_sort(by_shard.begin(), by_shard.end(),
                   [&](uint32_t l, uint32_t r) { return shard_of(l) < shard_of(r); });
  const auto runs = core::RunStarts(by_shard, [&](uint32_t i) { return shard_of(i); });
  Tally tally;
  core::ParallelReduce(
      services, runs.size() - 1, kMaintGrain, tally,
      [&](Tally &part, std::size_t first, std::size_t last) {
        for (std::size_t r = first; r < last && part.ok; ++r) {
          for (std::size_t k = runs[r]; k < runs[r + 1]; ++k) {
            const auto &delta = deltas[by_shard[k]];
            part.ok = delta.customer
                          ? ApplyCustomer(m, part, delta.custkey, delta.sign, Apply::Live)
                          : ApplyOrder(m, part, delta.custkey, delta.comment,
                                       delta.sign, Apply::Live);
            if (!part.ok)
              break;
          }
        }
      },
      [](Tally &into, Tally &&from) { into.Merge(from); });
  if (!tally.ok) {
    request_rebuild(); // inconsistent delta: drop the working copy
    return outcome;
  }
  m.Absorb(tally);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, existing_version,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts,
                     std::make_shared<const TpchQ13State>(m.Finish()))));
  return outcome;
}

bool TpchQ13Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ13Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  static const std::string kEmpty;
  const std::string *pattern = &kEmpty;
  if (!arguments.values.empty() && !arguments.values[0].is_null)
    if (const auto *p = std::get_if<std::string>(&arguments.values[0].payload))
      pattern = p;

  // One observation: whole-state validity plus the partition's handle.
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = core::TpchQ13Layout::kCustomerCounts;
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), hit_key, wanted);
  if (snapshot.available) {
    for (const auto &image : snapshot.partitions) {
      if (image.partition != core::TpchQ13Layout::kCustomerCounts)
        continue;
      const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
      if (!typed)
        break;
      EmitResult(*typed->state, *pattern, services, sink);
      return;
    }
  }
  ExecuteFallback(services, arguments, sink);
}

bool TpchQ13Module::LoadState(
    core::AuxEngineServices &services,
    std::shared_ptr<const TpchQ13State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ13Layout::kCustomerCounts});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ13Layout::kCustomerCounts)
      continue;
    const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
    if (!typed)
      return false;
    state = typed->state;
    return true;
  }
  return false;
}

void TpchQ13Module::EmitResult(const TpchQ13State &state,
                               const std::string &pattern,
                               core::AuxEngineServices &services,
                               core::AuxRowSink &sink) const {
  // Tracked pattern (all 16 TPC-H ones): the maintained, pre-sorted histogram.
  for (std::size_t p = 0; p < state.patterns.size(); ++p) {
    if (state.patterns[p].text == pattern) {
      EmitSorted(state.sorted[p], sink);
      return;
    }
  }

  // Any other LIKE pattern: exact, in memory, from the base histogram plus the
  // customers that own matching orders.
  const auto rows = EvalUntracked(state, pattern, services);
  EmitSorted(rows, sink);
}

std::vector<core::CalibrationWrite> TpchQ13Module::CalibrationWrites() const {
  return TpchQ13CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
