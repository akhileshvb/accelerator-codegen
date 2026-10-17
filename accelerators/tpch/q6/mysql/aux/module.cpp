#include "tpch/q6/mysql/aux/module.hpp"
#include "tpch/q6/calibration_writes.hpp"
#include "prims/pg_numeric.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <iterator>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

using Q6State = ::accelerator_codegen::Q6State;

// The single state image's partition id.
constexpr core::PartitionId kStatePartition = 0;
// Deltas per slice (~4 us); below it a commit sums its deltas inline.
constexpr std::size_t kDeltaGrain = 4096;
// Cells per slice of the build's sort of the partition cells.
constexpr std::size_t kCellGrain = 16384;

std::shared_ptr<const TpchQ6StateImage>
MakeStateImage(std::shared_ptr<const Q6State> state) {
  auto image = std::make_shared<TpchQ6StateImage>();
  image->state = std::move(state);
  return image;
}

const TpchQ6StateImage *AsQ6Image(const core::StateImage &image) {
  return image.As<TpchQ6StateImage>();
}

bool ReadLineitem(const core::AuxRow &row, TpchQ6LineitemDelta &delta,
                  int8_t sign) {
  int32_t shipdate_days = 0;
  int64_t discount_raw = 0, quantity_raw = 0, extended_raw = 0;
  int32_t discount_scale = 0, quantity_scale = 0, extended_scale = 0;
  if (!core::DateCell(row, 0, shipdate_days) ||
      !core::DecimalCell(row, 1, discount_raw, discount_scale) ||
      !core::DecimalCell(row, 2, quantity_raw, quantity_scale) ||
      !core::DecimalCell(row, 3, extended_raw, extended_scale))
    return false;
  discount_raw = core::ScaleToRaw(discount_raw, discount_scale, 2);
  quantity_raw = core::ScaleToRaw(quantity_raw, quantity_scale, 2);
  extended_raw = core::ScaleToRaw(extended_raw, extended_scale, 2);
  delta.shipdate_days = shipdate_days;
  delta.discount_raw = discount_raw;
  delta.quantity_raw = quantity_raw;
  delta.revenue = ::accelerator_codegen::Q6RowRevenue(extended_raw, discount_raw);
  delta.sign = sign;
  return true;
}

// A numeric template argument (DOUBLE PRECISION in PostgreSQL, an exact
// decimal or integer elsewhere) as floor(100 v).
bool NumericArg(const core::AuxValue &value,
                ::accelerator_codegen::Q6Scaled100 &out) {
  if (value.is_null)
    return false;
  if (std::holds_alternative<double>(value.payload))
    return ::accelerator_codegen::Q6Scaled100FromDouble(
        std::get<double>(value.payload), out);
  if (std::holds_alternative<int64_t>(value.payload)) {
    out = ::accelerator_codegen::Q6Scaled100FromDecimal(
        std::get<int64_t>(value.payload), 0);
    return true;
  }
  if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(value.payload);
    out = ::accelerator_codegen::Q6Scaled100FromDecimal(decimal.value,
                                                        decimal.scale);
    return true;
  }
  return false;
}

using CellKey = std::tuple<int32_t, int64_t, int64_t>; // day, discount, quantity
struct CellSum {
  ::accelerator_codegen::Q6AccT revenue = 0;
  int64_t count = 0;
};
using CellSums = std::map<CellKey, CellSum>;

void MergeSums(CellSums &into, CellSums &&from) {
  for (const auto &[key, sum] : from) {
    auto &cell = into[key];
    cell.revenue += sum.revenue;
    cell.count += sum.count;
  }
}

// The deltas summed per cell: each slice sums its own range, the slices merge
// in order, and the integer sums equal the serial ones.
CellSums SumDeltas(const core::AuxEngineServices &services,
                   const std::vector<TpchQ6LineitemDelta> &deltas) {
  CellSums cells;
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, cells,
      [&](CellSums &into, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
          const auto &delta = deltas[i];
          auto &cell = into[CellKey{delta.shipdate_days, delta.discount_raw, delta.quantity_raw}];
          cell.revenue += delta.sign * delta.revenue;
          cell.count += delta.sign;
        }
      },
      MergeSums);
  return cells;
}

bool CellLess(const TpchQ6CellAggregate &l, const TpchQ6CellAggregate &r) {
  return std::tie(l.shipdate_days, l.discount_raw, l.quantity_raw) <
         std::tie(r.shipdate_days, r.discount_raw, r.quantity_raw);
}

} // namespace

bool TpchQ6TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ6TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ6TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ6LineitemDelta> &TpchQ6TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ6TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ6TxnBuffer::AddDelta(TpchQ6LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ6Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ6Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3}, {10, 6, 4, 5}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ6Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ6TxnBuffer>();
}

