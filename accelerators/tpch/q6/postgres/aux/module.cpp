#include "tpch/q6/postgres/aux/module.hpp"
#include "tpch/q6/calibration_writes.hpp"
#include "prims/pg_numeric.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "prims/flat_hash_map.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/partitioned_rows.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using Q6State = ::accelerator_codegen::Q6State;

// The single state image's partition id.
constexpr core::PartitionId kStatePartition = 0;

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

std::shared_ptr<const TpchQ6StateImage>
MakeStateImage(std::shared_ptr<const Q6State> state) {
  auto image = std::make_shared<TpchQ6StateImage>();
  image->state = std::move(state);
  return image;
}

const TpchQ6StateImage *AsQ6Image(const core::StateImage &image) {
  return image.As<TpchQ6StateImage>();
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ6LineitemDelta &delta,
                  int8_t sign) {
  int32_t shipdate_days = 0;
  int64_t discount_raw = 0, quantity_raw = 0, extended_raw = 0;
  int32_t discount_scale = 0, quantity_scale = 0, extended_scale = 0;
  if (!DateCell(row, 0, shipdate_days) ||
      !RawDecimalCell(row, 1, discount_raw, discount_scale) ||
      !RawDecimalCell(row, 2, quantity_raw, quantity_scale) ||
      !RawDecimalCell(row, 3, extended_raw, extended_scale))
    return false;
  discount_raw = ScaleToRaw(discount_raw, discount_scale, 2);
  quantity_raw = ScaleToRaw(quantity_raw, quantity_scale, 2);
  extended_raw = ScaleToRaw(extended_raw, extended_scale, 2);
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


// Rows per slice below which a commit's deltas are aggregated inline.
constexpr std::size_t kDeltaGrain = 4096;

// Pre-aggregated (shipdate, discount, quantity) cell: the sum of
// price * discount (scale 4) and the row count.
struct CellAggregate {
  int32_t shipdate_days = 0;
  int64_t discount_raw = 0; // scale 2
  int64_t quantity_raw = 0; // scale 2
  int64_t count = 0;
  ::accelerator_codegen::Q6AccT revenue_raw = 0;
};

// One build partition's cells. The discount and quantity domains are tiny, so
// a cell is keyed by their (partition-local) ids.
class CellTable {
public:
  void AddRow(const TpchQ6LineitemInput &input) {
    const auto discount = ScaleToRaw(input.discount_raw, input.discount_scale, 2);
    Add({input.shipdate_days, discount,
         ScaleToRaw(input.quantity_raw, input.quantity_scale, 2), 1,
         ::accelerator_codegen::Q6RowRevenue(
             ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2),
             discount)});
  }

  void Merge(CellTable &&from) {
    for (const auto entry : from.cells)
      Add(entry.second);
  }

  // The cells ordered by (day, discount, quantity), as the state build wants them.
  std::vector<const CellAggregate *> Ordered() const {
    std::vector<const CellAggregate *> ordered;
    ordered.reserve(cells.size());
    for (const auto entry : cells)
      ordered.push_back(&entry.second);
    std::sort(ordered.begin(), ordered.end(),
              [](const CellAggregate *l, const CellAggregate *r) {
                return std::tie(l->shipdate_days, l->discount_raw, l->quantity_raw) <
                       std::tie(r->shipdate_days, r->discount_raw, r->quantity_raw);
              });
    return ordered;
  }

private:
  static constexpr int kIdBits = 20;

  void Add(const CellAggregate &add) {
    const int64_t d = discounts.Intern(add.discount_raw);
    const int64_t q = quantities.Intern(add.quantity_raw);
    if ((d | q) >> kIdBits)
      throw std::length_error("TPC-H Q6: too many distinct discounts or quantities");
    const auto day = static_cast<uint64_t>(static_cast<uint32_t>(add.shipdate_days));
    auto &cell = cells[static_cast<int64_t>((day << (2 * kIdBits)) | (d << kIdBits) | q)];
    cell.shipdate_days = add.shipdate_days;
    cell.discount_raw = add.discount_raw;
    cell.quantity_raw = add.quantity_raw;
    cell.count += add.count;
    cell.revenue_raw += add.revenue_raw;
  }

  ::accelerator_codegen::Q6ValueDict discounts, quantities;
  core::FlatHashMap<CellAggregate> cells;
};

