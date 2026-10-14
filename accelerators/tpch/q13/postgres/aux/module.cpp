#include "tpch/q13/postgres/aux/module.hpp"
#include "tpch/q13/calibration_writes.hpp"
#include "prims/sql_like.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tpch/q13/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

namespace like = core::sql_like;

// LIKE semantics of the target DBMS. PostgreSQL LIKE is case-sensitive; the
// MySQL copy of this file sets this to true (default utf8mb4 collations are
// case-insensitive). This is the only intentional difference between the
// postgres/ and mysql/ copies. '_' matches one UTF-8 character in both.
constexpr bool kLikeCaseInsensitive = false;

constexpr std::size_t kSlotBits = 12; // log2(kTpchQ13SegmentRows)
static_assert(kTpchQ13SegmentRows == (std::size_t{1} << kSlotBits));

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

} // namespace

// ── Known patterns ─────────────────────────────────────────────────────────
namespace {
constexpr std::string_view kWord1[4] = {"special", "pending", "unusual", "express"};
constexpr std::string_view kWord2[4] = {"packages", "requests", "accounts", "deposits"};
} // namespace

int TpchQ13State::PatternIndex(std::string_view pattern) {
  // '%' w1 '%' w2 '%' exactly (no other wildcard / escape characters).
  if (pattern.size() < 3 || pattern.front() != '%' || pattern.back() != '%')
    return -1;
  for (int i = 0; i < 4; ++i) {
    const auto w1 = kWord1[i];
    if (pattern.size() < w1.size() + 3 || pattern.substr(1, w1.size()) != w1 ||
        pattern[1 + w1.size()] != '%')
      continue;
    const auto rest = pattern.substr(2 + w1.size(), pattern.size() - 3 - w1.size());
    for (int j = 0; j < 4; ++j)
      if (rest == kWord2[j])
        return i * 4 + j;
  }
  return -1;
}

// s LIKE '%a%b%' iff some occurrence of a ends at or before the start of some
// occurrence of b, i.e. (end of the first a) <= (start of the last b). Byte
// comparison is exact for PostgreSQL's case-sensitive LIKE on UTF-8 / single
// byte encodings (the words are ASCII).
namespace {
// First position >= from of `w` in buf[0, n), or -1. buf must be readable (and
// hold no letters) for 32 bytes past n. SSE2: a 16-position block is a
// candidate where both of w's first two bytes line up; candidates are checked
// with memcmp.
inline int64_t FindWord(const char *buf, std::size_t n, std::size_t from,
                        std::string_view w) {
#if defined(__SSE2__)
  const __m128i c0 = _mm_set1_epi8(w[0]);
  const __m128i c1 = _mm_set1_epi8(w[1]);
  for (std::size_t k = from; k + w.size() <= n; k += 16) {
    const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(buf + k));
    const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(buf + k + 1));
    uint32_t bits = static_cast<uint32_t>(_mm_movemask_epi8(
        _mm_and_si128(_mm_cmpeq_epi8(b0, c0), _mm_cmpeq_epi8(b1, c1))));
    while (bits) {
      const std::size_t pos = k + static_cast<std::size_t>(__builtin_ctz(bits));
      bits &= bits - 1;
      if (pos + w.size() > n)
        return -1;
      if (std::memcmp(buf + pos + 2, w.data() + 2, w.size() - 2) == 0)
        return static_cast<int64_t>(pos);
    }
  }
  return -1;
#else
  if (from > n)
    return -1;
  const void *hit = memmem(buf + from, n - from, w.data(), w.size());
  return hit ? static_cast<const char *>(hit) - buf : -1;
#endif
}
} // namespace