void TpchQ6Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  builder.Write(core::StateImage::Make(
      core::TpchQ6Layout::kAccelerator, kStatePartition, 0,
      MakeStateImage(BuildState(services))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ6Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ6Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ6TxnBuffer &>(base_buffer);
  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ6LineitemDelta delta;
    if (!ReadLineitem(row, delta, sign)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddDelta(delta);
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
    // Any column change, shipdate included, is -old +new.
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
TpchQ6Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ6TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.Deltas().empty())
    return outcome;

  // The state is one copy-on-write tree: applying a delta is always cheaper
  // than a rebuild, which is chosen only when the state is missing or when it
  // is forced.
  std::shared_ptr<Q6State> next;
  core::Version version = 0;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ6Image(image);
    if (image.partition == kStatePartition && typed && typed->state) {
      next = std::make_shared<Q6State>(*typed->state); // COW copy
      version = image.version;
    }
  }
  auto request = core::MaintenanceRequest::IncrementalSingleState(
      buffer.Deltas().size(), services.ExpectedQueriesBeforeNextWrite());
  request.incremental_supported = next != nullptr;
  const auto decision = services.ChooseMaintenance(request);
  if (!next || decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  for (const auto &[key, sum] : SumDeltas(services, buffer.Deltas()))
    ::accelerator_codegen::Q6AddCell(*next, std::get<0>(key), std::get<1>(key),
                                     std::get<2>(key), sum.revenue, sum.count);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ6Layout::kAccelerator, kStatePartition, version,
      MakeStateImage(std::move(next))));
  return outcome;
}

bool TpchQ6Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ6Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  // ReadHit: validity + the one immutable state image under one visibility.
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, {kStatePartition});
  const auto *typed = snapshot.available && snapshot.partitions.size() == 1
                          ? AsQ6Image(snapshot.partitions[0])
                          : nullptr;
  if (!typed || !typed->state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  Answer(*typed->state, arguments, sink);
}

std::shared_ptr<::accelerator_codegen::Q6State>
TpchQ6Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q6 requires a typed initial scan source");
  // A cell comes once per partition: gather them all, sort, add equal cells up.
  using Cells = std::vector<TpchQ6CellAggregate>;
  auto cells = core::PartitionedBuild<Cells>(
      services, [] { return Cells{}; },
      [&](std::size_t partitions, auto &&sink) {
        initial_scan->ScanCellAggregatesPartitioned(
            services, partitions,
            [&](std::size_t partition, const TpchQ6CellAggregate &cell) {
              sink(partition, core::SingleRowBatch<TpchQ6CellAggregate>{cell});
            });
      },
      [](Cells &into, const TpchQ6CellAggregate &cell) { into.push_back(cell); },
      [](Cells &into, Cells &&from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()),
                    std::make_move_iterator(from.end()));
      });
  core::ParallelStableSort(services, cells, CellLess, kCellGrain);
  auto state = std::make_shared<Q6State>();
  for (std::size_t i = 0; i < cells.size();) {
    auto sum = cells[i];
    // Sorted, so a cell that does not sort after `sum` is the same cell.
    for (++i; i < cells.size() && !CellLess(sum, cells[i]); ++i) {
      sum.count += cells[i].count;
      sum.revenue_raw += cells[i].revenue_raw;
    }
    ::accelerator_codegen::Q6AddCell(*state, sum.shipdate_days, sum.discount_raw,
                                     sum.quantity_raw, sum.revenue_raw, sum.count);
  }
  return state;
}

void TpchQ6Module::Answer(const Q6State &state,
                          const core::QueryArguments &arguments,
                          core::AuxRowSink &sink) const {
  // A NULL argument makes the predicate NULL: no row qualifies, and
  // SUM over no rows is one NULL row.
  ::accelerator_codegen::Q6Result result;
  ::accelerator_codegen::Q6Scaled100 discount, quantity;
  int64_t start_days = 0;
  ::accelerator_codegen::Q6Predicate predicate;
  if (arguments.values.size() >= 3 && !arguments.values[0].is_null &&
      NumericArg(arguments.values[1], discount) &&
      NumericArg(arguments.values[2], quantity)) {
    const auto &date = arguments.values[0].payload;
    bool have_date = true;
    if (std::holds_alternative<core::AuxDate>(date))
      start_days = std::get<core::AuxDate>(date).days_since_epoch;
    else if (std::holds_alternative<int64_t>(date))
      start_days = std::get<int64_t>(date);
    else
      have_date = false;
    if (have_date && ::accelerator_codegen::Q6ResolvePredicate(
                         start_days, discount, quantity, predicate))
      result = ::accelerator_codegen::Q6Query(state, predicate);
  }
  EmitResult(result, sink);
}

void TpchQ6Module::EmitResult(const ::accelerator_codegen::Q6Result &result,
                              core::AuxRowSink &sink) const {
  // SUM(l_extendedprice * l_discount): exact NUMERIC text at scale 4 (any
  // width), NULL when no row qualified.
  sink.Begin({{{"revenue", core::AuxType::String, true}}});
  if (result.count == 0)
    sink.WriteRow({{core::AuxValue::Null(core::AuxType::String)}});
  else
    sink.WriteRow({{core::AuxValue::Of(
        core::AuxType::String,
        ::accelerator_codegen::pg_numeric::ToText(result.revenue, 4))}});
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ6Module::CalibrationWrites() const {
  return TpchQ6CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
