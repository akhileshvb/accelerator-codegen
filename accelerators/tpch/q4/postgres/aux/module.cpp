#include "tpch/q4/postgres/aux/module.hpp"
#include "tpch/q4/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "prims/sql_date.hpp"

#include <algorithm>
#include <variant>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

// Build: lineitem and order rows are dealt out to buckets by order key; a
// bucket is built alone, so buckets run in parallel.
using core::kBuildBuckets;
// Touched order keys per maintenance slice (untimed).
constexpr std::size_t kKeyGrain = 1024;
// Buckets per slice of the build (untimed): each builds a whole partial state.
constexpr std::size_t kBucketGrain = 1;
// Months in the query's order-date window.
constexpr int32_t kWindowMonths = 3;

using Cell = core::CellState;

std::shared_ptr<const TpchQ4StateImage>
MakeSideImage(std::shared_ptr<const TpchQ4Side> state) {
  auto image = std::make_shared<TpchQ4StateImage>();
  image->kind = TpchQ4ImageKind::Side;
  image->state = std::move(state);
  return image;
}

std::shared_ptr<const TpchQ4TreeImage>
MakeTreeImage(std::shared_ptr<const TpchQ4TreePayload> payload) {
  auto image = std::make_shared<TpchQ4TreeImage>();
  image->kind = TpchQ4ImageKind::Tree;
  image->payload = std::move(payload);
  return image;
}

std::shared_ptr<const TpchQ4EligibilityImage>
MakeEligibilityImage(std::shared_ptr<const TpchQ4EligibilityPayload> payload) {
  auto image = std::make_shared<TpchQ4EligibilityImage>();
  image->kind = TpchQ4ImageKind::Eligibility;
  image->payload = std::move(payload);
  return image;
}

const TpchQ4StateImage *AsQ4Image(const core::StateImage &image) {
  return image.As<TpchQ4StateImage>();
}

const TpchQ4TreeImage *AsQ4TreeImage(const core::StateImage &image) {
  return image.As<TpchQ4TreeImage>();
}

const TpchQ4EligibilityImage *AsQ4EligibilityImage(const core::StateImage &image) {
  return image.As<TpchQ4EligibilityImage>();
}

bool LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days) {
  return commitdate_days < receiptdate_days;
}

// Adds `by` eligible orders for (day, priority) to the tree / outlier map.
void AdjustTree(TpchQ4Side &side, TpchQ4TreePayload &tree, int32_t day,
                uint32_t priority, int64_t by) {
  if (tree.by_priority.size() <= priority)
    tree.by_priority.resize(priority + 1);
  if (TpchQ4DayTree::InWindow(day)) {
    tree.by_priority[priority].Add(day, by);
    return;
  }
  const auto key = std::make_pair(static_cast<int64_t>(day), priority);
  const int64_t now = (side.outliers[key] += by);
  if (now == 0)
    side.outliers.erase(key);
}

// Eligible orders per (day, priority): the unit both build and maintenance
// fold orders into before touching the tree.
using TreeCells = std::unordered_map<uint64_t, int64_t>;

void AddCell(TreeCells &cells, int32_t day, uint32_t priority, int64_t by) {
  cells[(static_cast<uint64_t>(static_cast<uint32_t>(day)) << 32) | priority] += by;
}

// An order's tree contribution: one eligible order at (date, priority), or none.
void CountOrder(TreeCells &cells, const TpchQ4OrderInfo &info, int64_t by) {
  if (info.Eligible())
    AddCell(cells, info.orderdate_days, info.priority_idx, by);
}

void MergeCells(TreeCells &into, TreeCells &&from) {
  for (const auto &[key, by] : from)
    into[key] += by;
}

void ApplyCells(const TreeCells &cells, TpchQ4Side &side, TpchQ4TreePayload &tree) {
  for (const auto &[key, by] : cells)
    if (by != 0)
      AdjustTree(side, tree, static_cast<int32_t>(static_cast<uint32_t>(key >> 32)),
                 static_cast<uint32_t>(key), by);
}