uint32_t TpchQ13State::PatternMask(std::string_view comment) {
  static_assert(!kLikeCaseInsensitive);
  // Most comments contain no word1: 4 short SIMD scans. For each word1 found,
  // the end of its first occurrence is the best start for a word2.
  constexpr std::size_t kInline = 224;
  char local[kInline + 32];
  std::string padded;
  const char *d;
  const std::size_t n = comment.size();
  if (n <= kInline) {
    std::memcpy(local, comment.data(), n);
    std::memset(local + n, 0, 32);
    d = local;
  } else {
    padded.assign(comment.data(), n);
    padded.append(32, '\0');
    d = padded.data();
  }
  uint32_t mask = 0;
  for (int i = 0; i < 4; ++i) {
    const int64_t pos = FindWord(d, n, 0, kWord1[i]);
    if (pos < 0)
      continue;
    const auto end = static_cast<std::size_t>(pos) + kWord1[i].size();
    for (int j = 0; j < 4; ++j)
      if (FindWord(d, n, end, kWord2[j]) >= 0)
        mask |= uint32_t{1} << (i * 4 + j);
  }
  return mask;
}

void TpchQ13PatternHist::Add(int64_t c_count, int64_t delta) {
  if (c_count < kDenseLimit) {
    const auto i = static_cast<std::size_t>(c_count);
    if (dense.size() <= i)
      dense.resize(i + 1, 0);
    dense[i] += delta;
    return;
  }
  auto &v = big[c_count];
  v += delta;
  if (v == 0)
    big.erase(c_count);
}

TpchQ13State::PatternCounts TpchQ13State::Matched(int64_t key) const {
  if (Dense(key))
    return matched.Get(key);
  const PatternCounts *m = sparse_matched.Find(key);
  return m ? *m : PatternCounts{};
}

// Adds (sign = +1) / removes (-1) customer `custkey` to / from every pattern
// histogram at its current effective count total - matched[p].
void TpchQ13State::HistAdd(int64_t custkey, int64_t sign) {
  const int64_t total = Count(custkey);
  const PatternCounts m = Matched(custkey);
  for (std::size_t p = 0; p < kTpchQ13Patterns; ++p)
    hist[p].Add(total - m[p], sign);
}

void TpchQ13State::DisablePatternIndex() {
  patterns_ready = false;
  matched.Clear();
  sparse_matched = {};
  hist = {};
}

// An order delta of `sign` for `custkey` whose comment matches the patterns in
// `mask`: c_count changes by `sign` exactly for the patterns NOT in `mask`.
void TpchQ13State::ApplyOrderCount(int64_t custkey, int32_t sign, uint32_t mask) {
  if (!patterns_ready) {
    AddCount(custkey, sign);
    return;
  }
  const int64_t total = Count(custkey);
  AddCount(custkey, sign);
  PatternCounts m = Matched(custkey);
  if (mask != 0) {
    PatternCounts next = m;
    for (uint32_t b = mask; b; b &= b - 1) {
      const int p = __builtin_ctz(b);
      const int v = static_cast<int>(next[p]) + sign;
      if (v < 0 || v > 255) {
        DisablePatternIndex();
        return;
      }
      next[p] = static_cast<uint8_t>(v);
    }
    (Dense(custkey) ? matched.Mutable(custkey) : sparse_matched[custkey]) = next;
  }
  if (!Valid(custkey))
    return;
  for (std::size_t p = 0; p < kTpchQ13Patterns; ++p) {
    if (mask >> p & 1)
      continue;
    const int64_t eff = total - m[p];
    hist[p].Add(eff, -1);
    hist[p].Add(eff + sign, +1);
  }
}

