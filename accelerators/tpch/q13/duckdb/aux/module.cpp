#include "tpch/q13/duckdb/aux/module.hpp"
#include "tpch/q13/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <chrono>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include <cstdio>
#include "tpch/q13/layout.hpp"
#include <algorithm>
#include <array>
#include <vector>

// ── Custom profiling (no-op unless ACCEL_AUX_CUSTOM_PROFILE is set) ─────────
struct CustomTimer {
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point start;
  bool active;
  explicit CustomTimer(bool enabled) : start(Clock::now()), active(enabled) {}
  double us() const {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start)
            .count()) /
           1000.0;
  }
};

static bool CustomProfilingEnabled() {
  static const bool enabled = []() {
    const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
    return v && v[0] != '\0';
  }();
  return enabled;
}

static void EmitCustomProfile(const char *phase, double us_val, int64_t rows,
                              int64_t keys, int64_t groups, int64_t scanned) {
  if (!CustomProfilingEnabled())
    return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {"
          "\"phase\":\"%s\","
          "\"us\":%.3f,"
          "\"rows\":%ld,"
          "\"keys\":%ld,"
          "\"groups\":%ld,"
          "\"scanned\":%ld"
          "}\n",
          phase, us_val, (long)rows, (long)keys, (long)groups, (long)scanned);
}

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kCustomerMagic = 0x43313351; // Q13C
constexpr uint32_t kVersion = 1;

// ── Cell extraction helpers ─────────────────────────────────────────────────
bool StringCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<std::string>(row.values[index].payload))
    return false;
  out = std::get<std::string>(row.values[index].payload);
  return true;
}

bool IntCell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = std::get<int64_t>(cell.payload);
    return true;
  }
  if (std::holds_alternative<uint64_t>(cell.payload)) {
    const auto value = std::get<uint64_t>(cell.payload);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
    out = static_cast<int64_t>(value);
    return true;
  }
  return false;
}

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



// ── Sharded, copy-on-write state machinery ──────────────────────────────────
constexpr int64_t kMaxKey = int64_t{1} << 36;  // keys outside are ignored
constexpr std::size_t kShardMask = kTpchQ13Shards - 1;

bool KeyOk(int64_t ck) { return ck >= 0 && ck < kMaxKey; }

// ── Literal search ──────────────────────────────────────────────────────────
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

// Shards per slice. A shard holds ~100 orders per SF-1 (about 1us each
// when indexed), so only large builds and reads pay for a task.
constexpr std::size_t kFinishGrain = 32;  // bulk token index and match counts
constexpr std::size_t kHistGrain = 64;     // ~200 customers per shard at SF 20
constexpr std::size_t kEvalGrain = 1024;   // untracked-pattern read
constexpr std::size_t kMaintGrain = 64;    // maintenance: shards touched per slice (~2 us each)

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

// ── Word-token index ────────────────────────────────────────────────────────
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

void EnsureCust(TpchQ13CustShard &c, std::size_t lc, std::size_t np) {
  if (c.match.size() < np)
    c.match.resize(np);
  if (lc >= c.total.size()) {
    c.total.resize(lc + 1, 0);
    c.mult.resize(lc + 1, 0);
  }
  for (auto &m : c.match)
    if (m.size() < c.total.size())
      m.resize(c.total.size(), 0);
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
             std::size_t lc, int64_t delta) {
  const int64_t mult = c.mult[lc];
  if (tally.hist.size() < np)
    tally.hist.resize(np);
  HistAdd(tally.base, mult * c.total[lc], delta);
  for (std::size_t p = 0; p < np; ++p)
    HistAdd(tally.hist[p], mult * (c.total[lc] - c.match[p][lc]), delta);
}

enum class Apply { Live, Raw };

