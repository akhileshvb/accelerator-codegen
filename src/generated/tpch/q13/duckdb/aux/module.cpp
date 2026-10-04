#include "tpch/q13/duckdb/aux/module.hpp"
#include "tpch/q13/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <chrono>
#include <cstdio>
#include "tpch/q13/layout.hpp"

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

// Recursive SQL LIKE matcher. Pattern is kept in original case.
bool SqlLikeAt(const char *text, std::size_t text_len,
               const char *pattern, std::size_t pattern_len,
               std::size_t text_pos, std::size_t pattern_pos) {
  while (pattern_pos < pattern_len) {
    const char pattern_ch = pattern[pattern_pos];
    if (pattern_ch == '%') {
      if (pattern_pos + 1 == pattern_len)
        return true;
      for (std::size_t start = text_pos; start <= text_len; ++start) {
        if (SqlLikeAt(text, text_len, pattern, pattern_len, start, pattern_pos + 1))
          return true;
      }
      return false;
    }
    if (pattern_ch == '_') {
      if (text_pos >= text_len)
        return false;
      ++text_pos;
      ++pattern_pos;
      continue;
    }
    if (text_pos >= text_len || text[text_pos] != pattern_ch)
      return false;
    ++text_pos;
    ++pattern_pos;
  }
  return text_pos == text_len;
}

bool SqlLike(const std::string &text, const std::string &pattern) {
  return SqlLikeAt(text.data(), text.size(), pattern.data(), pattern.size(), 0, 0);
}