void TpchQ13State::EnablePatternIndex() {
  DisablePatternIndex();
  // 1. Match masks of every order, segments in parallel.
  const std::size_t nseg = segments.size();
  std::vector<std::vector<uint16_t>> masks(nseg);
  {
    constexpr std::size_t kBlock = 16;
    unsigned threads = 1;
    if (nseg >= 64)
      threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
      for (;;) {
        const std::size_t lo = next.fetch_add(kBlock);
        if (lo >= nseg)
          return;
        for (std::size_t s = lo; s < std::min(nseg, lo + kBlock); ++s) {
          const auto *seg = segments[s].get();
          if (!seg || seg->live == 0)
            continue;
          auto &out = masks[s];
          out.assign(seg->orderkey.size(), 0);
          for (std::size_t i = 0; i < out.size(); ++i)
            if (seg->alive[i])
              out[i] = static_cast<uint16_t>(PatternMask(seg->comment.View(i)));
        }
      }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t)
      pool.emplace_back(worker);
    worker();
    for (auto &th : pool)
      th.join();
  }
  // 2. Per-customer match counts.
  for (std::size_t s = 0; s < nseg; ++s) {
    const auto &mk = masks[s];
    for (std::size_t i = 0; i < mk.size(); ++i) {
      if (!mk[i])
        continue;
      const int64_t custkey = segments[s]->custkey[i];
      PatternCounts &m = Dense(custkey) ? matched.Mutable(custkey)
                                        : sparse_matched[custkey];
      for (uint32_t b = mk[i]; b; b &= b - 1) {
        const int p = __builtin_ctz(b);
        if (m[p] == 255) {
          DisablePatternIndex();
          return;
        }
        ++m[p];
      }
    }
    std::vector<uint16_t>().swap(masks[s]);
  }
  // 3. Histograms over the valid customers.
  using ValidArray = core::ChunkedCowArray<uint8_t>;
  for (std::size_t ci = 0; ci < customer_valid.NumChunks(); ++ci) {
    const auto *valid = customer_valid.ChunkAt(ci);
    if (!valid)
      continue;
    const int64_t base = static_cast<int64_t>(ci * ValidArray::kChunkSize);
    for (std::size_t j = 0; j < valid->size(); ++j)
      if ((*valid)[j])
        HistAdd(base + static_cast<int64_t>(j), +1);
  }
  sparse_customers.ForEach([&](int64_t key, const SparseSlot &slot) {
    if (slot.valid)
      HistAdd(key, +1);
  });
  patterns_ready = true;
}

// ── TpchQ13State implementation ─────────────────────────────────────────────
int32_t TpchQ13State::Count(int64_t key) const {
  if (Dense(key))
    return order_count.Get(key);
  const SparseSlot *s = sparse_customers.Find(key);
  return s ? s->count : 0;
}
bool TpchQ13State::Valid(int64_t key) const {
  if (Dense(key))
    return customer_valid.Get(key) != 0;
  const SparseSlot *s = sparse_customers.Find(key);
  return s && s->valid;
}
void TpchQ13State::AddCount(int64_t key, int32_t delta) {
  if (Dense(key))
    order_count.Mutable(key) += delta;
  else
    sparse_customers[key].count += delta;
}
void TpchQ13State::SetValid(int64_t key, bool valid) {
  if (Dense(key))
    customer_valid.Set(key, valid ? 1 : 0);
  else
    sparse_customers[key].valid = valid ? 1 : 0;
}

void TpchQ13State::AddCustomer(int64_t custkey) {
  if (Valid(custkey))
    return;
  SetValid(custkey, true);
  ++customers;
  if (patterns_ready)
    HistAdd(custkey, +1);
}

void TpchQ13State::RemoveCustomer(int64_t custkey) {
  if (!Valid(custkey))
    return;
  if (patterns_ready)
    HistAdd(custkey, -1);
  SetValid(custkey, false);
  --customers;
}

std::shared_ptr<TpchQ13OrderSegment> &TpchQ13State::MutableSegment(uint32_t id) {
  auto &segment = segments[id];
  if (segment.use_count() > 1)
    segment = std::make_shared<TpchQ13OrderSegment>(*segment);
  return segment;
}

