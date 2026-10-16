#include "tpch/q13/mysql/aux/module.hpp"
#include "tpch/q13/calibration_writes.hpp"
#include "prims/sql_like.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/partitioned_rows.hpp"

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

std::shared_ptr<TpchQ13OrderSegment> NewOrderSegment() {
  auto segment = std::make_shared<TpchQ13OrderSegment>();
  segment->orderkey.reserve(kTpchQ13SegmentRows);
  segment->custkey.reserve(kTpchQ13SegmentRows);
  segment->alive.reserve(kTpchQ13SegmentRows);
  segment->comment.Reserve(kTpchQ13SegmentRows, 0);
  return segment;
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

std::size_t TpchQ13State::OpenSegmentRoom() const {
  if (open_segment < 0)
    return 0;
  return kTpchQ13SegmentRows - segments[static_cast<std::size_t>(open_segment)]->orderkey.size();
}

uint32_t TpchQ13State::OpenFreshSegment() {
  if (!free_segments.empty()) {
    open_segment = free_segments.back();
    free_segments.pop_back();
  } else {
    open_segment = static_cast<int64_t>(segments.size());
    segments.emplace_back();
  }
  segments[static_cast<std::size_t>(open_segment)] = NewOrderSegment();
  return static_cast<uint32_t>(open_segment);
}

// Orders count toward their custkey whether or not that customer exists yet
// (the join only sees a customer row), so customer and order maintenance are
// independent and a customer insert/delete is a single flag flip.
void TpchQ13State::AddOrder(int64_t orderkey, int64_t custkey,
                            std::string_view comment) {
  RemoveOrder(orderkey); // primary key: re-adding an existing key replaces it
  const auto id = OpenSegmentRoom() > 0 ? static_cast<uint32_t>(open_segment)
                                        : OpenFreshSegment();
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

namespace {

// Parallel loading of orders, for the build and for large maintenance batches.
// The state's index shards and count chunks are written by the slice that owns
// them: a key belongs to owner (shard or chunk) % kOwners, so slices never share
// a write. What a slice may not write itself waits in buckets sorted by owner.
constexpr std::size_t kOwners = 64;

using OrderIndex = decltype(TpchQ13State::order_row);
using CountArray = decltype(TpchQ13State::order_count);

// The state's index is default-built: kShards shards, so this is its shard.
constexpr unsigned kIndexShardBits = static_cast<unsigned>(__builtin_ctzll(OrderIndex::kShards));

std::size_t IndexOwner(int64_t orderkey) {
  return OrderIndex::ShardOf(orderkey, kIndexShardBits) % kOwners;
}
std::size_t CountOwner(int64_t custkey) {
  return static_cast<std::size_t>(custkey) / CountArray::kChunkSize % kOwners;
}

struct CountChange {
  int64_t custkey;
  int32_t delta;
};

struct IndexEntry {
  int64_t orderkey;
  uint64_t ref; // (segment << kSlotBits) | slot
};

// Count changes and index entries of orders placed in segments, by owner.
struct Placed {
  Buckets<IndexEntry> index;
  Buckets<CountChange> counts;
  std::vector<CountChange> sparse_counts; // keys outside the dense range
  int64_t max_dense_custkey = -1;
  uint64_t segment_base = 0; // added to the segment of every entry

  Placed() : index(kOwners), counts(kOwners) {}

  void AddCount(int64_t custkey, int32_t delta) {
    if (!TpchQ13State::Dense(custkey)) {
      sparse_counts.push_back({custkey, delta});
      return;
    }
    counts[CountOwner(custkey)].push_back({custkey, delta});
    max_dense_custkey = std::max(max_dense_custkey, custkey);
  }
  void AddOrder(int64_t orderkey, uint64_t ref, int64_t custkey) {
    index[IndexOwner(orderkey)].push_back({orderkey, ref});
    AddCount(custkey, 1);
  }
};

template <class Row> void Release(std::vector<Row> &rows) { std::vector<Row>().swap(rows); }

// Writes the index entries and count changes into the state, one slice per owner.
void ApplyPlaced(const core::AuxEngineServices &services, TpchQ13State &state,
                 std::vector<Placed> &placed) {
  int64_t max_dense_custkey = -1;
  for (const auto &p : placed)
    max_dense_custkey = std::max(max_dense_custkey, p.max_dense_custkey);
  // The count directory is sized up front: slices then only fill their own chunks.
  if (max_dense_custkey >= 0)
    state.order_count.Mutable(max_dense_custkey);
  core::ParallelEach(services, kOwners, 1, [&](std::size_t owner) {
    for (auto &p : placed) {
      for (const auto &entry : p.index[owner])
        state.order_row.Upsert(entry.orderkey, entry.ref + (p.segment_base << kSlotBits));
      for (const auto &change : p.counts[owner])
        state.order_count.Mutable(change.custkey) += change.delta;
      Release(p.index[owner]);
      Release(p.counts[owner]);
    }
  });
  for (const auto &p : placed)
    for (const auto &change : p.sparse_counts)
      state.AddCount(change.custkey, change.delta);
}

// ── Build ──────────────────────────────────────────────────────────────────

// The orders of one scan partition: full segments of its own.
struct OrderPartial {
  std::vector<std::shared_ptr<TpchQ13OrderSegment>> segments;
  Placed placed; // refs are relative to this partition's first segment

  void Add(const TpchQ13OrderInput &order) {
    if (segments.empty() || segments.back()->orderkey.size() >= kTpchQ13SegmentRows)
      segments.push_back(NewOrderSegment());
    auto &segment = *segments.back();
    const uint64_t slot = segment.orderkey.size();
    segment.orderkey.push_back(order.orderkey);
    segment.custkey.push_back(order.custkey);
    segment.alive.push_back(1);
    segment.comment.Append(order.comment);
    ++segment.live;
    placed.AddOrder(order.orderkey, ((segments.size() - 1) << kSlotBits) | slot, order.custkey);
  }
};

using OrderPartials = std::vector<OrderPartial>;

// Moves the partitions' segments into the state in partition order, then indexes
// and counts them. Order keys are unique (primary key), so no row replaces another.
void LoadOrders(const core::AuxEngineServices &services, TpchQ13State &state,
                OrderPartials &partials) {
  std::vector<Placed> placed;
  for (auto &partial : partials) {
    partial.placed.segment_base = state.segments.size();
    for (auto &segment : partial.segments) {
      state.live_orders += segment->live;
      state.segments.push_back(std::move(segment));
    }
    placed.push_back(std::move(partial.placed));
  }
  ApplyPlaced(services, state, placed);
}

// ── Maintenance ────────────────────────────────────────────────────────────

constexpr uint64_t kNoRow = ~uint64_t{0};
constexpr std::size_t kNoDelta = ~std::size_t{0};

// What a batch does to one order key: any delta on it removes the row it had,
// and the last delta, if an insert, is the new row.
struct KeyNet {
  uint64_t old_ref = kNoRow;
  std::size_t insert = kNoDelta;
};

// The rows one owner of order keys removes and the inserts that stay, in delta order.
struct Resolved {
  std::vector<uint64_t> removed; // refs of the rows the keys had
  std::vector<std::size_t> kept; // delta indexes
};

Resolved ResolveKeys(TpchQ13State &state, const std::vector<TpchQ13Delta> &deltas,
                     const std::vector<std::size_t> &mine) {
  std::unordered_map<int64_t, KeyNet> nets;
  for (const std::size_t i : mine) {
    const auto &delta = deltas[i];
    const auto [net, fresh] = nets.try_emplace(delta.orderkey);
    if (const uint64_t *ref = fresh ? state.order_row.Find(delta.orderkey) : nullptr) {
      net->second.old_ref = *ref;
      state.order_row.Erase(delta.orderkey);
    }
    net->second.insert = delta.sign > 0 ? i : kNoDelta;
  }
  Resolved out;
  for (const std::size_t i : mine)
    if (nets.find(deltas[i].orderkey)->second.insert == i)
      out.kept.push_back(i);
  for (const auto &[key, net] : nets)
    if (net.old_ref != kNoRow)
      out.removed.push_back(net.old_ref);
  return out;
}

// Marks the removed rows dead, one slice per owner of their segments; returns
// their customers' count changes.
std::vector<Placed> KillRows(const core::AuxEngineServices &services, TpchQ13State &state,
                             const std::vector<Resolved> &resolved) {
  Buckets<uint64_t> by_segment(kOwners);
  for (const auto &r : resolved)
    for (const uint64_t ref : r.removed) {
      by_segment[(ref >> kSlotBits) % kOwners].push_back(ref);
      state.MarkTouched(static_cast<uint32_t>(ref >> kSlotBits));
    }
  std::vector<Placed> placed(kOwners);
  core::ParallelEach(services, kOwners, 1, [&](std::size_t owner) {
    for (const uint64_t ref : by_segment[owner]) {
      auto &segment = *state.MutableSegment(static_cast<uint32_t>(ref >> kSlotBits));
      const std::size_t slot = ref & (kTpchQ13SegmentRows - 1);
      segment.alive[slot] = 0;
      --segment.live;
      placed[owner].AddCount(segment.custkey[slot], -1);
    }
  });
  return placed;
}

// Appends the kept inserts to the open segment and to fresh ones, one slice per
// target segment.
std::vector<Placed> AppendRows(const core::AuxEngineServices &services, TpchQ13State &state,
                               const std::vector<TpchQ13Delta> &deltas,
                               const std::vector<std::size_t> &kept) {
  std::vector<uint32_t> targets;
  std::vector<std::size_t> first{0}; // kept[first[t] .. first[t + 1]) go to targets[t]
  const auto target = [&](uint32_t id, std::size_t room) {
    targets.push_back(id);
    first.push_back(std::min(kept.size(), first.back() + room));
  };
  if (const std::size_t room = state.OpenSegmentRoom(); room > 0 && !kept.empty())
    target(static_cast<uint32_t>(state.open_segment), room);
  while (first.back() < kept.size())
    target(state.OpenFreshSegment(), kTpchQ13SegmentRows);

  std::vector<Placed> placed(targets.size());
  core::ParallelEach(services, targets.size(), 1, [&](std::size_t t) {
    auto &segment = *state.MutableSegment(targets[t]);
    for (std::size_t k = first[t]; k < first[t + 1]; ++k) {
      const auto &delta = deltas[kept[k]];
      const uint64_t slot = segment.orderkey.size();
      segment.orderkey.push_back(delta.orderkey);
      segment.custkey.push_back(delta.custkey);
      segment.alive.push_back(1);
      segment.comment.Append(delta.comment);
      ++segment.live;
      placed[t].AddOrder(delta.orderkey, (uint64_t{targets[t]} << kSlotBits) | slot,
                         delta.custkey);
    }
  });
  return placed;
}

// The order deltas with the state AddOrder / RemoveOrder one by one would give
// (rows may sit in other segment slots). The keys of a batch are resolved against
// the index by shard owners, dead rows and appended rows are written by segment,
// and the index and counts by owner.
void ApplyOrderDeltas(const core::AuxEngineServices &services, TpchQ13State &state,
                      const std::vector<TpchQ13Delta> &deltas) {
  Buckets<std::size_t> by_owner(kOwners);
  for (std::size_t i = 0; i < deltas.size(); ++i)
    if (deltas[i].table == TpchQ13Delta::Table::Order)
      by_owner[IndexOwner(deltas[i].orderkey)].push_back(i);
  std::vector<Resolved> resolved(kOwners);
  core::ParallelEach(services, kOwners, 1, [&](std::size_t owner) {
    resolved[owner] = ResolveKeys(state, deltas, by_owner[owner]);
  });

  std::size_t removed = 0;
  std::vector<std::size_t> kept;
  for (const auto &r : resolved) {
    removed += r.removed.size();
    kept.insert(kept.end(), r.kept.begin(), r.kept.end());
  }
  auto placed = KillRows(services, state, resolved);
  auto appended = AppendRows(services, state, deltas, kept);
  placed.insert(placed.end(), std::make_move_iterator(appended.begin()),
                std::make_move_iterator(appended.end()));
  state.live_orders += static_cast<int64_t>(kept.size()) - static_cast<int64_t>(removed);
  ApplyPlaced(services, state, placed);
}

// Customers first, then orders: the two do not interact. A batch too small to
// split runs delta by delta.
constexpr std::size_t kDeltasGrain = 8192;

void ApplyDeltas(const core::AuxEngineServices &services, TpchQ13State &state,
                 const std::vector<TpchQ13Delta> &deltas) {
  const bool parallel = core::SliceCount(services, deltas.size(), kDeltasGrain) > 1;
  for (const auto &delta : deltas) {
    if (delta.table == TpchQ13Delta::Table::Customer) {
      if (delta.sign > 0)
        state.AddCustomer(delta.orderkey);
      else
        state.RemoveCustomer(delta.orderkey);
    } else if (!parallel) {
      if (delta.sign > 0)
        state.AddOrder(delta.orderkey, delta.custkey, delta.comment);
      else
        state.RemoveOrder(delta.orderkey);
    }
  }
  if (parallel)
    ApplyOrderDeltas(services, state, deltas);
}

} // namespace

std::shared_ptr<TpchQ13State>
TpchQ13Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q13 requires a typed initial scan source");
  auto state = std::make_shared<TpchQ13State>();
  const std::size_t partitions = BuildPartitions(services);
  const auto customers = core::PartitionedBuild<std::vector<int64_t>>(
      services, [] { return std::vector<int64_t>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanCustomersPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ13CustomerInput &input) {
              sink(partition, OneRowBatch<TpchQ13CustomerInput>{input});
            });
      },
      [](std::vector<int64_t> &into, const TpchQ13CustomerInput &input) {
        into.push_back(input.key);
      },
      [](std::vector<int64_t> &into, std::vector<int64_t> &&from) {
        into.insert(into.end(), from.begin(), from.end());
      },
      partitions);
  for (const int64_t key : customers)
    state->AddCustomer(key);

  auto orders = core::PartitionedBuild<OrderPartials>(
      services, [] { return OrderPartials(1); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrdersPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ13OrderInput &input) {
              sink(partition, OneRowBatch<TpchQ13OrderInput>{input});
            });
      },
      [](OrderPartials &into, const TpchQ13OrderInput &input) { into.front().Add(input); },
      [](OrderPartials &into, OrderPartials &&from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()),
                    std::make_move_iterator(from.end()));
      },
      partitions);
  LoadOrders(services, *state, orders);
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

  ApplyDeltas(services, *updated_state, buffer.Deltas());
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
  core::ParallelReduce(
      services, state.order_count.NumChunks(), kChunksGrain, max_count,
      [&](int64_t &acc, std::size_t begin, std::size_t end) {
        for (std::size_t ci = begin; ci < end; ++ci)
          if (const auto *chunk = state.order_count.ChunkAt(ci))
            for (const int32_t c : *chunk)
              acc = std::max<int64_t>(acc, c);
      },
      [](int64_t &into, int64_t &&from) { into = std::max(into, from); });
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
