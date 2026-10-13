#include "tpch/q4/mysql/aux/module.hpp"
#include "tpch/q4/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"
#include "accelerator_codegen/stateful/prims/sql_date.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

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
                              int64_t keys, int64_t nodes) {
  if (!CustomProfilingEnabled())
    return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {"
          "\"phase\":\"%s\","
          "\"us\":%.3f,"
          "\"rows\":%ld,"
          "\"keys\":%ld,"
          "\"nodes\":%ld"
          "}\n",
          phase, us_val, (long)rows, (long)keys, (long)nodes);
}

// Cell readers distinguish SQL NULL from an unreadable payload.
enum class Cell { Null, Ok, Bad };

Cell IntCell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = std::get<int64_t>(cell.payload);
    return Cell::Ok;
  }
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return Cell::Ok;
  }
  return Cell::Bad;
}

Cell DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  const Cell c = IntCell(row, index, value);
  if (c != Cell::Ok)
    return c;
  if (value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return Cell::Bad;
  days = static_cast<int32_t>(value);
  return Cell::Ok;
}

Cell StringCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (!std::holds_alternative<std::string>(cell.payload))
    return Cell::Bad;
  out = std::get<std::string>(cell.payload);
  return Cell::Ok;
}

core::InvalidationPlan EmptyPlan() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

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

}  // namespace

int32_t TpchQ4DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return static_cast<int32_t>(core::sql_date::DaysFromCivil(year, month, day));
}

// date + INTERVAL 'months' MONTH with PostgreSQL/MySQL end-of-month clamping.
int64_t TpchQ4AddMonths(int64_t days_since_epoch, int64_t months) {
  return core::sql_date::AddMonths(days_since_epoch, months);
}

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
      const Cell key = IntCell(row, 0, delta.orderkey);
      const Cell date = DateCell(row, 1, delta.orderdate_days);
      const Cell prio = StringCell(row, 2, delta.orderpriority);
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
    const Cell k = IntCell(row, 0, orderkey);
    const Cell c = DateCell(row, 1, commit);
    const Cell r = DateCell(row, 2, receipt);
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
  auto rebuild_all = [&]() {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = EmptyPlan();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  };

  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = EmptyPlan();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty())
    return outcome;

  const auto total_deltas =
      buffer.Deltas().size() + buffer.OrderDeltas().size();
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas), 1, true, false, true, false, false,
       core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = EmptyPlan();
    outcome.pending_rebuild = outcome.invalidations;
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
      !tree_image->payload || !elig_image || !elig_image->payload)
    return rebuild_all();

  auto side = std::make_shared<TpchQ4Side>(*side_image->state);
  auto tree = std::make_shared<TpchQ4TreePayload>(*tree_image->payload);
  auto elig = std::make_shared<TpchQ4EligibilityPayload>(*elig_image->payload);

  // Per order key: remove the order's old contribution (if it counted), replay
  // its order-row changes in sequence (delete, then insert for an update),
  // add the net qualifying-lineitem delta, and add the new contribution (if
  // the order exists and has a qualifying lineitem).
  std::map<int64_t, int64_t> lineitem_delta;
  for (const auto &delta : buffer.Deltas())
    lineitem_delta[delta.orderkey] += delta.sign;
  std::map<int64_t, std::vector<const TpchQ4OrderDelta *>> order_changes;
  for (const auto &delta : buffer.OrderDeltas())
    order_changes[delta.orderkey].push_back(&delta);
  std::set<int64_t> touched;
  for (const auto &entry : lineitem_delta)
    touched.insert(entry.first);
  for (const auto &entry : order_changes)
    touched.insert(entry.first);

  for (const int64_t orderkey : touched) {
    TpchQ4OrderInfo info = elig->orders.Get(orderkey);
    if (info.Eligible())
      AdjustTree(*side, *tree, info.orderdate_days, info.priority_idx, -1);
    const auto changes = order_changes.find(orderkey);
    if (changes != order_changes.end()) {
      for (const auto *delta : changes->second) {
        if (delta->sign < 0) {
          info.priority_idx = kTpchQ4NoOrder;
          info.orderdate_days = 0;
        } else {
          info.orderdate_days = delta->orderdate_days;
          info.priority_idx = TpchQ4PriorityIndex(
              delta->orderpriority, delta->priority_is_null, *side);
        }
      }
    }
    const auto lines = lineitem_delta.find(orderkey);
    if (lines != lineitem_delta.end()) {
      const int64_t q = static_cast<int64_t>(info.qualifying_count) + lines->second;
      info.qualifying_count = q < 0 ? 0 : static_cast<uint32_t>(q);
    }
    elig->orders.Set(orderkey, info);
    if (info.Eligible())
      AdjustTree(*side, *tree, info.orderdate_days, info.priority_idx, +1);
  }

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

namespace {

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

void TpchQ4Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  CustomTimer execute_timer(CustomProfilingEnabled());
  bool is_null = false;
  int64_t start = 0;
  if (!StartDays(arguments, is_null, start))
    return ExecuteFallback(services, arguments, sink);
  if (is_null) {
    // NULL date: o_orderdate >= NULL is never true, so there are no groups.
    FinalizeTpchQ4({}, TpchQ4Side{}, sink);
    return;
  }

  CustomTimer load_timer(CustomProfilingEnabled());
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
                                 start, TpchQ4AddMonths(start, 3));
  EmitCustomProfile("state_load", load_timer.us(),
                    side_image->state->priority_names.size(), 0, 0);
  FinalizeTpchQ4(counts, *side_image->state, sink);
}

// Exact answer without published state: scan the tables.
void TpchQ4Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  bool is_null = false;
  int64_t start = 0;
  StartDays(arguments, is_null, start);
  if (is_null) {
    FinalizeTpchQ4({}, TpchQ4Side{}, sink);
    return;
  }
  const auto state = BuildState(services);
  FinalizeTpchQ4(
      CountRange(state.side, state.tree, start, TpchQ4AddMonths(start, 3)),
      state.side, sink);
}

TpchQ4State TpchQ4Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ4State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q4 requires a typed initial scan source");
  auto &orders = state.eligibility.orders;

  // First scan: qualifying lineitems per order key (also for keys that have
  // no order row yet; an order inserted later picks the count up).
  initial_scan->ScanLineitems([&](const TpchQ4LineitemInput &input) {
    if (LineitemQualifies(input.commitdate_days, input.receiptdate_days))
      orders.AddQualifying(input.orderkey);
  });

  // Second scan: order rows; an eligible order goes straight into the tree.
  initial_scan->ScanOrders([&](const TpchQ4OrderInput &input) {
    TpchQ4OrderInfo info = orders.Get(input.key);
    if (info.Eligible())  // duplicate key: replace the earlier row
      AdjustTree(state.side, state.tree, info.orderdate_days, info.priority_idx, -1);
    info.orderdate_days = input.orderdate_days;
    info.priority_idx = TpchQ4PriorityIndex(input.orderpriority,
                                            input.priority_is_null, state.side);
    orders.Set(input.key, info);
    if (info.Eligible())
      AdjustTree(state.side, state.tree, info.orderdate_days, info.priority_idx, +1);
  });
  state.tree.by_priority.resize(state.side.priority_names.size());
  return state;
}

std::vector<core::CalibrationWrite> TpchQ4Module::CalibrationWrites() const {
  return TpchQ4CalibrationWrites();
}

}  // namespace accelerator_codegen::aux_state::query