// Orders count toward their custkey whether or not that customer exists yet
// (the join only sees a customer row), so customer and order maintenance are
// independent and a customer insert/delete is a single flag flip.
void TpchQ13State::AddOrder(int64_t orderkey, int64_t custkey,
                            std::string_view comment) {
  RemoveOrder(orderkey); // primary key: re-adding an existing key replaces it
  if (open_segment < 0 ||
      segments[static_cast<std::size_t>(open_segment)]->orderkey.size() >=
          kTpchQ13SegmentRows) {
    if (!free_segments.empty()) {
      open_segment = free_segments.back();
      free_segments.pop_back();
    } else {
      open_segment = static_cast<int64_t>(segments.size());
      segments.emplace_back();
    }
    auto &segment = segments[static_cast<std::size_t>(open_segment)];
    segment = std::make_shared<TpchQ13OrderSegment>();
    segment->orderkey.reserve(kTpchQ13SegmentRows);
    segment->custkey.reserve(kTpchQ13SegmentRows);
    segment->alive.reserve(kTpchQ13SegmentRows);
    segment->comment.Reserve(kTpchQ13SegmentRows, 0);
  }
  const auto id = static_cast<uint32_t>(open_segment);
  auto &segment = *MutableSegment(id);
  const std::size_t slot = segment.orderkey.size();
  segment.orderkey.push_back(orderkey);
  segment.custkey.push_back(custkey);
  segment.alive.push_back(1);
  segment.comment.Append(comment);
  ++segment.live;
  order_row.Upsert(orderkey, (static_cast<uint64_t>(id) << kSlotBits) | slot);
  ApplyOrderCount(custkey, 1, patterns_ready ? PatternMask(comment) : 0);
  ++live_orders;
}

void TpchQ13State::RemoveOrder(int64_t orderkey) {
  const uint64_t *found = order_row.Find(orderkey);
  if (!found)
    return;
  const auto id = static_cast<uint32_t>(*found >> kSlotBits);
  const std::size_t slot = *found & (kTpchQ13SegmentRows - 1);
  auto &segment = *MutableSegment(id);
  segment.alive[slot] = 0;
  --segment.live;
  ApplyOrderCount(segment.custkey[slot], -1,
                  patterns_ready ? PatternMask(segment.comment.View(slot)) : 0);
  order_row.Erase(orderkey);
  --live_orders;
  touched_.push_back(id);
}

// A segment that is mostly dead is rewritten without its dead rows (moved rows
// get new index entries); an empty one is released for reuse. Cost O(segment).
void TpchQ13State::CompactSegment(uint32_t id) {
  auto &slot_ptr = segments[id];
  if (!slot_ptr)
    return;
  const auto &old = *slot_ptr;
  const std::size_t rows = old.orderkey.size();
  if (old.live == 0) {
    slot_ptr.reset();
    free_segments.push_back(id);
    if (open_segment == static_cast<int64_t>(id))
      open_segment = -1;
    return;
  }
  if ((rows - old.live) * 2 <= rows)
    return;
  auto fresh = std::make_shared<TpchQ13OrderSegment>();
  fresh->orderkey.reserve(old.live);
  fresh->custkey.reserve(old.live);
  fresh->alive.reserve(old.live);
  fresh->comment.Reserve(old.live, old.comment.bytes());
  for (std::size_t i = 0; i < rows; ++i) {
    if (!old.alive[i])
      continue;
    const std::size_t slot = fresh->orderkey.size();
    fresh->orderkey.push_back(old.orderkey[i]);
    fresh->custkey.push_back(old.custkey[i]);
    fresh->alive.push_back(1);
    fresh->comment.Append(old.comment.View(i));
    if (slot != i)
      order_row.Upsert(old.orderkey[i],
                       (static_cast<uint64_t>(id) << kSlotBits) | slot);
  }
  fresh->live = old.live;
  slot_ptr = std::move(fresh);
}

