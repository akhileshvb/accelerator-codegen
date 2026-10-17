#include "tpch/q1/mysql/aux/module.hpp"
#include "tpch/q1/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include "tpch/q1/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using Q1FenwickState = ::accelerator_codegen::Q1FenwickState;

// Deltas per slice (~4 us); below it maintenance folds them on the calling thread.
constexpr std::size_t kDeltaGrain = 4096;
// The single state image's partition id.
constexpr core::PartitionId kStatePartition = 0;

constexpr int64_t kQ1BaseDays = core::sql_date::DaysFromCivil(1998, 12, 1);

::accelerator_codegen::Q1DayGroupStats StatsOf(const TpchQ1DayGroupAggregate &agg) {
  ::accelerator_codegen::Q1DayGroupStats stats;
  stats.count = agg.count;
  stats.sum_qty = agg.sum_qty_raw;
  stats.sum_base_price = agg.sum_base_price_raw;
  stats.sum_discount = agg.sum_discount_raw;
  stats.sum_disc_price = agg.sum_disc_price_raw;
  stats.sum_charge = agg.sum_charge_raw;
  return stats;
}

std::shared_ptr<const TpchQ1StateImage>
MakeStateImage(std::shared_ptr<const Q1FenwickState> state) {
  auto image = std::make_shared<TpchQ1StateImage>();
  image->state = std::move(state);
  return image;
}

const TpchQ1StateImage *AsQ1Image(const core::StateImage &image) {
  return image.As<TpchQ1StateImage>();
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// Sums per (shipdate day, group): the unit both build and maintenance fold
// rows into before touching the Fenwick tree.
using DayGroupCells = std::unordered_map<int64_t, ::accelerator_codegen::Q1DayGroupStats>;

int64_t CellKey(int32_t day, ::accelerator_codegen::Q1GroupKey group) {
  return (static_cast<int64_t>(day) << 16) | group;
}

void AddToCell(DayGroupCells &cells, int32_t day,
               ::accelerator_codegen::Q1GroupKey group,
               const ::accelerator_codegen::Q1DayGroupStats &stats) {
  ::accelerator_codegen::AddQ1Stats(cells[CellKey(day, group)], stats);
}

void MergeCells(DayGroupCells &into, DayGroupCells &&from) {
  for (const auto &[key, stats] : from)
    ::accelerator_codegen::AddQ1Stats(into[key], stats);
}

// Adds are exact integer sums, so the cell order does not change the state.
void ApplyCells(const DayGroupCells &cells, Q1FenwickState &state) {
  for (const auto &[key, stats] : cells)
    ::accelerator_codegen::Q1FenwickAdd(
        state, static_cast<int32_t>(key >> 16),
        static_cast<::accelerator_codegen::Q1GroupKey>(key & 0xFFFF), stats);
}

DayGroupCells FoldDeltas(core::AuxEngineServices &services,
                         const std::vector<TpchQ1LineitemDelta> &deltas) {
  DayGroupCells cells;
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, cells,
      [&](DayGroupCells &into, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
          const auto &delta = deltas[i];
          AddToCell(into, delta.shipdate_days, delta.group_key,
                    ::accelerator_codegen::Q1RowContribution(
                        delta.quantity_raw, delta.extended_price_raw,
                        delta.discount_raw, delta.tax_raw, delta.sign));
        }
      },
      MergeCells);
  return cells;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ1LineitemDelta &delta,
                  int8_t sign) {
  int32_t shipdate_days = 0;
  int64_t quantity_raw = 0, extended_raw = 0, discount_raw = 0, tax_raw = 0;
  int32_t quantity_scale = 0, extended_scale = 0, discount_scale = 0,
          tax_scale = 0;
  std::string returnflag, linestatus;
  if (!core::DateCell(row, 0, shipdate_days) ||
      !core::DecimalCell(row, 1, quantity_raw, quantity_scale) ||
      !core::DecimalCell(row, 2, extended_raw, extended_scale) ||
      !core::DecimalCell(row, 3, discount_raw, discount_scale) ||
      !core::DecimalCell(row, 4, tax_raw, tax_scale) ||
      !core::StringCell(row, 5, returnflag) || !core::StringCell(row, 6, linestatus) ||
      returnflag.size() != 1 || linestatus.size() != 1)
    return false;
  delta.shipdate_days = shipdate_days;
  delta.group_key = ::accelerator_codegen::PackQ1GroupKey(returnflag[0],
                                                            linestatus[0]);
  delta.quantity_raw = core::ScaleToRaw(quantity_raw, quantity_scale, 2);
  delta.extended_price_raw = core::ScaleToRaw(extended_raw, extended_scale, 2);
  delta.discount_raw = core::ScaleToRaw(discount_raw, discount_scale, 2);
  delta.tax_raw = core::ScaleToRaw(tax_raw, tax_scale, 2);
  delta.sign = sign;
  return true;
}

// DELTA of the template: the cutoff day is 1998-12-01 - DELTA, computed in
// 64 bits (any int32 DELTA, negative or huge, stays exact). NULL: no row
// qualifies, so the grouped result is empty.
bool CutoffOf(const core::QueryArguments &arguments, int64_t &cutoff_days) {
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<int64_t>(arguments.values[0].payload))
    return false;
  cutoff_days = static_cast<int64_t>(kQ1BaseDays) -
                std::get<int64_t>(arguments.values[0].payload);
  // MySQL: DATE_SUB outside 0000-01-01 .. 9999-12-31 is NULL: no row qualifies.
  return cutoff_days >= core::sql_date::DaysFromCivil(0, 1, 1) &&
         cutoff_days <= core::sql_date::DaysFromCivil(9999, 12, 31);
}

} // namespace

