#include "tpch/q12/postgres/aux/module.hpp"
#include "tpch/q12/calibration_writes.hpp"
#include "prims/sql_date.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include "tpch/q12/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr int64_t kModeBits = 24;  // cell key = day << 24 | shipmode id

// Cell readers distinguish SQL NULL from an unreadable payload.
enum class Cell { Null, Ok, Bad };

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

std::shared_ptr<const TpchQ12StateImage>
MakeStateImage(TpchQ12ImageKind kind,
               const std::shared_ptr<const TpchQ12State> &state) {
  auto image = std::make_shared<TpchQ12StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const TpchQ12StateImage *AsQ12Image(const core::StateImage &image,
                                    TpchQ12ImageKind kind) {
  const auto *payload = image.As<TpchQ12StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// What one joined qualifying lineitem adds to its (day, shipmode) cell for an
// order of the given class; `sign` is -1 to take it away.
TpchQ12Counts Contribution(TpchQ12PriorityClass priority, int64_t sign) {
  switch (priority) {
  case TpchQ12PriorityClass::High:
    return {sign, 0, sign};
  case TpchQ12PriorityClass::Low:
    return {0, sign, sign};
  case TpchQ12PriorityClass::Neither:
    return {0, 0, sign};
  default:
    return {};
  }
}

uint32_t ShipmodeId(TpchQ12State &state, const std::string &mode) {
  const auto it = state.shipmode_ids.find(mode);
  if (it != state.shipmode_ids.end())
    return it->second;
  const auto id = static_cast<uint32_t>(state.shipmodes.size());
  if (id >= (uint32_t{1} << kModeBits))
    throw std::runtime_error("TPC-H Q12: too many distinct shipmodes");
  state.shipmodes.push_back(mode);
  state.shipmode_ids.emplace(mode, id);
  return id;
}

int64_t CellKey(int32_t day, uint32_t mode_id) {
  return static_cast<int64_t>(day) * (int64_t{1} << kModeBits) + mode_id;
}

// cell(day, mode) += delta; a cell that drops to zero rows is removed.
void AddToCell(TpchQ12State &state, int32_t day, const std::string &mode,
               const TpchQ12Counts &delta) {
  const int64_t key = CellKey(day, ShipmodeId(state, mode));
  const TpchQ12Counts *old = state.cells.Find(key);
  TpchQ12Counts now = old ? *old : TpchQ12Counts{};
  now += delta;
  if (now == TpchQ12Counts{})
    state.cells.Erase(key);
  else
    state.cells.Upsert(key, now);
}

}  // namespace

int32_t TpchQ12DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return static_cast<int32_t>(core::sql_date::DaysFromCivil(year, month, day));
}

// date + INTERVAL 'months' MONTH with PostgreSQL/MySQL end-of-month clamping.
int64_t TpchQ12AddMonths(int64_t days_since_epoch, int64_t months) {
  return core::sql_date::AddMonths(days_since_epoch, months);
}

// o_orderpriority = '1-URGENT' OR = '2-HIGH' counts high; <> both counts low;
// NULL satisfies neither (the row joins but adds 0 to both sums).
TpchQ12PriorityClass TpchQ12ClassifyPriority(const std::string &priority,
                                             bool is_null) {
  if (is_null)
    return TpchQ12PriorityClass::Neither;
  if (priority == "1-URGENT" || priority == "2-HIGH")
    return TpchQ12PriorityClass::High;
  return TpchQ12PriorityClass::Low;
}

bool TpchQ12LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days,
                              int32_t shipdate_days) {
  return commitdate_days < receiptdate_days && shipdate_days < commitdate_days;
}

bool TpchQ12TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ12TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
}
bool TpchQ12TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ12LineitemDelta> &TpchQ12TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ12OrderDelta> &TpchQ12TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ12TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
}
void TpchQ12TxnBuffer::AddDelta(TpchQ12LineitemDelta delta) {
  pending = true;
  deltas.push_back(std::move(delta));
}
void TpchQ12TxnBuffer::AddOrderDelta(TpchQ12OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}

