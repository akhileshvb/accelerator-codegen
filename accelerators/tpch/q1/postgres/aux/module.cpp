#include "tpch/q1/postgres/aux/module.hpp"
#include "tpch/q1/calibration_writes.hpp"

#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include "tpch/q1/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using Q1FenwickState = ::accelerator_codegen::Q1FenwickState;

constexpr int32_t kEpoch = 719468;
// The single state image's partition id.
constexpr core::PartitionId kStatePartition = 0;

constexpr int64_t DaysFromCivil(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - kEpoch;
}

constexpr int64_t kQ1BaseDays = DaysFromCivil(1998, 12, 1);

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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return true;
  }
  return false;
}

bool RawDecimalCell(const core::AuxRow &row, std::size_t index, int64_t &raw,
                    int32_t &scale) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
    raw = decimal.value;
    scale = decimal.scale;
    return true;
  }
  return false;
}

bool DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  days = static_cast<int32_t>(value);
  return true;
}

int64_t ScaleToRaw(int64_t raw, int32_t scale, int32_t target_scale) {
  if (scale == target_scale)
    return raw;
  if (scale < target_scale) {
    int64_t factor = 1;
    for (int32_t i = scale; i < target_scale; ++i)
      factor *= 10;
    return raw * factor;
  }
  int64_t factor = 1;
  for (int32_t i = target_scale; i < scale; ++i)
    factor *= 10;
  return raw / factor;
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

bool ReadLineitem(const core::AuxRow &row, TpchQ1LineitemDelta &delta,
                  int8_t sign) {
  int32_t shipdate_days = 0;
  int64_t quantity_raw = 0, extended_raw = 0, discount_raw = 0, tax_raw = 0;
  int32_t quantity_scale = 0, extended_scale = 0, discount_scale = 0,
          tax_scale = 0;
  std::string returnflag, linestatus;
  if (!DateCell(row, 0, shipdate_days) ||
      !RawDecimalCell(row, 1, quantity_raw, quantity_scale) ||
      !RawDecimalCell(row, 2, extended_raw, extended_scale) ||
      !RawDecimalCell(row, 3, discount_raw, discount_scale) ||
      !RawDecimalCell(row, 4, tax_raw, tax_scale) ||
      !StringCell(row, 5, returnflag) || !StringCell(row, 6, linestatus) ||
      returnflag.size() != 1 || linestatus.size() != 1)
    return false;
  delta.shipdate_days = shipdate_days;
  delta.group_key = ::accelerator_codegen::PackQ1GroupKey(returnflag[0],
                                                            linestatus[0]);
  delta.quantity_raw = ScaleToRaw(quantity_raw, quantity_scale, 2);
  delta.extended_price_raw = ScaleToRaw(extended_raw, extended_scale, 2);
  delta.discount_raw = ScaleToRaw(discount_raw, discount_scale, 2);
  delta.tax_raw = ScaleToRaw(tax_raw, tax_scale, 2);
  delta.sign = sign;
  return true;
}

} // namespace

void TpchQ1InitialScan::ScanDayGroupAggregates(
    const std::function<void(const TpchQ1DayGroupAggregate &)> &consumer) {
  // Row-scan fallback: aggregate (day, group) cells here.
  std::map<std::tuple<int32_t, char, char>, TpchQ1DayGroupAggregate> cells;
  ScanLineitems([&](const TpchQ1LineitemInput &input) {
    auto &cell = cells[{input.shipdate_days, input.returnflag, input.linestatus}];
    cell.shipdate_days = input.shipdate_days;
    cell.returnflag = input.returnflag;
    cell.linestatus = input.linestatus;
    const auto stats = ::accelerator_codegen::Q1RowContribution(
        ScaleToRaw(input.quantity_raw, input.quantity_scale, 2),
        ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2),
        ScaleToRaw(input.discount_raw, input.discount_scale, 2),
        ScaleToRaw(input.tax_raw, input.tax_scale, 2), 1);
    cell.count += stats.count;
    cell.sum_qty_raw += stats.sum_qty;
    cell.sum_base_price_raw += stats.sum_base_price;
    cell.sum_discount_raw += stats.sum_discount;
    cell.sum_disc_price_raw += stats.sum_disc_price;
    cell.sum_charge_raw += stats.sum_charge;
  });
  for (const auto &entry : cells)
    consumer(entry.second);
}

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
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
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
  if (!buffer.RequiresRebuild() && buffer.Deltas().empty())
    return outcome;

  // The state is one copy-on-write Fenwick tree: applying a delta is always
  // cheaper than any rebuild, so a rebuild is chosen only when the deltas
  // could not be read (or the state is missing) or when it is forced.
  std::shared_ptr<Q1FenwickState> next;
  core::Version version = 0;
  if (!buffer.RequiresRebuild()) {
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kStatePartition});
    for (const auto &image : snapshot.partitions) {
      const auto *typed = AsQ1Image(image);
      if (image.partition == kStatePartition && typed && typed->state) {
        next = std::make_shared<Q1FenwickState>(*typed->state); // COW copy
        version = image.version;
      }
    }
  }
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, next != nullptr,
       false, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (!next || decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta ||
                             decision.action == core::MaintenanceAction::RebuildPartition
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto &delta : buffer.Deltas())
    ::accelerator_codegen::Q1FenwickAdd(
        *next, delta.shipdate_days, delta.group_key,
        ::accelerator_codegen::Q1RowContribution(
            delta.quantity_raw, delta.extended_price_raw, delta.discount_raw,
            delta.tax_raw, delta.sign));

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

namespace {

// DELTA of the template: the cutoff day is 1998-12-01 - DELTA, computed in
// 64 bits (any int32 DELTA, negative or huge, stays exact). NULL: no row
// qualifies, so the grouped result is empty.
bool CutoffOf(const core::QueryArguments &arguments, int64_t &cutoff_days) {
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<int64_t>(arguments.values[0].payload))
    return false;
  cutoff_days = static_cast<int64_t>(kQ1BaseDays) -
                std::get<int64_t>(arguments.values[0].payload);
  return true;
}

} // namespace

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

void TpchQ1Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  int64_t cutoff_days = 0;
  if (!CutoffOf(arguments, cutoff_days))
    return;
  EmitResult(::accelerator_codegen::Q1FenwickPrefixRows(*BuildState(services),
                                                        cutoff_days),
             sink);
}

std::shared_ptr<::accelerator_codegen::Q1FenwickState>
TpchQ1Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q1 requires a typed initial scan source");
  auto state = std::make_shared<::accelerator_codegen::Q1FenwickState>();
  initial_scan->ScanDayGroupAggregates([&](const TpchQ1DayGroupAggregate &agg) {
    ::accelerator_codegen::Q1DayGroupStats stats;
    stats.count = agg.count;
    stats.sum_qty = agg.sum_qty_raw;
    stats.sum_base_price = agg.sum_base_price_raw;
    stats.sum_discount = agg.sum_discount_raw;
    stats.sum_disc_price = agg.sum_disc_price_raw;
    stats.sum_charge = agg.sum_charge_raw;
    ::accelerator_codegen::Q1FenwickAdd(
        *state, agg.shipdate_days,
        ::accelerator_codegen::PackQ1GroupKey(agg.returnflag, agg.linestatus),
        stats);
  });
  return state;
}

void TpchQ1Module::EmitResult(
    const std::vector<::accelerator_codegen::Q1OutputRow> &rows,
    core::AuxRowSink &sink) const {
  // Numeric columns are emitted as PostgreSQL numeric text (exact, any
  // width); PgSink converts text to numeric for NUMERIC result columns.
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