void TpchQ13State::CompactTouched() {
  std::sort(touched_.begin(), touched_.end());
  touched_.erase(std::unique(touched_.begin(), touched_.end()), touched_.end());
  for (const uint32_t id : touched_)
    CompactSegment(id);
  touched_.clear();
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ13TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ13TxnBuffer::Clear() {
  pending = false;
  deltas.clear();
}
const std::vector<TpchQ13Delta> &TpchQ13TxnBuffer::Deltas() const { return deltas; }
void TpchQ13TxnBuffer::AddDelta(TpchQ13Delta delta) {
  pending = true;
  deltas.push_back(std::move(delta));
}

// ── TpchQ13Module ───────────────────────────────────────────────────────────
core::AcceleratorId TpchQ13Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ13Module::Dependencies() const {
  // orders projection: o_orderkey, o_custkey, o_comment.
  return {
      {config.customer_table, {0}, {0}},
      {config.orders_table, {0, 1, 2}, {0, 1, 8}},
  };
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ13Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ13TxnBuffer>();
}

void TpchQ13Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services, /*pattern_index=*/true);
  builder.Write(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, 0,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts, state)));
  builder.Finish();
}

std::shared_ptr<TpchQ13State>
TpchQ13Module::BuildState(core::AuxEngineServices &services,
                          bool pattern_index) const {
  (void)services;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q13 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ13State>();
  initial_scan->ScanCustomers([&](const TpchQ13CustomerInput &input) {
    state->AddCustomer(input.key);
  });
  // Rows stream straight into the segments: no transient copy of the orders.
  initial_scan->ScanOrders([&](const TpchQ13OrderInput &input) {
    state->AddOrder(input.orderkey, input.custkey, input.comment);
  });
  if (pattern_index)
    state->EnablePatternIndex();
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

  auto add_customer = [&](const core::AuxRow &row, int8_t sign) {
    int64_t custkey = 0;
    if (!IntCell(row, 0, custkey))
      return; // a NULL key never joins
    TpchQ13Delta delta;
    delta.table = TpchQ13Delta::Table::Customer;
    delta.sign = sign;
    delta.orderkey = custkey;
    buffer.AddDelta(std::move(delta));
  };
  // An order with a NULL orderkey/custkey/comment never contributes to any
  // count (the join predicate is not true), so it is not tracked at all.
  auto add_order = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ13Delta delta;
    delta.sign = sign;
    if (!IntCell(row, 0, delta.orderkey) || !IntCell(row, 1, delta.custkey) ||
        !StringCell(row, 2, delta.comment))
      return;
    buffer.AddDelta(std::move(delta));
  };

  const bool is_customer = mutation.table == config.customer_table;
  if (!is_customer && mutation.table != config.orders_table)
    return;
  auto apply = [&](const core::AuxRow &row, int8_t sign) {
    if (is_customer)
      add_customer(row, sign);
    else
      add_order(row, sign);
  };

  // An update is "all old rows removed, then all new rows added": identical to
  // pairwise handling, and immune to key swaps within one statement.
  if (mutation.kind != core::MutationKind::Insert)
    for (const auto &row : mutation.old_rows)
      apply(row, -1);
  if (mutation.kind != core::MutationKind::Delete)
    for (const auto &row : mutation.new_rows)
      apply(row, 1);
}

core::StagedMaintenanceOutcome
TpchQ13Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ13TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

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

  std::shared_ptr<TpchQ13State> updated_state;
  core::Version existing_version = 0;
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ13Layout::kCustomerCounts)
      continue;
    const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
    if (!typed)
      return outcome;
    // Copy-on-write: shares every segment / chunk / index shard with the
    // published state; only the ones touched below are cloned.
    updated_state = std::make_shared<TpchQ13State>(*typed->state);
    existing_version = image.version;
    break;
  }
  if (!updated_state)
    return outcome;

  for (const auto &delta : buffer.Deltas()) {
    if (delta.table == TpchQ13Delta::Table::Customer) {
      if (delta.sign > 0)
        updated_state->AddCustomer(delta.orderkey);
      else
        updated_state->RemoveCustomer(delta.orderkey);
    } else if (delta.sign > 0) {
      updated_state->AddOrder(delta.orderkey, delta.custkey, delta.comment);
    } else {
      updated_state->RemoveOrder(delta.orderkey);
    }
  }
  updated_state->CompactTouched();

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, existing_version,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts,
                     std::static_pointer_cast<const TpchQ13State>(updated_state))));
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
  EmitResult(*state, arguments, sink);
}