core::AcceleratorId TpchQ12Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ12Module::Dependencies() const {
  return {{config.orders_table, {0, 1}, {0, 5}},
          {config.lineitem_table, {0, 1, 2, 3, 4}, {0, 14, 11, 12, 10}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ12Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ12TxnBuffer>();
}

void TpchQ12Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = std::make_shared<const TpchQ12State>(BuildState(services));
  for (const auto &[part, kind] :
       {std::make_pair(TpchQ12Parts::kOrderPriorities,
                       TpchQ12ImageKind::OrderPriorities),
        std::make_pair(TpchQ12Parts::kCells, TpchQ12ImageKind::Cells),
        std::make_pair(TpchQ12Parts::kSide, TpchQ12ImageKind::Side)})
    builder.Write(core::StateImage::Make(core::TpchQ12Layout::kAccelerator,
                                         part, 0, MakeStateImage(kind, state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ12Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ12Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ12TxnBuffer &>(base_buffer);
  if (mutation.table == config.orders_table) {
    // A NULL orderkey never joins, so such an order is a no-op.
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0;
      std::string priority;
      const Cell k = IntCell(row, 0, orderkey);
      const Cell p = StringCell(row, 1, priority);
      if (k == Cell::Bad || p == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      if (k == Cell::Null)
        return;
      buffer.AddOrderDelta(
          {orderkey, TpchQ12ClassifyPriority(priority, p == Cell::Null), sign});
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
  // A lineitem with a NULL orderkey, shipmode or date can never qualify
  // (NULL comparisons are not true), so it is a no-op.
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ12LineitemDelta delta;
    delta.sign = sign;
    int32_t commit = 0, receipt = 0, ship = 0;
    const Cell cells[5] = {IntCell(row, 0, delta.orderkey),
                           StringCell(row, 1, delta.shipmode),
                           DateCell(row, 2, commit), DateCell(row, 3, receipt),
                           DateCell(row, 4, ship)};
    bool null_seen = false;
    for (const Cell c : cells) {
      if (c == Cell::Bad) {
        buffer.MarkRebuild();
        return;
      }
      null_seen |= c == Cell::Null;
    }
    if (null_seen || !TpchQ12LineitemQualifies(commit, receipt, ship))
      return;
    delta.receipt_day = receipt;
    buffer.AddDelta(std::move(delta));
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
TpchQ12Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ12TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  auto rebuild_all = [&]() {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
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
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size() + buffer.OrderDeltas().size()),
       1, true, false, true, false, false, core::MaintenanceAction::NoOp,
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

  // One private working copy: directory / shard pointer copies of the
  // published images; writes below clone only the chunks and shards touched.
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ12Parts::kOrderPriorities, TpchQ12Parts::kCells,
       TpchQ12Parts::kSide});
  const TpchQ12StateImage *orders_image = nullptr, *cells_image = nullptr,
                          *side_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ12Parts::kOrderPriorities)
      orders_image = AsQ12Image(image, TpchQ12ImageKind::OrderPriorities);
    else if (image.partition == TpchQ12Parts::kCells)
      cells_image = AsQ12Image(image, TpchQ12ImageKind::Cells);
    else if (image.partition == TpchQ12Parts::kSide)
      side_image = AsQ12Image(image, TpchQ12ImageKind::Side);
  }
  if (!orders_image || !cells_image || !side_image)
    return rebuild_all();
  auto work = std::make_shared<TpchQ12State>();
  work->order_class = orders_image->state->order_class;
  work->cells = cells_image->state->cells;
  work->shipmodes = side_image->state->shipmodes;
  work->shipmode_ids = side_image->state->shipmode_ids;
  const std::size_t modes_before = work->shipmodes.size();

  // The class of every touched order key before and after the batch (order
  // changes replayed in sequence: delete, then insert for an update).
  struct ClassChange {
    TpchQ12PriorityClass before, after;
  };
  std::map<int64_t, ClassChange> changes;
  for (const auto &delta : buffer.OrderDeltas()) {
    auto it = changes.find(delta.orderkey);
    if (it == changes.end()) {
      const auto before = work->PriorityOf(delta.orderkey);
      it = changes.emplace(delta.orderkey, ClassChange{before, before}).first;
    }
    it->second.after = delta.sign < 0 ? TpchQ12PriorityClass::Absent
                                      : delta.priority;
  }

  // A key whose class changed (order inserted, deleted, reclassified) moves
  // ALL of its qualifying lineitems between classes, including ones this
  // batch did not touch: ask the database for them (index lookup).
  std::vector<int64_t> moved;
  for (const auto &[key, change] : changes)
    if (change.before != change.after)
      moved.push_back(key);
  if (!moved.empty()) {
    if (!initial_scan)
      return rebuild_all();
    initial_scan->LookupLineitemsByOrder(moved, [&](const TpchQ12LineitemInput &input) {
      if (!TpchQ12LineitemQualifies(input.commitdate_days,
                                    input.receiptdate_days, input.shipdate_days))
        return;
      const auto it = changes.find(input.orderkey);
      if (it == changes.end() || it->second.before == it->second.after)
        return;
      AddToCell(*work, input.receiptdate_days, input.shipmode,
                Contribution(it->second.after, +1));
      AddToCell(*work, input.receiptdate_days, input.shipmode,
                Contribution(it->second.before, -1));
    });
  }
  // The lookup returned the lineitem set AFTER the batch, and above it was
  // taken out of the OLD class for all of it.  The batch's own lineitem
  // inserts were not in the old set (so were wrongly subtracted) and its
  // deletes were (but are missing from the lookup): in both cases the
  // correction is sign x old-class contribution.  For keys whose class did
  // not change this is simply the lineitem delta itself.
  for (const auto &delta : buffer.Deltas()) {
    const auto it = changes.find(delta.orderkey);
    const auto before = it != changes.end() ? it->second.before
                                            : work->PriorityOf(delta.orderkey);
    AddToCell(*work, delta.receipt_day, delta.shipmode,
              Contribution(before, delta.sign));
  }
  for (const auto &[key, change] : changes)
    if (change.before != change.after)
      work->SetPriority(key, change.after);

  const std::shared_ptr<const TpchQ12State> published = work;
  outcome.action = core::MaintenanceAction::ApplyDelta;
  if (!changes.empty())
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, TpchQ12Parts::kOrderPriorities, 0,
        MakeStateImage(TpchQ12ImageKind::OrderPriorities, published)));
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ12Layout::kAccelerator, TpchQ12Parts::kCells, 0,
      MakeStateImage(TpchQ12ImageKind::Cells, published)));
  if (work->shipmodes.size() != modes_before)
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, TpchQ12Parts::kSide, 0,
        MakeStateImage(TpchQ12ImageKind::Side, published)));
  return outcome;
}