struct OrderRow {
  int64_t key = 0;
  int32_t orderdate_days = 0;
  uint32_t priority = 0; // index into the owning piece's dictionary
};

// The order rows of one scan partition, with the partition's own priority
// dictionary (partitions cannot share one without locking).
struct OrdersPiece {
  core::Buckets<OrderRow> rows = core::Buckets<OrderRow>(kBuildBuckets);
  std::vector<std::string> names;
  std::unordered_map<std::string, uint32_t> index;

  void Add(const TpchQ4OrderInput &input) {
    const auto [it, inserted] =
        index.try_emplace(input.orderpriority, static_cast<uint32_t>(names.size()));
    if (inserted)
      names.push_back(input.orderpriority);
    rows[TpchQ4OrderTable::BucketOf(input.key, kBuildBuckets)].push_back(
        {input.key, input.orderdate_days, it->second});
  }
};

// Merging keeps the pieces apart: renumbering their rows would be a serial pass.
using OrderPieces = std::vector<OrdersPiece>;

struct BucketBuild {
  TpchQ4OrderTable orders;
  TreeCells cells;
};

// `renumber[p]` maps piece p's priority indexes to the side dictionary.
BucketBuild BuildBucket(const std::vector<int64_t> &qualifying, const OrderPieces &pieces,
                        const std::vector<std::vector<uint32_t>> &renumber,
                        std::size_t bucket) {
  BucketBuild built;
  for (const int64_t key : qualifying)
    built.orders.AddQualifying(key);
  for (std::size_t p = 0; p < pieces.size(); ++p)
    for (const OrderRow &row : pieces[p].rows[bucket]) {
      TpchQ4OrderInfo info = built.orders.Get(row.key);
      CountOrder(built.cells, info, -1); // duplicate key: replace the earlier row
      info.orderdate_days = row.orderdate_days;
      info.priority_idx = renumber[p][row.priority];
      built.orders.Set(row.key, info);
      CountOrder(built.cells, info, +1);
    }
  return built;
}

// An order-row change of a commit; priority_idx is the side dictionary index
// of an insert.
struct OrderEdit {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  uint32_t priority_idx = kTpchQ4NoOrder;
  int8_t sign = 0;
};

// One order key touched by a commit: its net qualifying-lineitem change and its
// order-row changes (a range of the key-sorted order edits, in commit order).
struct KeyEdit {
  int64_t orderkey = 0;
  int64_t line_delta = 0;
  std::size_t orders_begin = 0, orders_end = 0;
};

// Both inputs sorted by order key.
std::vector<KeyEdit> GroupByKey(const std::vector<TpchQ4LineitemDelta> &lines,
                                const std::vector<OrderEdit> &orders) {
  std::vector<KeyEdit> keys;
  std::size_t l = 0, o = 0;
  while (l < lines.size() || o < orders.size()) {
    KeyEdit edit;
    edit.orderkey = o == orders.size()  ? lines[l].orderkey
                    : l == lines.size() ? orders[o].orderkey
                                        : std::min(lines[l].orderkey, orders[o].orderkey);
    edit.orders_begin = o;
    for (; l < lines.size() && lines[l].orderkey == edit.orderkey; ++l)
      edit.line_delta += lines[l].sign;
    while (o < orders.size() && orders[o].orderkey == edit.orderkey)
      ++o;
    edit.orders_end = o;
    keys.push_back(edit);
  }
  return keys;
}

// The order after a commit: its order-row changes replayed (a delete, then the
// insert of an update), then the net qualifying-lineitem change.
TpchQ4OrderInfo Resolve(TpchQ4OrderInfo info, const KeyEdit &edit,
                        const std::vector<OrderEdit> &orders) {
  for (std::size_t i = edit.orders_begin; i < edit.orders_end; ++i) {
    if (orders[i].sign < 0) {
      info.priority_idx = kTpchQ4NoOrder;
      info.orderdate_days = 0;
    } else {
      info.orderdate_days = orders[i].orderdate_days;
      info.priority_idx = orders[i].priority_idx;
    }
  }
  const int64_t count = static_cast<int64_t>(info.qualifying_count) + edit.line_delta;
  info.qualifying_count = count < 0 ? 0 : static_cast<uint32_t>(count);
  return info;
}