void TpchQ13Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  // One-shot state: the pattern index would not pay for itself.
  const auto state = BuildState(services, /*pattern_index=*/false);
  EmitResult(*state, arguments, sink);
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

namespace {

// excluded[custkey] = number of the customer's orders whose comment matches
// `matcher`. Segments are handed out in blocks to worker threads; each segment
// is matched with one SIMD pass over its comment blob.
// Matches are counted in a flat array when the custkey range is dense enough,
// otherwise (and for negative / huge keys) in a mutex-protected hash.
struct Excluded {
  std::vector<int32_t> flat;
  std::mutex mu;
  std::unordered_map<int64_t, int32_t> hash;
  void Add(int64_t key) {
    if (key >= 0 && static_cast<std::size_t>(key) < flat.size()) {
      __atomic_fetch_add(&flat[static_cast<std::size_t>(key)], 1, __ATOMIC_RELAXED);
      return;
    }
    std::lock_guard<std::mutex> lock(mu);
    ++hash[key];
  }
  int32_t Get(int64_t key) const {
    if (key >= 0 && static_cast<std::size_t>(key) < flat.size())
      return flat[static_cast<std::size_t>(key)];
    const auto it = hash.find(key);
    return it == hash.end() ? 0 : it->second;
  }
};

void CountMatchingOrders(const TpchQ13State &state,
                         const like::LikeMatcher &matcher, Excluded &excluded) {
  const std::size_t nseg = state.segments.size();
  constexpr std::size_t kBlock = 16;
  unsigned threads = 1;
  if (nseg >= 64)
    threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));

  std::atomic<std::size_t> next{0};
  auto worker = [&] {
    for (;;) {
      const std::size_t lo = next.fetch_add(kBlock);
      if (lo >= nseg)
        return;
      for (std::size_t s = lo; s < std::min(nseg, lo + kBlock); ++s) {
        const auto *seg = state.segments[s].get();
        if (!seg || seg->live == 0)
          continue;
        const std::size_t n = seg->orderkey.size();
        const auto bits = like::MatchBlobBitmap(
            matcher, seg->comment.offsets(), n, seg->comment.data(), 1);
        for (std::size_t w = 0; w < bits.size(); ++w) {
          uint64_t b = bits[w];
          while (b) {
            const std::size_t i = w * 64 + static_cast<std::size_t>(__builtin_ctzll(b));
            b &= b - 1;
            if (seg->alive[i])
              excluded.Add(seg->custkey[i]);
          }
        }
      }
    }
  };
  if (threads == 1) {
    worker();
    return;
  }
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t)
    pool.emplace_back(worker);
  for (auto &th : pool)
    th.join();
}

} // namespace