// A commit's deltas summed per (day, discount, quantity).
using DeltaCells = std::map<std::tuple<int32_t, int64_t, int64_t>,
                            std::pair<::accelerator_codegen::Q6AccT, int64_t>>;

void AddDeltas(DeltaCells &into, const std::vector<TpchQ6LineitemDelta> &deltas,
               std::size_t begin, std::size_t end) {
  for (std::size_t i = begin; i < end; ++i) {
    const auto &delta = deltas[i];
    auto &cell = into[{delta.shipdate_days, delta.discount_raw, delta.quantity_raw}];
    cell.first += delta.sign * delta.revenue;
    cell.second += delta.sign;
  }
}

void MergeDeltas(DeltaCells &into, DeltaCells &&from) {
  for (const auto &[key, cell] : from) {
    auto &sum = into[key];
    sum.first += cell.first;
    sum.second += cell.second;
  }
}

// Applies the summed deltas day by day: one Fenwick path walk per day.
void ApplyDeltaCells(Q6State &state, const DeltaCells &cells) {
  int32_t day = 0;
  std::vector<::accelerator_codegen::Q6DayCell> day_cells;
  const auto flush = [&] {
    if (!day_cells.empty())
      ::accelerator_codegen::Q6AddDayCells(state, day, day_cells);
    day_cells.clear();
  };
  for (const auto &[key, cell] : cells) {
    if (std::get<0>(key) != day)
      flush();
    day = std::get<0>(key);
    day_cells.push_back({std::get<1>(key), std::get<2>(key), cell.first, cell.second});
  }
  flush();
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
  if (!buffer.RequiresRebuild() && buffer.Deltas().empty())
    return outcome;

  // The state is one copy-on-write tree: applying a delta is always cheaper
  // than a rebuild, which is chosen only when the deltas could not be read
  // (or the state is missing) or when it is forced.
  std::shared_ptr<Q6State> next;
  core::Version version = 0;
  if (!buffer.RequiresRebuild()) {
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kStatePartition});
    for (const auto &image : snapshot.partitions) {
      const auto *typed = AsQ6Image(image);
      if (image.partition == kStatePartition && typed && typed->state) {
        next = std::make_shared<Q6State>(*typed->state); // COW copy
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

  // Sum the deltas per cell (slices in parallel), then walk each day once.
  const auto &deltas = buffer.Deltas();
  DeltaCells cells;
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, cells,
      [&](DeltaCells &into, std::size_t begin, std::size_t end) {
        AddDeltas(into, deltas, begin, end);
      },
      MergeDeltas);
  ApplyDeltaCells(*next, cells);

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

void TpchQ6Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  Answer(*BuildState(services), arguments, sink);
}

std::shared_ptr<::accelerator_codegen::Q6State>
TpchQ6Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q6 requires a typed initial scan source");
  const auto table = core::PartitionedBuild<CellTable>(
      services, [] { return CellTable{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitemsPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ6LineitemInput &input) {
              sink(partition, OneRowBatch<TpchQ6LineitemInput>{input});
            });
      },
      [](CellTable &into, const TpchQ6LineitemInput &input) { into.AddRow(input); },
      [](CellTable &into, CellTable &&from) { into.Merge(std::move(from)); },
      BuildPartitions(services));
  auto state = std::make_shared<Q6State>();
  // A day's cells go in together: one Fenwick path walk per day.
  int32_t day = 0;
  std::vector<::accelerator_codegen::Q6DayCell> cells;
  const auto flush = [&] {
    if (!cells.empty())
      ::accelerator_codegen::Q6AddDayCells(*state, day, cells);
    cells.clear();
  };
  for (const auto *cell : table.Ordered()) {
    if (cell->shipdate_days != day)
      flush();
    day = cell->shipdate_days;
    cells.push_back({cell->discount_raw, cell->quantity_raw, cell->revenue_raw, cell->count});
  }
  flush();
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