// The date argument as days; false when it is not a date.
bool StartDays(const core::QueryArguments &arguments, bool &is_null,
               int64_t &days) {
  if (arguments.values.empty())
    return false;
  const auto &value = arguments.values[0];
  is_null = value.is_null;
  if (value.is_null)
    return true;
  if (std::holds_alternative<core::AuxDate>(value.payload)) {
    days = std::get<core::AuxDate>(value.payload).days_since_epoch;
    return true;
  }
  if (std::holds_alternative<int64_t>(value.payload)) {
    days = std::get<int64_t>(value.payload);
    return true;
  }
  return false;
}

}  // namespace

uint32_t TpchQ4PriorityIndex(const std::string &priority, bool is_null,
                             TpchQ4Side &side) {
  if (is_null) {
    if (side.null_priority_idx < 0) {
      side.null_priority_idx = static_cast<int32_t>(side.priority_names.size());
      side.priority_names.emplace_back();
    }
    return static_cast<uint32_t>(side.null_priority_idx);
  }
  auto it = side.priority_map.find(priority);
  if (it != side.priority_map.end())
    return static_cast<uint32_t>(it->second);
  const int32_t idx = static_cast<int32_t>(side.priority_names.size());
  side.priority_names.push_back(priority);
  side.priority_map.emplace(priority, idx);
  return static_cast<uint32_t>(idx);
}