bool TpchQ12Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

namespace {

// A string parameter: nullopt for SQL NULL (which equals no shipmode).
bool StringArg(const core::AuxValue &value, std::string &out, bool &is_null) {
  is_null = value.is_null;
  if (value.is_null)
    return true;
  if (!std::holds_alternative<std::string>(value.payload))
    return false;
  out = std::get<std::string>(value.payload);
  return true;
}

bool DateArg(const core::AuxValue &value, int64_t &days, bool &is_null) {
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

std::vector<TpchQ12Group> TpchQ12Module::Answer(const TpchQ12State &state,
                                                const core::QueryArguments &arguments) {
  std::vector<TpchQ12Group> groups;
  std::string modes[2];
  bool mode_null[2] = {false, false};
  int64_t start = 0;
  bool date_null = false;
  if (arguments.values.size() < 3 ||
      !StringArg(arguments.values[0], modes[0], mode_null[0]) ||
      !StringArg(arguments.values[1], modes[1], mode_null[1]) ||
      !DateArg(arguments.values[2], start, date_null))
    throw std::invalid_argument("TPC-H Q12: bad arguments");
  // l_receiptdate >= NULL is never true; NULL shipmodes match nothing.
  if (date_null)
    return groups;
  const int64_t end = core::sql_date::AddYears(start, 1);
  for (int i = 0; i < 2; ++i) {
    if (mode_null[i] || (i == 1 && !mode_null[0] && modes[1] == modes[0]))
      continue;  // IN (x, x) is one group
    const auto id = state.shipmode_ids.find(modes[i]);
    if (id == state.shipmode_ids.end())
      continue;
    TpchQ12Counts total;
    for (int64_t day = start; day < end; ++day) {
      const TpchQ12Counts *c =
          state.cells.Find(CellKey(static_cast<int32_t>(day), id->second));
      if (c)
        total += *c;
    }
    if (total.rows > 0)
      groups.push_back({modes[i], total});
  }
  return groups;
}

void TpchQ12Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (arguments.values.size() < 3)
    return ExecuteFallback(services, arguments, sink);
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ12Parts::kCells, TpchQ12Parts::kSide});
  const TpchQ12StateImage *cells_image = nullptr, *side_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ12Parts::kCells)
      cells_image = AsQ12Image(image, TpchQ12ImageKind::Cells);
    else if (image.partition == TpchQ12Parts::kSide)
      side_image = AsQ12Image(image, TpchQ12ImageKind::Side);
  }
  if (!cells_image || !side_image)
    return ExecuteFallback(services, arguments, sink);
  TpchQ12State view;
  view.cells = cells_image->state->cells;
  view.shipmode_ids = side_image->state->shipmode_ids;
  TpchQ12FinalizeAndEmit(Answer(view, arguments), sink);
}

// Exact answer without published state: scan the tables.
void TpchQ12Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  TpchQ12FinalizeAndEmit(Answer(BuildState(services), arguments), sink);
}

TpchQ12State TpchQ12Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ12State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q12 requires a typed initial scan source");
  initial_scan->ScanOrders([&](const TpchQ12OrderInput &input) {
    state.SetPriority(input.key, TpchQ12ClassifyPriority(input.orderpriority,
                                                         input.priority_is_null));
  });
  initial_scan->ScanLineitems([&](const TpchQ12LineitemInput &input) {
    const auto priority = state.PriorityOf(input.orderkey);
    if (priority == TpchQ12PriorityClass::Absent ||
        !TpchQ12LineitemQualifies(input.commitdate_days, input.receiptdate_days,
                                  input.shipdate_days))
      return;
    AddToCell(state, input.receiptdate_days, input.shipmode,
              Contribution(priority, +1));
  });
  return state;
}

std::vector<core::CalibrationWrite> TpchQ12Module::CalibrationWrites() const {
  return TpchQ12CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