bool SqlLikeSlice(const char *text, std::size_t text_len, const std::string &pattern) {
  return SqlLikeAt(text, text_len, pattern.data(), pattern.size(), 0, 0);
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
constexpr const char *kDefaultPattern = "%unusual%accounts%";
constexpr int64_t kMaxKey = int64_t{1} << 36;  // keys outside are ignored
constexpr std::size_t kShardMask = kTpchQ13Shards - 1;

bool KeyOk(int64_t ck) { return ck >= 0 && ck < kMaxKey; }

bool PatMatch(const TpchQ13Pattern &p, const char *d, std::size_t n) {
  if (p.matcher.enabled)
    return p.matcher.MatchesView(d, n);
  return SqlLikeAt(d, n, p.text.data(), p.text.size(), 0, 0);
}

void HistAdd(std::vector<int64_t> &h, int64_t eff, int64_t delta) {
  if (eff < 0)
    eff = 0;
  if (static_cast<std::size_t>(eff) >= h.size())
    h.resize(static_cast<std::size_t>(eff) + 1, 0);
  h[static_cast<std::size_t>(eff)] += delta;
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
    mo.resize(kTpchQ13Shards);
    mc.resize(kTpchQ13Shards);
  }
  explicit Mut(const TpchQ13State &base) : st(base) {
    mo.resize(kTpchQ13Shards);
    mc.resize(kTpchQ13Shards);
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
  TpchQ13State Finish() {
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

void CompactOrders(TpchQ13OrderShard &o) {
  TpchQ13OrderShard n;
  const std::size_t live = o.custkey.size() - o.dead;
  n.custkey.reserve(live);
  n.start.reserve(live);
  n.len.reserve(live);
  n.alive.reserve(live);
  n.blob.reserve(o.blob.size() / 2 + 64);
  for (std::size_t i = 0; i < o.custkey.size(); ++i) {
    if (!o.alive[i])
      continue;
    n.custkey.push_back(o.custkey[i]);
    n.start.push_back(static_cast<uint32_t>(n.blob.size()));
    n.len.push_back(o.len[i]);
    n.blob.append(o.blob, o.start[i], o.len[i]);
    n.alive.push_back(1);
  }
  o = std::move(n);
}

// Applies one order insert (sign>0) or delete (sign<0).  Returns false when a
// delete finds no matching order (caller falls back to a rebuild).
bool ApplyOrder(Mut &m, int64_t ck, std::string_view comment, int sign,
                bool track_hist) {
  if (!KeyOk(ck))
    return true;
  const std::size_t s = static_cast<std::size_t>(ck) & kShardMask;
  const std::size_t lc = static_cast<std::size_t>(ck) >> kTpchQ13ShardBits;
  const std::size_t np = m.st.patterns.size();
  TpchQ13OrderShard &o = m.O(s);
  if (sign > 0) {
    o.custkey.push_back(ck);
    o.start.push_back(static_cast<uint32_t>(o.blob.size()));
    o.len.push_back(static_cast<uint32_t>(comment.size()));
    o.blob.append(comment.data(), comment.size());
    o.alive.push_back(1);
  } else {
    bool found = false;
    for (std::size_t i = o.custkey.size(); i > 0; --i) {
      const std::size_t idx = i - 1;
      if (!o.alive[idx] || o.custkey[idx] != ck || o.len[idx] != comment.size())
        continue;
      if (std::memcmp(o.blob.data() + o.start[idx], comment.data(),
                      comment.size()) != 0)
        continue;
      o.alive[idx] = 0;
      ++o.dead;
      found = true;
      break;
    }
    if (!found)
      return false;
    if (o.dead > 32 && static_cast<std::size_t>(o.dead) * 2 > o.custkey.size())
      CompactOrders(o);
  }
  TpchQ13CustShard &c = m.C(s);
  EnsureCust(c, lc, np);
  const bool valid = c.mult[lc] > 0;
  if (track_hist && valid)
    for (std::size_t p = 0; p < np; ++p)
      HistAdd(m.st.hist[p], int64_t{c.mult[lc]} * (c.total[lc] - c.match[p][lc]), -1);
  c.total[lc] += sign;
  for (std::size_t p = 0; p < np; ++p)
    if (PatMatch(m.st.patterns[p], comment.data(), comment.size()))
      c.match[p][lc] += sign;
  if (track_hist && valid)
    for (std::size_t p = 0; p < np; ++p)
      HistAdd(m.st.hist[p], int64_t{c.mult[lc]} * (c.total[lc] - c.match[p][lc]), +1);
  return true;
}

// Customer row insert (sign>0) / delete (sign<0).  False on delete of a
// non-existent customer.
bool ApplyCustomer(Mut &m, int64_t ck, int sign, bool track_hist) {
  if (!KeyOk(ck))
    return true;
  const std::size_t s = static_cast<std::size_t>(ck) & kShardMask;
  const std::size_t lc = static_cast<std::size_t>(ck) >> kTpchQ13ShardBits;
  const std::size_t np = m.st.patterns.size();
  TpchQ13CustShard &c = m.C(s);
  EnsureCust(c, lc, np);
  if (sign < 0 && c.mult[lc] <= 0)
    return false;
  // c_count = (#customer rows with this key) * (#non-matching orders): the
  // LEFT JOIN multiplies rows for duplicated customer keys.
  if (track_hist && c.mult[lc] > 0)
    for (std::size_t p = 0; p < np; ++p)
      HistAdd(m.st.hist[p], int64_t{c.mult[lc]} * (c.total[lc] - c.match[p][lc]), -1);
  c.mult[lc] += sign;
  if (track_hist && c.mult[lc] > 0)
    for (std::size_t p = 0; p < np; ++p)
      HistAdd(m.st.hist[p], int64_t{c.mult[lc]} * (c.total[lc] - c.match[p][lc]), +1);
  return true;
}

void RecomputeHist(Mut &m) {
  const std::size_t np = m.st.patterns.size();
  m.st.hist.assign(np, {});
  for (std::size_t s = 0; s < kTpchQ13Shards; ++s) {
    const TpchQ13CustShard *c = m.ReadC(s);
    if (!c)
      continue;
    for (std::size_t lc = 0; lc < c->total.size(); ++lc) {
      if (c->mult[lc] <= 0)
        continue;
      for (std::size_t p = 0; p < np; ++p)
        HistAdd(m.st.hist[p],
                int64_t{c->mult[lc]} *
                    (c->total[lc] - (p < c->match.size() ? c->match[p][lc] : 0)),
                1);
    }
  }
}

TpchQ13Pattern MakePattern(const std::string &text) {
  TpchQ13Pattern p;
  p.text = text;
  p.matcher.Build(text);
  return p;
}

// Starts tracking a new pattern on an existing state: one pass over comments
// (rare; done once per pattern), clones only customer shards.
void AddPattern(Mut &m, const std::string &text) {
  const std::size_t idx = m.st.patterns.size();
  m.st.patterns.push_back(MakePattern(text));
  const TpchQ13Pattern &pat = m.st.patterns[idx];
  for (std::size_t s = 0; s < kTpchQ13Shards; ++s) {
    if (!m.ReadC(s))
      continue;
    TpchQ13CustShard &c = m.C(s);
    c.match.resize(idx + 1);
    c.match[idx].assign(c.total.size(), 0);
    const TpchQ13OrderShard *o = m.ReadO(s);
    if (!o)
      continue;
    for (std::size_t i = 0; i < o->custkey.size(); ++i) {
      if (!o->alive[i])
        continue;
      if (PatMatch(pat, o->blob.data() + o->start[i], o->len[i]))
        ++c.match[idx][static_cast<std::size_t>(o->custkey[i]) >>
                       kTpchQ13ShardBits];
    }
  }
  m.st.hist.emplace_back();
  auto &h = m.st.hist[idx];
  for (std::size_t s = 0; s < kTpchQ13Shards; ++s) {
    const TpchQ13CustShard *c = m.ReadC(s);
    if (!c)
      continue;
    for (std::size_t lc = 0; lc < c->total.size(); ++lc)
      if (c->mult[lc] > 0)
        HistAdd(h, int64_t{c->mult[lc]} * (c->total[lc] - c->match[idx][lc]), 1);
  }
}

void EmitHist(const std::vector<int64_t> &h, core::AuxRowSink &sink) {
  std::vector<TpchQ13OutputRow> rows;
  for (std::size_t e = 0; e < h.size(); ++e)
    if (h[e] > 0)
      rows.push_back({static_cast<int64_t>(e), h[e]});
  std::sort(rows.begin(), rows.end(), Q13OutputRowBetter);
  sink.Begin({{{"c_count", core::AuxType::Int64, false},
               {"custdist", core::AuxType::Int64, false}}});
  for (const auto &r : rows)
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, r.c_count),
                    core::AuxValue::Of(core::AuxType::Int64, r.custdist)}});
  sink.End();
}