int32_t TpchQ1CutoffDays(int32_t day_offset) {
  return kQ1BaseDays - day_offset;
}

bool TpchQ1TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ1TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ1TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ1LineitemDelta> &TpchQ1TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ1TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ1TxnBuffer::AddDelta(TpchQ1LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ1Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ1Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3, 4, 5, 6},
           {10, 4, 5, 6, 7, 8, 9}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ1Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ1TxnBuffer>();
}

void TpchQ1Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  builder.Write(core::StateImage::Make(
      core::TpchQ1Layout::kAccelerator, kStatePartition, 0,
      MakeStateImage(BuildState(services))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ1Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ1Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ1TxnBuffer &>(base_buffer);
  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ1LineitemDelta delta;
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
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      TpchQ1LineitemDelta old_delta, new_delta;
      if (!ReadLineitem(mutation.old_rows[i], old_delta, -1) ||
          !ReadLineitem(mutation.new_rows[i], new_delta, 1)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddDelta(old_delta);
      buffer.AddDelta(new_delta);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ1Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ1TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.Deltas().empty())
    return outcome;

  // The state is one copy-on-write Fenwick tree: applying a delta is always
  // cheaper than any rebuild, so a rebuild is chosen only when the state is
  // missing or when it is forced.
  std::shared_ptr<Q1FenwickState> next;
  core::Version version = 0;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {kStatePartition});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ1Image(image);
    if (image.partition == kStatePartition && typed && typed->state) {
      next = std::make_shared<Q1FenwickState>(*typed->state); // COW copy
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

  // The tree is one COW structure, so the adds stay serial; the deltas are
  // folded in parallel into per-cell sums first (one tree add per cell).
  ApplyCells(FoldDeltas(services, buffer.Deltas()), *next);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ1Layout::kAccelerator, kStatePartition, version,
      MakeStateImage(std::move(next))));
  return outcome;
}

bool TpchQ1Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ1Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  int64_t cutoff_days = 0;
  if (!CutoffOf(arguments, cutoff_days))
    return;

  // ReadHit: validity + the one immutable state image under one visibility.
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadHitPartitions(
      visibility, {Id(), {}}, {kStatePartition});
  const auto *typed = snapshot.available && snapshot.partitions.size() == 1
                          ? AsQ1Image(snapshot.partitions[0])
                          : nullptr;
  if (!typed || !typed->state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(::accelerator_codegen::Q1FenwickPrefixRows(*typed->state, cutoff_days),
             sink);
}

std::shared_ptr<::accelerator_codegen::Q1FenwickState>
TpchQ1Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q1 requires a typed initial scan source");
  // Integer sums, so the cells add up the same in any partition order.
  const auto cells = core::PartitionedBuild<DayGroupCells>(
      services, [] { return DayGroupCells{}; },
      [&](std::size_t partitions, auto &&sink) {
        initial_scan->ScanDayGroupAggregates(
            services, partitions,
            [&](std::size_t partition, const TpchQ1DayGroupAggregate &agg) {
              sink(partition, core::SingleRowBatch<TpchQ1DayGroupAggregate>{agg});
            });
      },
      [](DayGroupCells &into, const TpchQ1DayGroupAggregate &agg) {
        AddToCell(into, agg.shipdate_days,
                  ::accelerator_codegen::PackQ1GroupKey(agg.returnflag, agg.linestatus),
                  StatsOf(agg));
      },
      MergeCells);
  auto state = std::make_shared<::accelerator_codegen::Q1FenwickState>();
  ApplyCells(cells, *state);
  return state;
}

void TpchQ1Module::EmitResult(
    const std::vector<::accelerator_codegen::Q1OutputRow> &rows,
    core::AuxRowSink &sink) const {
  // Numeric columns are emitted as exact DECIMAL text (any width: sums can
  // exceed int64); the TSV sink prints text as is.
  sink.Begin({{{"l_returnflag", core::AuxType::String, false},
               {"l_linestatus", core::AuxType::String, false},
               {"sum_qty", core::AuxType::String, false},
               {"sum_base_price", core::AuxType::String, false},
               {"sum_disc_price", core::AuxType::String, false},
               {"sum_charge", core::AuxType::String, false},
               {"avg_qty", core::AuxType::String, false},
               {"avg_price", core::AuxType::String, false},
               {"avg_disc", core::AuxType::String, false},
               {"count_order", core::AuxType::Int64, false}}});
  namespace pgn = ::accelerator_codegen::pg_numeric;
  const auto numeric = [](std::string text) {
    return core::AuxValue::Of(core::AuxType::String, std::move(text));
  };
  for (const auto &row : rows) {
    sink.WriteRow(
        {{core::AuxValue::Of(
              core::AuxType::String,
              ::accelerator_codegen::UnpackQ1Returnflag(row.group_key)),
          core::AuxValue::Of(
              core::AuxType::String,
              ::accelerator_codegen::UnpackQ1Linestatus(row.group_key)),
          numeric(pgn::ToText(row.sum_qty, 2)),
          numeric(pgn::ToText(row.sum_base_price, 2)),
          numeric(pgn::ToText(row.sum_disc_price, 4)),
          numeric(pgn::ToText(row.sum_charge, 6)),
          numeric(pgn::ToText(row.avg_qty)),
          numeric(pgn::ToText(row.avg_price)),
          numeric(pgn::ToText(row.avg_disc)),
          core::AuxValue::Of(core::AuxType::Int64, row.count_order)}});
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ1Module::CalibrationWrites() const {
  return TpchQ1CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