bool TpchQ4TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ4TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
}
bool TpchQ4TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ4LineitemDelta> &TpchQ4TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ4OrderDelta> &TpchQ4TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ4TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
}
void TpchQ4TxnBuffer::AddDelta(TpchQ4LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ4TxnBuffer::AddOrderDelta(TpchQ4OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}

core::AcceleratorId TpchQ4Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ4Module::Dependencies() const {
  return {{config.orders_table, {0, 1, 2}, {0, 4, 5}},
          {config.lineitem_table, {0, 1, 2}, {0, 11, 12}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ4Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ4TxnBuffer>();
}

void TpchQ4Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto state = BuildState(services);
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kSide, 0,
      MakeSideImage(std::make_shared<TpchQ4Side>(std::move(state.side)))));
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kEligibility, 0,
      MakeEligibilityImage(std::make_shared<TpchQ4EligibilityPayload>(
          std::move(state.eligibility)))));
  builder.Write(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kTreeBase, 0,
      MakeTreeImage(
          std::make_shared<TpchQ4TreePayload>(std::move(state.tree)))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ4Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ4Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ4TxnBuffer &>(base_buffer);
  if (mutation.table == config.orders_table) {
    // An order with a NULL orderkey or orderdate can never satisfy the date
    // predicate / join, so such rows are no-ops in either direction.
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ4OrderDelta delta;
      delta.sign = sign;
      const Cell key = core::NullableIntOrDateCell(row, 0, delta.orderkey);
      const Cell date = core::NullableInt32OrDateCell(row, 1, delta.orderdate_days);
      const Cell prio = core::NullableStringCell(row, 2, delta.orderpriority);
      if (key == Cell::Bad || date == Cell::Bad || prio == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      if (key == Cell::Null || date == Cell::Null)
        return;
      delta.priority_is_null = prio == Cell::Null;
      buffer.AddOrderDelta(std::move(delta));
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
    return;
  }
  if (mutation.table != config.lineitem_table)
    return;
  // A lineitem with NULL orderkey / commitdate / receiptdate never satisfies
  // the EXISTS, so it is a no-op.
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0;
    int32_t commit = 0, receipt = 0;
    const Cell k = core::NullableIntOrDateCell(row, 0, orderkey);
    const Cell c = core::NullableInt32OrDateCell(row, 1, commit);
    const Cell r = core::NullableInt32OrDateCell(row, 2, receipt);
    if (k == Cell::Bad || c == Cell::Bad || r == Cell::Bad) {
      buffer.MarkRebuild();
      return;
    }
    if (k == Cell::Null || c == Cell::Null || r == Cell::Null ||
        !LineitemQualifies(commit, receipt))
      return;
    buffer.AddDelta({orderkey, sign});
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
TpchQ4Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ4TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  const auto total_deltas = buffer.Deltas().size() + buffer.OrderDeltas().size();
  if (total_deltas == 0)
    return outcome;

  const auto decision =
      services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
          total_deltas, services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  // One read of the three published images; the copies below are O(directory)
  // pointer copies, and a write clones only the chunks it touches.
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ4Layout::kSide, TpchQ4Layout::kTreeBase,
       TpchQ4Layout::kEligibility});
  const TpchQ4StateImage *side_image = nullptr;
  const TpchQ4TreeImage *tree_image = nullptr;
  const TpchQ4EligibilityImage *elig_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ4Layout::kSide)
      side_image = AsQ4Image(image);
    else if (image.partition == TpchQ4Layout::kTreeBase)
      tree_image = AsQ4TreeImage(image);
    else if (image.partition == TpchQ4Layout::kEligibility)
      elig_image = AsQ4EligibilityImage(image);
  }
  if (!side_image || !side_image->state || !tree_image ||
      !tree_image->payload || !elig_image || !elig_image->payload) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  auto side = std::make_shared<TpchQ4Side>(*side_image->state);
  auto tree = std::make_shared<TpchQ4TreePayload>(*tree_image->payload);
  auto elig = std::make_shared<TpchQ4EligibilityPayload>(*elig_image->payload);

  // Priorities are interned first (they extend the dictionary); per order key
  // the new state is then resolved in parallel against the unchanged tables,
  // and the single copy-on-write structures are written serially.
  std::vector<OrderEdit> order_edits;
  order_edits.reserve(buffer.OrderDeltas().size());
  for (const auto &delta : buffer.OrderDeltas()) {
    OrderEdit edit{delta.orderkey, delta.orderdate_days, kTpchQ4NoOrder, delta.sign};
    if (delta.sign > 0)
      edit.priority_idx = TpchQ4PriorityIndex(delta.orderpriority,
                                              delta.priority_is_null, *side);
    order_edits.push_back(edit);
  }
  auto line_edits = buffer.Deltas();
  core::ParallelStableSort(
      services, line_edits,
      [](const TpchQ4LineitemDelta &l, const TpchQ4LineitemDelta &r) {
        return l.orderkey < r.orderkey;
      },
      kKeyGrain);
  core::ParallelStableSort(
      services, order_edits,
      [](const OrderEdit &l, const OrderEdit &r) { return l.orderkey < r.orderkey; },
      kKeyGrain);
  const auto keys = GroupByKey(line_edits, order_edits);

  std::vector<TpchQ4OrderInfo> resolved(keys.size());
  TreeCells cells;
  core::ParallelReduce(
      services, keys.size(), kKeyGrain, cells,
      [&](TreeCells &into, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
          const TpchQ4OrderInfo old = elig->orders.Get(keys[i].orderkey);
          CountOrder(into, old, -1);
          resolved[i] = Resolve(old, keys[i], order_edits);
          CountOrder(into, resolved[i], +1);
        }
      },
      MergeCells);
  for (std::size_t i = 0; i < keys.size(); ++i)
    elig->orders.Set(keys[i].orderkey, resolved[i]);
  ApplyCells(cells, *side, *tree);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kSide, 0,
      MakeSideImage(std::move(side))));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kEligibility, 0,
      MakeEligibilityImage(std::move(elig))));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ4Layout::kAccelerator, TpchQ4Layout::kTreeBase, 0,
      MakeTreeImage(std::move(tree))));
  return outcome;
}

bool TpchQ4Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

