#include "tpch/q13/mysql/aux/module.hpp"
#include "tpch/q13/calibration_writes.hpp"
#include "prims/sql_like.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include <string>
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
constexpr bool kLikeCaseInsensitive = true;

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
}

void TpchQ13State::RemoveCustomer(int64_t custkey) {
  if (!Valid(custkey))
    return;
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
  AddCount(custkey, 1);
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
  AddCount(segment.custkey[slot], -1);
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
  const auto state = BuildState(services);
  builder.Write(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, 0,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts, state)));
  builder.Finish();
}

std::shared_ptr<TpchQ13State>
TpchQ13Module::BuildState(core::AuxEngineServices &services) const {
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
  EmitResult(services, *state, arguments, sink);
}

void TpchQ13Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  const auto state = BuildState(services);
  EmitResult(services, *state, arguments, sink);
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
// `matcher`. Each segment is matched with one SIMD pass over its comment blob.
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

// Customer chunks per slice: 4096 customers each, ~10 us a chunk.
constexpr std::size_t kChunksGrain = 16;

// Segments per slice: one holds thousands of comments (~0.1 ms to match).
constexpr std::size_t kSegmentsGrain = 16;

void CountMatchingOrders(const core::AuxEngineServices &services,
                         const TpchQ13State &state,
                         const like::LikeMatcher &matcher, Excluded &excluded) {
  core::ParallelSlices(
      services, state.segments.size(), kSegmentsGrain,
      [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t s = begin; s < end; ++s) {
          const auto *seg = state.segments[s].get();
          if (!seg || seg->live == 0)
            continue;
          const std::size_t n = seg->orderkey.size();
          const auto bits = like::MatchBlobBitmap(
              matcher, seg->comment.offsets(), n, seg->comment.data());
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
      });
}

} // namespace

void TpchQ13Module::EmitResult(const core::AuxEngineServices &services,
                               const TpchQ13State &state,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) const {
  const bool pattern_null =
      arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload);

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
    CountMatchingOrders(services, state, matcher, excluded);
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
  const std::size_t dense_size = static_cast<std::size_t>(std::min(max_count, kDense)) + 1;
  struct Histogram {
    std::vector<int64_t> dense; // sized on first use: a slice may see no customer
    std::map<int64_t, int64_t> sparse;
    void Count(int64_t eff, std::size_t size) {
      if (eff > kDense) {
        ++sparse[eff];
        return;
      }
      dense.resize(size);
      ++dense[static_cast<std::size_t>(eff)];
    }
  };
  Histogram histogram;
  core::ParallelReduce(
      services, state.customer_valid.NumChunks(), kChunksGrain, histogram,
      [&](Histogram &acc, std::size_t begin, std::size_t end) {
        for (std::size_t ci = begin; ci < end; ++ci) {
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
            acc.Count(eff, dense_size);
          }
        }
      },
      [](Histogram &into, Histogram &&from) {
        if (into.dense.size() < from.dense.size())
          into.dense.resize(from.dense.size());
        for (std::size_t i = 0; i < from.dense.size(); ++i)
          into.dense[i] += from.dense[i];
        for (const auto &[eff, n] : from.sparse)
          into.sparse[eff] += n;
      });
  state.sparse_customers.ForEach([&](int64_t key, const TpchQ13State::SparseSlot &slot) {
    if (!slot.valid)
      return;
    histogram.Count(pattern_null ? 0 : slot.count - excluded.Get(key), dense_size);
  });
  const auto &dense = histogram.dense;
  const auto &sparse = histogram.sparse;

  std::vector<TpchQ13OutputRow> rows;
  for (std::size_t c = 0; c < dense.size(); ++c)
    if (dense[c] > 0)
      rows.push_back({static_cast<int64_t>(c), dense[c]});
  for (const auto &[c, n] : sparse)
    rows.push_back({c, n});
  std::sort(rows.begin(), rows.end(), Q13OutputRowBetter);

  sink.Begin({{{"c_count", core::AuxType::Int64, false},
               {"custdist", core::AuxType::Int64, false}}});
  for (const auto &r : rows)
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, r.c_count),
                    core::AuxValue::Of(core::AuxType::Int64, r.custdist)}});
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ13Module::CalibrationWrites() const {
  return TpchQ13CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