// Read-only O(orders) evaluation for a pattern the state does not track.
// Allocates only local scratch; never touches shared state.
std::vector<int64_t> ScanHistogram(const TpchQ13State &state,
                                   const std::string &pattern) {
  const TpchQ13Pattern pat = MakePattern(pattern);
  std::vector<int64_t> h;
  std::vector<int32_t> m;
  for (std::size_t s = 0; s < kTpchQ13Shards; ++s) {
    const TpchQ13CustShard *c = state.cust[s].get();
    if (!c)
      continue;
    m.assign(c->total.size(), 0);
    if (const TpchQ13OrderShard *o = state.orders[s].get()) {
      for (std::size_t i = 0; i < o->custkey.size(); ++i) {
        if (!o->alive[i])
          continue;
        if (PatMatch(pat, o->blob.data() + o->start[i], o->len[i]))
          ++m[static_cast<std::size_t>(o->custkey[i]) >> kTpchQ13ShardBits];
      }
    }
    for (std::size_t lc = 0; lc < c->total.size(); ++lc)
      if (c->mult[lc] > 0)
        HistAdd(h, int64_t{c->mult[lc]} * (c->total[lc] - m[lc]), 1);
  }
  return h;
}

} // namespace

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
      live_orders += o->custkey.size() - o->dead;
  core::WriteImageHeader(writer, kCustomerMagic, kVersion,
                         static_cast<uint32_t>(live_orders));

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
    for (std::size_t i = 0; i < o->custkey.size(); ++i) {
      if (!o->alive[i])
        continue;
      writer.I64(o->custkey[i]);
      writer.String(std::string(o->blob, o->start[i], o->len[i]));
    }
  }
  return writer.bytes;
}