void TpchQ13Module::EmitResult(const TpchQ13State &state,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) const {
  const bool pattern_null =
      arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload);

  auto emit = [&](std::vector<TpchQ13OutputRow> &rows) {
    std::sort(rows.begin(), rows.end(), Q13OutputRowBetter);
    sink.Begin({{{"c_count", core::AuxType::Int64, false},
                 {"custdist", core::AuxType::Int64, false}}});
    for (const auto &r : rows)
      sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, r.c_count),
                      core::AuxValue::Of(core::AuxType::Int64, r.custdist)}});
    sink.End();
  };

  // Fast paths, O(result rows): a NULL pattern (every customer has c_count 0)
  // and the known patterns (maintained histograms).
  if (pattern_null) {
    std::vector<TpchQ13OutputRow> rows;
    if (state.customers > 0)
      rows.push_back({0, state.customers});
    emit(rows);
    return;
  }
  if (state.patterns_ready) {
    const int p = TpchQ13State::PatternIndex(
        std::get<std::string>(arguments.values[0].payload));
    if (p >= 0) {
      const auto &h = state.hist[static_cast<std::size_t>(p)];
      std::vector<TpchQ13OutputRow> rows;
      for (std::size_t c = 0; c < h.dense.size(); ++c)
        if (h.dense[c] > 0)
          rows.push_back({static_cast<int64_t>(c), h.dense[c]});
      for (const auto &[c, n] : h.big)
        if (n > 0)
          rows.push_back({c, n});
      emit(rows);
      return;
    }
  }

  using Array = core::ChunkedCowArray<int32_t>;
  const std::size_t span =
      std::max(state.order_count.NumChunks(), state.customer_valid.NumChunks()) *
      Array::kChunkSize;

  // `o_comment NOT LIKE NULL` is never true: no order joins, every customer has
  // c_count 0. Otherwise count the matching orders per customer.
  Excluded excluded;
  if (!pattern_null) {
    const auto matcher = like::LikeMatcher::Compile(
        std::get<std::string>(arguments.values[0].payload), '\\',
        kLikeCaseInsensitive, /*utf8_underscore=*/true);
    if (span <= static_cast<std::size_t>(state.customers) * 4 + (std::size_t{1} << 20))
      excluded.flat.assign(span, 0);
    CountMatchingOrders(state, matcher, excluded);
  }

  // Histogram of effective counts. Counts up to kDense use a flat array; the
  // (never expected) larger ones fall into a sparse map.
  constexpr int64_t kDense = int64_t{1} << 22;
  int64_t max_count = 0;
  for (std::size_t ci = 0; ci < state.order_count.NumChunks(); ++ci)
    if (const auto *chunk = state.order_count.ChunkAt(ci))
      for (const int32_t c : *chunk)
        max_count = std::max<int64_t>(max_count, c);
  state.sparse_customers.ForEach([&](int64_t, const TpchQ13State::SparseSlot &slot) {
    max_count = std::max<int64_t>(max_count, slot.count);
  });
  std::vector<int64_t> dense(static_cast<std::size_t>(std::min(max_count, kDense)) + 1, 0);
  std::map<int64_t, int64_t> sparse;

  for (std::size_t ci = 0; ci < state.customer_valid.NumChunks(); ++ci) {
    const auto *valid = state.customer_valid.ChunkAt(ci);
    if (!valid)
      continue;
    const auto *counts = state.order_count.ChunkAt(ci);
    const std::size_t base = ci * Array::kChunkSize;
    for (std::size_t j = 0; j < valid->size(); ++j) {
      if (!(*valid)[j])
        continue;
      int64_t eff = pattern_null ? 0 : (counts ? (*counts)[j] : 0);
      if (!pattern_null)
        eff -= excluded.Get(static_cast<int64_t>(base + j));
      if (eff <= kDense)
        ++dense[static_cast<std::size_t>(eff)];
      else
        ++sparse[eff];
    }
  }
  state.sparse_customers.ForEach([&](int64_t key, const TpchQ13State::SparseSlot &slot) {
    if (!slot.valid)
      return;
    const int64_t eff = pattern_null ? 0 : slot.count - excluded.Get(key);
    if (eff <= kDense)
      ++dense[static_cast<std::size_t>(eff)];
    else
      ++sparse[eff];
  });

  std::vector<TpchQ13OutputRow> rows;
  for (std::size_t c = 0; c < dense.size(); ++c)
    if (dense[c] > 0)
      rows.push_back({static_cast<int64_t>(c), dense[c]});
  for (const auto &[c, n] : sparse)
    rows.push_back({c, n});
  emit(rows);
}

std::vector<core::CalibrationWrite> TpchQ13Module::CalibrationWrites() const {
  return TpchQ13CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