// Applies one order insert (sign>0) or delete (sign<0).  Raw (initial load)
// only stores the order and counts it; matches/histograms come from BulkFinish.
// Returns false when a delete finds no matching order (caller rebuilds).
bool ApplyOrder(Mut &m, Tally &tally, int64_t ck, std::string_view comment,
                int sign, Apply mode) {
  if (!KeyOk(ck))
    return true;
  const std::size_t s = static_cast<std::size_t>(ck) & kShardMask;
  const std::size_t lc = static_cast<std::size_t>(ck) >> kTpchQ13ShardBits;
  const std::size_t np = m.st.patterns.size();
  TpchQ13OrderShard &o = m.O(s);
  tally.live_orders += sign;
  if (sign > 0) {
    TpchQ13OrderSeg &t = o.tail;
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
  EnsureCust(c, lc, np);
  if (mode == Apply::Raw) {
    c.total[lc] += sign;
    return true;
  }
  const bool valid = c.mult[lc] > 0;
  if (valid)
    HistAll(tally, np, c, lc, -1);
  c.total[lc] += sign;
  const uint64_t mask = LitMask(*m.st.lits, comment.data(), comment.size());
  for (std::size_t p = 0; p < np; ++p)
    if (PatMatch(m.st.patterns[p], mask, comment.data(), comment.size()))
      c.match[p][lc] += sign;
  if (valid)
    HistAll(tally, np, c, lc, +1);
  return true;
}

// Customer row insert (sign>0) / delete (sign<0).  False on delete of a
// non-existent customer.
bool ApplyCustomer(Mut &m, Tally &tally, int64_t ck, int sign, Apply mode) {
  if (!KeyOk(ck))
    return true;
  const std::size_t s = static_cast<std::size_t>(ck) & kShardMask;
  const std::size_t lc = static_cast<std::size_t>(ck) >> kTpchQ13ShardBits;
  TpchQ13CustShard &c = m.C(s);
  const std::size_t np = m.st.patterns.size();
  EnsureCust(c, lc, np);
  if (sign < 0 && c.mult[lc] <= 0)
    return false;
  if (mode == Apply::Live && c.mult[lc] > 0)
    HistAll(tally, np, c, lc, -1);
  c.mult[lc] += sign;
  if (mode == Apply::Live && c.mult[lc] > 0)
    HistAll(tally, np, c, lc, +1);
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
                 const std::size_t lc = static_cast<std::size_t>(o->custkey[j]) >>
                                        kTpchQ13ShardBits;
                 for (std::size_t p = 0; p < np; ++p)
                   if (PatMatch(m.st.patterns[p], mask[j], d, o->len[j]))
                     ++c->match[p][lc];
               }
               IndexSeg(*o, os->alive.data(), intern, df_add);
               o->custkey.shrink_to_fit();
               o->start.shrink_to_fit();
               o->len.shrink_to_fit();
               os->alive.shrink_to_fit();
               o->blob.shrink_to_fit();
               MergeTail(*os, df_add);
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
               for (std::size_t lc = 0; lc < c->total.size(); ++lc) {
                 if (c->mult[lc] <= 0)
                   continue;
                 const int64_t mult = c->mult[lc];
                 HistAdd(h[np], mult * c->total[lc], 1);
                 for (std::size_t p = 0; p < np; ++p)
                   HistAdd(h[p],
                           mult * (c->total[lc] -
                                   (p < c->match.size() ? c->match[p][lc] : 0)),
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
void QuerySeg(const TpchQ13OrderSeg *o, const uint8_t *alive,
              const TpchQ13Like &like, bool anchored_scan, const IndexPlan *plan,
              std::vector<int32_t> &m, std::vector<uint32_t> &touched) {
  const char *blob = o->blob.data();
  auto count = [&](std::size_t j) {
    const uint32_t lc = static_cast<uint32_t>(
        static_cast<std::size_t>(o->custkey[j]) >> kTpchQ13ShardBits);
    if (m[lc]++ == 0)
      touched.push_back(lc);
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
    QuerySeg(o->base.get(), o->alive.data(), like, anchored_scan, plan, m,
             touched);
  QuerySeg(&o->tail, o->alive.data() + o->BaseSize(), like, anchored_scan, plan,
           m, touched);
  for (const uint32_t lc : touched) {
    if (c->mult[lc] > 0) {
      const int64_t mult = c->mult[lc];
      HistAdd(delta, mult * c->total[lc], -1);
      HistAdd(delta, mult * (c->total[lc] - m[lc]), +1);
    }
    m[lc] = 0;
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

} // namespace

// ── LIKE matcher ────────────────────────────────────────────────────────────
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


// ── TwoPartLikeMatcher ──────────────────────────────────────────────────────
void TpchQ13TwoPartLikeMatcher::Build(const std::string &pattern) {
  enabled = false;
  part1.clear();
  part2.clear();
  if (pattern.size() < 4 || pattern.front() != '%' || pattern.back() != '%')
    return;
  const std::size_t first_end = pattern.find('%', 1);
  if (first_end == std::string::npos || first_end == 1)
    return;
  const std::size_t second_start = first_end + 1;
  const std::size_t second_end = pattern.find('%', second_start);
  if (second_end == std::string::npos || second_end == second_start)
    return;
  if (pattern.find('%', second_end + 1) != std::string::npos)
    return;
  part1 = pattern.substr(1, first_end - 1);
  part2 = pattern.substr(second_start, second_end - second_start);
  enabled = !part1.empty() && !part2.empty() &&
            part1.find('_') == std::string::npos &&
            part2.find('_') == std::string::npos &&
            part1.find('\\') == std::string::npos &&
            part2.find('\\') == std::string::npos;
}

bool TpchQ13TwoPartLikeMatcher::Matches(const std::string &text) const {
  if (!enabled)
    return false;
  const std::string_view haystack(text.data(), text.size());
  std::size_t pos = 0;
  while (pos + part1.size() <= haystack.size()) {
    const auto found = haystack.find(part1, pos);
    if (found == std::string_view::npos)
      return false;
    if (haystack.find(part2, found + part1.size()) != std::string_view::npos)
      return true;
    pos = found + 1;
  }
  return false;
}

bool TpchQ13TwoPartLikeMatcher::MatchesView(const char *data, std::size_t len) const {
  if (!enabled)
    return false;
  const std::string_view haystack(data, len);
  std::size_t pos = 0;
  while (pos + part1.size() <= haystack.size()) {
    const auto found = haystack.find(part1, pos);
    if (found == std::string_view::npos)
      return false;
    if (haystack.find(part2, found + part1.size()) != std::string_view::npos)
      return true;
    pos = found + 1;
  }
  return false;
}


// ── Encode / Decode ─────────────────────────────────────────────────────────
// Layout: header, patterns, customer rows (key, multiplicity), live orders.
std::vector<uint8_t> EncodeTpchQ13CustomerCounts(const TpchQ13State &state) {
  ByteWriter writer;
  uint64_t live_orders = 0;
  for (const auto &o : state.orders)
    if (o)
      live_orders += o->Size() - o->dead;
  core::WriteImageHeader(writer, kCustomerMagic, kVersion,
                         static_cast<uint64_t>(live_orders));

  writer.U32(static_cast<uint32_t>(state.patterns.size()));
  for (const auto &p : state.patterns)
    writer.String(p.text);

  std::vector<std::pair<int64_t, int32_t>> custs;
  for (std::size_t s = 0; s < state.cust.size(); ++s) {
    const auto &c = state.cust[s];
    if (!c)
      continue;
    for (std::size_t lc = 0; lc < c->mult.size(); ++lc)
      if (c->mult[lc] > 0)
        custs.emplace_back(
            static_cast<int64_t>((lc << kTpchQ13ShardBits) | s), c->mult[lc]);
  }
  writer.U64(custs.size());
  for (const auto &[ck, mult] : custs) {
    writer.I64(ck);
    writer.I32(mult);
  }

  writer.U64(live_orders);
  for (const auto &o : state.orders) {
    if (!o)
      continue;
    std::size_t g = 0;
    auto emit = [&](const TpchQ13OrderSeg &sg) {
      for (std::size_t i = 0; i < sg.custkey.size(); ++i, ++g) {
        if (!o->alive[g])
          continue;
        writer.I64(sg.custkey[i]);
        writer.String(std::string(sg.blob, sg.start[i], sg.len[i]));
      }
    };
    if (o->base)
      emit(*o->base);
    emit(o->tail);
  }
  return writer.bytes;
}

bool DecodeTpchQ13CustomerCounts(const std::vector<uint8_t> &bytes,
                                 TpchQ13State &state,
                                 const core::AuxEngineServices &services) {
  ByteReader reader(bytes);
  uint64_t magic = 0, version = 0, record_count = 0;
  if (!core::ReadImageHeader(reader, magic, version, record_count) ||
      magic != kCustomerMagic)
    return false;
  uint32_t np = 0;
  if (!reader.U32(np) || np > kTpchQ13MaxPatterns)
    return false;
  Mut m;
  for (uint32_t p = 0; p < np; ++p) {
    std::string text;
    if (!reader.String(text))
      return false;
    m.st.patterns.push_back(MakePattern(text));
  }
  m.InitPatterns();
  Tally loaded;
  uint64_t ncust = 0;
  if (!reader.U64(ncust))
    return false;
  for (uint64_t i = 0; i < ncust; ++i) {
    int64_t ck = 0;
    int32_t mult = 0;
    if (!reader.I64(ck) || !reader.I32(mult) || mult < 0)
      return false;
    for (int32_t k = 0; k < mult; ++k)
      ApplyCustomer(m, loaded, ck, +1, Apply::Raw);
  }
  uint64_t norders = 0;
  if (!reader.U64(norders))
    return false;
  for (uint64_t i = 0; i < norders; ++i) {
    int64_t ck = 0;
    std::string comment;
    if (!reader.I64(ck) || !reader.String(comment))
      return false;
    ApplyOrder(m, loaded, ck, comment, +1, Apply::Raw);
  }
  m.Absorb(loaded);
  BulkFinish(m, services);
  RecomputeHist(m, services);
  state = m.Finish();
  return true;
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
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

// ── TpchQ13Module ───────────────────────────────────────────────────────────
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

  const bool profiling = CustomProfilingEnabled();
  Mut m;
  // Every TPC-H-domain pattern is maintained from the start.
  for (const auto &text : DomainPatterns())
    m.st.patterns.push_back(MakePattern(text));
  m.InitPatterns();

  {  // exact per-shard sizes: reserve once, no growth slack or copies
    std::vector<TpchQ13ShardSize> sizes;
    if (initial_scan->ShardSizes(sizes))
      for (const auto &z : sizes) {
        if (z.shard >= kTpchQ13Shards)
          continue;
        if (z.orders > 0) {
          auto &o = m.O(z.shard);
          o.tail.custkey.reserve(z.orders);
          o.tail.start.reserve(z.orders);
          o.tail.len.reserve(z.orders);
          o.alive.reserve(z.orders);
          o.tail.blob.reserve(z.comment_bytes);
        }
        auto &c = m.C(z.shard);
        const std::size_t slots = std::max<uint64_t>(z.cust_slots, 1);
        c.total.reserve(slots);
        c.mult.reserve(slots);
        c.match.resize(m.st.patterns.size());
        for (auto &v : c.match)
          v.reserve(slots);
      }
  }

  CustomTimer t_cust(profiling);
  Tally loaded;
  int64_t ncust = 0;
  initial_scan->ScanCustomers([&](const TpchQ13CustomerInput &input) {
    ApplyCustomer(m, loaded, input.key, +1, Apply::Raw);
    ++ncust;
  });
  if (profiling)
    EmitCustomProfile("build_customers", t_cust.us(), ncust, 0, 0, 0);

  CustomTimer t_orders(profiling);
  int64_t norders = 0;
  initial_scan->ScanOrdersView([&](int64_t custkey, const char *data,
                                   std::size_t len) {
    ApplyOrder(m, loaded, custkey, std::string_view(data, len), +1, Apply::Raw);
    ++norders;
  });
  if (profiling)
    EmitCustomProfile("build_orders", t_orders.us(), norders, 0, 0, 0);

  m.Absorb(loaded);
  CustomTimer t_fin(profiling);
  BulkFinish(m, services);
  RecomputeHist(m, services);
  if (profiling)
    EmitCustomProfile("build_finish", t_fin.us(), 0, 0, 0, 0);
  auto state = m.Finish();
  if (profiling) {
    uint64_t blob = 0, ids = 0, cust = 0, post = 0, tokoff = 0, matchb = 0;
    for (const auto &o : state.orders)
      if (o && o->base) {
        const auto &b = *o->base;
        blob += b.blob.capacity();
        ids += b.custkey.capacity() * 8 + b.start.capacity() * 4 +
               b.len.capacity() * 4 + o->alive.capacity();
        post += b.ix.ords.capacity() * 2 + b.ix.always.capacity() * 2;
        tokoff += b.ix.toks.capacity() * 2 + b.ix.off.capacity() * 4;
      }
    for (const auto &c : state.cust)
      if (c) {
        cust += (c->total.capacity() + c->mult.capacity()) * 4;
        for (const auto &v : c->match)
          matchb += v.capacity() * 4;
      }
    fprintf(stderr,
            "ACCEL_CUSTOM_PROFILE {\"phase\":\"memory_mb\",\"comment_blob\":%.1f,"
            "\"order_arrays\":%.1f,\"postings\":%.1f,\"index_toks_offs\":%.1f,"
            "\"cust_total_mult\":%.1f,\"cust_match16\":%.1f,\"vocab\":%zu}\n",
            blob / 1048576.0, ids / 1048576.0, post / 1048576.0,
            tokoff / 1048576.0, cust / 1048576.0, matchb / 1048576.0,
            state.vocab ? state.vocab->toks.size() : size_t{0});
  }
  return state;
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
      if (!IntCell(row, 0, custkey)) {
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
        if (IntCell(mutation.old_rows[i], 0, ok) &&
            IntCell(mutation.new_rows[i], 0, nk) && ok == nk)
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
    if (!IntCell(row, 0, custkey) || !StringCell(row, 1, comment)) {
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

  auto request_rebuild = [&]() {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.updates.clear();
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
  };

  if (buffer.RequiresRebuild()) {
    request_rebuild();
    return outcome;
  }
  if (buffer.Deltas().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
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
  CustomTimer t_apply(CustomProfilingEnabled());
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
  std::vector<std::pair<std::size_t, std::size_t>> runs; // by_shard ranges
  for (std::size_t i = 0; i < by_shard.size();) {
    std::size_t j = i + 1;
    while (j < by_shard.size() && shard_of(by_shard[j]) == shard_of(by_shard[i]))
      ++j;
    runs.emplace_back(i, j);
    i = j;
  }
  Tally tally;
  core::ParallelReduce(
      services, runs.size(), kMaintGrain, tally,
      [&](Tally &part, std::size_t first, std::size_t last) {
        for (std::size_t r = first; r < last && part.ok; ++r) {
          for (std::size_t k = runs[r].first; k < runs[r].second; ++k) {
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

  const double apply_us = t_apply.us();
  CustomTimer t_fin(CustomProfilingEnabled());
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, existing_version,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts,
                     std::make_shared<const TpchQ13State>(m.Finish()))));
  EmitCustomProfile("maint_apply", apply_us,
                    static_cast<int64_t>(buffer.Deltas().size()), 0, 0, 0);
  EmitCustomProfile("maint_finish", t_fin.us(), 0, 0, 0, 0);
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

void TpchQ13Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  std::string pattern;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    pattern = std::get<std::string>(arguments.values[0].payload);

  const auto state = BuildState(services);
  EmitResult(state, pattern, services, sink);
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
  const bool profiling = CustomProfilingEnabled();
  CustomTimer t(profiling);

  // Tracked pattern (all 16 TPC-H ones): the maintained, pre-sorted histogram.
  for (std::size_t p = 0; p < state.patterns.size(); ++p) {
    if (state.patterns[p].text == pattern) {
      EmitSorted(state.sorted[p], sink);
      if (profiling)
        EmitCustomProfile("hist_emit", t.us(), 0, 0,
                          static_cast<int64_t>(state.hist[p].size()), 0);
      return;
    }
  }

  // Any other LIKE pattern: exact, in memory, from the base histogram plus the
  // customers that own matching orders.
  const auto rows = EvalUntracked(state, pattern, services);
  EmitSorted(rows, sink);
  if (profiling)
    EmitCustomProfile("untracked_eval", t.us(), 0, 0,
                      static_cast<int64_t>(rows.size()), 0);
}

std::vector<core::CalibrationWrite> TpchQ13Module::CalibrationWrites() const {
  return TpchQ13CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