// Eligible orders per priority with start <= orderdate < end: O(log D) tree
// cells per priority plus the (normally empty) outlier map.
std::vector<int64_t> TpchQ4Module::CountRange(const TpchQ4Side &side,
                                              const TpchQ4TreePayload &tree,
                                              int64_t start, int64_t end) {
  std::vector<int64_t> counts(side.priority_names.size(), 0);
  if (start >= end)
    return counts;
  const int64_t a = std::clamp<int64_t>(start + TpchQ4DayTree::kDayBias, 0,
                                        TpchQ4DayTree::kKeySpan);
  const int64_t b = std::clamp<int64_t>(end + TpchQ4DayTree::kDayBias, 0,
                                        TpchQ4DayTree::kKeySpan);
  if (a < b)
    for (std::size_t p = 0; p < tree.by_priority.size() && p < counts.size(); ++p)
      counts[p] += tree.by_priority[p].SumKeys(a, b);
  for (auto it = side.outliers.lower_bound({start, 0});
       it != side.outliers.end() && it->first.first < end; ++it)
    if (it->first.second < counts.size())
      counts[it->first.second] += it->second;
  return counts;
}

void TpchQ4Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  bool is_null = false;
  int64_t start = 0;
  if (!StartDays(arguments, is_null, start))
    return ExecuteFallback(services, arguments, sink);
  if (is_null) {
    // NULL date: o_orderdate >= NULL is never true, so there are no groups.
    FinalizeTpchQ4({}, TpchQ4Side{}, sink);
    return;
  }

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ4Layout::kSide, TpchQ4Layout::kTreeBase});
  const TpchQ4StateImage *side_image = nullptr;
  const TpchQ4TreeImage *tree_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ4Layout::kSide)
      side_image = AsQ4Image(image);
    else if (image.partition == TpchQ4Layout::kTreeBase)
      tree_image = AsQ4TreeImage(image);
  }
  if (!side_image || !side_image->state || !tree_image || !tree_image->payload) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto counts = CountRange(*side_image->state, *tree_image->payload,
                                 start, core::sql_date::AddMonths(start, kWindowMonths));
  FinalizeTpchQ4(counts, *side_image->state, sink);
}

TpchQ4State TpchQ4Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q4 requires a typed initial scan source");
  const auto partitions = core::BuildPartitions(services);

  // First scan: qualifying lineitems per order key (also for keys that have
  // no order row yet; an order inserted later picks the count up).
  auto qualifying = core::PartitionedBuild<core::Buckets<int64_t>>(
      services, [] { return core::Buckets<int64_t>(kBuildBuckets); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(
            services, parts,
            [&](std::size_t partition, const TpchQ4LineitemInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ4LineitemInput>{input});
            });
      },
      [](core::Buckets<int64_t> &into, const TpchQ4LineitemInput &input) {
        if (LineitemQualifies(input.commitdate_days, input.receiptdate_days))
          into[TpchQ4OrderTable::BucketOf(input.orderkey, kBuildBuckets)].push_back(
              input.orderkey);
      },
      core::AppendBuckets<int64_t>, partitions);

  // Second scan: order rows.
  const auto pieces = core::PartitionedBuild<OrderPieces>(
      services, [] { return OrderPieces(1); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(
            services, parts,
            [&](std::size_t partition, const TpchQ4OrderInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ4OrderInput>{input});
            });
      },
      [](OrderPieces &into, const TpchQ4OrderInput &input) { into.front().Add(input); },
      core::AppendRows<OrdersPiece>, partitions);

  TpchQ4State state;
  std::vector<std::vector<uint32_t>> renumber;
  for (const auto &piece : pieces) {
    renumber.emplace_back();
    for (const auto &name : piece.names)
      renumber.back().push_back(TpchQ4PriorityIndex(name, false, state.side));
  }

  std::vector<BucketBuild> built(kBuildBuckets);
  core::ParallelEach(services, kBuildBuckets, kBucketGrain, [&](std::size_t b) {
    built[b] = BuildBucket(qualifying[b], pieces, renumber, b);
    qualifying[b] = {};
  });
  TreeCells cells;
  for (auto &bucket : built) {
    state.eligibility.orders.MergeDisjoint(std::move(bucket.orders));
    MergeCells(cells, std::move(bucket.cells));
  }
  ApplyCells(cells, state.side, state.tree);
  state.tree.by_priority.resize(state.side.priority_names.size());
  return state;
}

std::vector<core::CalibrationWrite> TpchQ4Module::CalibrationWrites() const {
  return TpchQ4CalibrationWrites();
}

}  // namespace accelerator_codegen::aux_state::query