bool DecodeTpchQ13CustomerCounts(const std::vector<uint8_t> &bytes,
                                 TpchQ13State &state) {
  ByteReader reader(bytes);
  uint32_t magic = 0, version = 0, record_count = 0;
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
  uint64_t ncust = 0;
  if (!reader.U64(ncust))
    return false;
  for (uint64_t i = 0; i < ncust; ++i) {
    int64_t ck = 0;
    int32_t mult = 0;
    if (!reader.I64(ck) || !reader.I32(mult) || mult < 0)
      return false;
    for (int32_t k = 0; k < mult; ++k)
      ApplyCustomer(m, ck, +1, false);
  }
  uint64_t norders = 0;
  if (!reader.U64(norders))
    return false;
  for (uint64_t i = 0; i < norders; ++i) {
    int64_t ck = 0;
    std::string comment;
    if (!reader.I64(ck) || !reader.String(comment))
      return false;
    ApplyOrder(m, ck, comment, +1, false);
  }
  RecomputeHist(m);
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

void TpchQ13Module::NoteWantedPattern(const std::string &pattern) const {
  std::lock_guard<std::mutex> lock(wanted_mu);
  if (wanted.size() + 1 >= kTpchQ13MaxPatterns)
    return;
  if (pattern == kDefaultPattern ||
      std::find(wanted.begin(), wanted.end(), pattern) != wanted.end())
    return;
  wanted.push_back(pattern);
}

std::vector<std::string> TpchQ13Module::WantedPatterns() const {
  std::lock_guard<std::mutex> lock(wanted_mu);
  return wanted;
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
  (void)services;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q13 requires a typed initial scan source");

  const bool profiling = CustomProfilingEnabled();
  Mut m;
  m.st.patterns.push_back(MakePattern(kDefaultPattern));
  for (const auto &w : WantedPatterns())
    if (m.st.patterns.size() < kTpchQ13MaxPatterns)
      m.st.patterns.push_back(MakePattern(w));

  CustomTimer t_cust(profiling);
  int64_t ncust = 0;
  initial_scan->ScanCustomers([&](const TpchQ13CustomerInput &input) {
    ApplyCustomer(m, input.key, +1, false);
    ++ncust;
  });
  if (profiling)
    EmitCustomProfile("build_customers", t_cust.us(), ncust, 0, 0, 0);

  CustomTimer t_orders(profiling);
  int64_t norders = 0;
  initial_scan->ScanOrders([&](const TpchQ13OrderInput &input) {
    ApplyOrder(m, input.custkey, input.comment, +1, false);
    ++norders;
  });
  if (profiling)
    EmitCustomProfile("build_orders", t_orders.us(), norders, 0, 0, 0);

  RecomputeHist(m);
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
  Mut m(*base);

  // Start tracking patterns that reads have asked for (one pass each, rare).
  for (const auto &w : WantedPatterns()) {
    if (m.st.patterns.size() >= kTpchQ13MaxPatterns)
      break;
    bool known = false;
    for (const auto &p : m.st.patterns)
      known = known || p.text == w;
    if (!known)
      AddPattern(m, w);
  }

  for (const auto &delta : buffer.Deltas()) {
    const bool ok =
        delta.customer
            ? ApplyCustomer(m, delta.custkey, delta.sign, true)
            : ApplyOrder(m, delta.custkey, delta.comment, delta.sign, true);
    if (!ok) {
      request_rebuild();  // inconsistent delta: drop the working copy
      return outcome;
    }
  }

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
  std::string pattern;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    pattern = std::get<std::string>(arguments.values[0].payload);

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::shared_ptr<const TpchQ13State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, pattern, services, sink);
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
  (void)services;
  const bool profiling = CustomProfilingEnabled();
  CustomTimer t(profiling);

  // Tracked pattern: answer is the maintained histogram, O(#distinct counts).
  for (std::size_t p = 0; p < state.patterns.size(); ++p) {
    if (state.patterns[p].text == pattern) {
      EmitHist(state.hist[p], sink);
      if (profiling)
        EmitCustomProfile("hist_emit", t.us(), 0, 0,
                          static_cast<int64_t>(state.hist[p].size()), 0);
      return;
    }
  }

  // Unseen pattern: exact read-only scan now; ask maintenance to track it.
  NoteWantedPattern(pattern);
  const auto h = ScanHistogram(state, pattern);
  EmitHist(h, sink);
  if (profiling)
    EmitCustomProfile("exclude_scan", t.us(), 0, 0,
                      static_cast<int64_t>(h.size()), 0);
}

std::vector<core::CalibrationWrite> TpchQ13Module::CalibrationWrites() const {
  return TpchQ13CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
