#include "tpch/q1/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include "duckdb/common/types/hugeint.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

using Q1DayPartition = ::accelerator_codegen::Q1DayPartition;
using Q1GroupKey = ::accelerator_codegen::Q1GroupKey;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kDayMagic = 0x44315151; // Q1D
constexpr int32_t kEpoch = 719468;

constexpr int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

constexpr int32_t kQ1BaseDays = DaysFromCivil(1998, 12, 1);
// TPC-H lineitem shipdate lower bound (1992-01-02).
constexpr int32_t kTpchMinLineitemShipdateDays = DaysFromCivil(1992, 1, 2);

void CollectQ1DayPartitions(int32_t cutoff_days,
                            std::vector<core::PartitionId> &partitions) {
  partitions.clear();
  if (cutoff_days < kTpchMinLineitemShipdateDays)
    return;
  partitions.reserve(static_cast<std::size_t>(cutoff_days -
                                              kTpchMinLineitemShipdateDays + 1));
  for (int32_t day = kTpchMinLineitemShipdateDays; day <= cutoff_days; ++day)
    partitions.push_back(core::TpchQ1Layout::PartitionFromDays(day));
}

void WriteQ1Acc(ByteWriter &writer, ::accelerator_codegen::Q1AccT value) {
#if defined(__SIZEOF_INT128__)
  writer.U64(static_cast<uint64_t>(value));
  writer.U64(static_cast<uint64_t>(value >> 64));
#else
  writer.I64(value);
#endif
}

bool ReadQ1Acc(ByteReader &reader, ::accelerator_codegen::Q1AccT &value) {
#if defined(__SIZEOF_INT128__)
  uint64_t lo = 0;
  uint64_t hi = 0;
  if (!reader.U64(lo) || !reader.U64(hi)) {
    return false;
  }
  value = static_cast<::accelerator_codegen::Q1AccT>(hi) << 64;
  value |= static_cast<::accelerator_codegen::Q1AccT>(lo);
  return true;
#else
  return reader.I64(value);
#endif
}

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
MakeStateImage(int32_t shipdate_days,
               const std::shared_ptr<const Q1DayPartition> &partition) {
  auto image = std::make_shared<TpchQ1StateImage>();
  image->shipdate_days = shipdate_days;
  image->partition = partition;
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

void ApplyLineitemInput(const TpchQ1LineitemInput &input,
                        Q1DayPartition &day) {
  const auto group_key = ::accelerator_codegen::PackQ1GroupKey(
      input.returnflag, input.linestatus);
  auto &group = day.groups[group_key];
  ::accelerator_codegen::ApplyQ1RowContribution(
      group, ScaleToRaw(input.quantity_raw, input.quantity_scale, 2),
      ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2),
      ScaleToRaw(input.discount_raw, input.discount_scale, 2),
      ScaleToRaw(input.tax_raw, input.tax_scale, 2), 1);
}

int64_t HugeintLower(const duckdb::hugeint_t &value) {
  return static_cast<int64_t>(value.lower);
}

} // namespace

std::vector<uint8_t> EncodeTpchQ1Day(const Q1DayPartition &partition) {
  std::vector<Q1GroupKey> keys;
  keys.reserve(partition.groups.size());
  for (const auto &entry : partition.groups)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());

  ByteWriter writer;
  core::WriteImageHeader(writer, kDayMagic, kVersion, static_cast<uint32_t>(keys.size()));
  for (const auto key : keys) {
    const auto &stats = partition.groups.at(key);
    writer.U16(key);
    writer.I64(stats.count);
    writer.I64(stats.sum_qty);
    writer.I64(stats.sum_base_price);
    WriteQ1Acc(writer, stats.sum_disc_price);
    WriteQ1Acc(writer, stats.sum_charge);
    WriteQ1Acc(writer, stats.sum_discount);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ1Day(const std::vector<uint8_t> &bytes, Q1DayPartition &partition) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kDayMagic, kVersion, count))
    return false;
  partition.groups.clear();
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t key = 0;
    ::accelerator_codegen::Q1DayGroupStats stats;
    if (!reader.U16(key) || !reader.I64(stats.count) ||
        !reader.I64(stats.sum_qty) || !reader.I64(stats.sum_base_price) ||
        !ReadQ1Acc(reader, stats.sum_disc_price) || !ReadQ1Acc(reader, stats.sum_charge) ||
        !ReadQ1Acc(reader, stats.sum_discount))
      return false;
    partition.groups.emplace(key, stats);
  }
  return reader.offset == bytes.size();
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
  return {{config.lineitem_table, {0, 1, 2, 3, 4, 5, 6}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ1Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ1TxnBuffer>();
}

void TpchQ1Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  const auto days = BuildState(services);
  for (const auto &entry : days) {
    const auto typed =
        std::make_shared<const Q1DayPartition>(entry.second);
    builder.Write(core::StateImage::Make(
        core::TpchQ1Layout::kAccelerator,
        core::TpchQ1Layout::PartitionFromDays(entry.first), 0,
        MakeStateImage(entry.first, typed)));
  }
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
      if (old_delta.shipdate_days != new_delta.shipdate_days) {
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
  if (buffer.Deltas().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, true, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  std::set<int32_t> touched_days;
  for (const auto &delta : buffer.Deltas())
    touched_days.insert(delta.shipdate_days);

  std::vector<core::PartitionId> partitions;
  partitions.reserve(touched_days.size());
  for (const auto day : touched_days)
    partitions.push_back(core::TpchQ1Layout::PartitionFromDays(day));

  std::map<int32_t, core::Version> versions;
  std::unordered_map<int32_t, Q1DayPartition> state;
  const auto visibility = services.CurrentVisibility();
  const auto snapshot =
      services.StateStore().ReadPartitions(visibility, partitions);
  for (const auto &image : snapshot.partitions) {
    const auto day = core::TpchQ1Layout::DaysFromPartition(image.partition);
    const auto *typed = AsQ1Image(image);
    if (!typed || !typed->partition || typed->shipdate_days != day) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    state[day] = *typed->partition;
    versions[day] = image.version;
  }
  if (versions.size() != touched_days.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto &delta : buffer.Deltas()) {
    auto &group = state[delta.shipdate_days].groups[delta.group_key];
    ::accelerator_codegen::ApplyQ1RowContribution(
        group, delta.quantity_raw, delta.extended_price_raw, delta.discount_raw,
        delta.tax_raw, delta.sign);
    if (group.count == 0)
      state[delta.shipdate_days].groups.erase(delta.group_key);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  for (const auto day : touched_days) {
    const auto partition = core::TpchQ1Layout::PartitionFromDays(day);
    const auto version = versions.count(day) ? versions.at(day) : 0;
    const auto typed = std::make_shared<const Q1DayPartition>(state.at(day));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ1Layout::kAccelerator, partition, version,
        MakeStateImage(day, typed)));
  }
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
  int32_t day_offset = 0;
  if (arguments.values.empty() || arguments.values[0].is_null)
    return;
  if (std::holds_alternative<int64_t>(arguments.values[0].payload))
    day_offset = static_cast<int32_t>(
        std::get<int64_t>(arguments.values[0].payload));
  else
    return;
  const auto cutoff_days = TpchQ1CutoffDays(day_offset);
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  std::unordered_map<int32_t, Q1DayPartition> days;
  if (!LoadPartitions(services, cutoff_days, days)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(ComposeOutput(days, cutoff_days), sink);
}

void TpchQ1Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  int32_t day_offset = 0;
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<int64_t>(arguments.values[0].payload))
    return;
  day_offset =
      static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  EmitResult(ComposeOutput(BuildState(services), TpchQ1CutoffDays(day_offset)),
             sink);
}

std::unordered_map<int32_t, Q1DayPartition>
TpchQ1Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  std::unordered_map<int32_t, Q1DayPartition> days;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q1 requires a typed initial scan source");

  const bool used_sql = initial_scan->ScanDayGroupAggregates(
      [&](const TpchQ1DayGroupAggregate &agg) {
        const auto group_key = ::accelerator_codegen::PackQ1GroupKey(
            agg.returnflag, agg.linestatus);
        auto &group = days[agg.shipdate_days].groups[group_key];
        group.count += agg.count;
        group.sum_qty += agg.sum_qty_raw;
        group.sum_base_price += agg.sum_base_price_raw;
        group.sum_discount += agg.sum_discount_raw;
        group.sum_disc_price +=
            static_cast<::accelerator_codegen::Q1AccT>(agg.sum_disc_price_raw);
        group.sum_charge +=
            static_cast<::accelerator_codegen::Q1AccT>(agg.sum_charge_raw);
      });
  if (used_sql)
    return days;

  initial_scan->ScanLineitems([&](const TpchQ1LineitemInput &input) {
    ApplyLineitemInput(input, days[input.shipdate_days]);
  });
  return days;
}

std::vector<::accelerator_codegen::Q1OutputRow> TpchQ1Module::ComposeOutput(
    const std::unordered_map<int32_t, Q1DayPartition> &days,
    int32_t cutoff_days) const {
  std::unordered_map<Q1GroupKey, ::accelerator_codegen::Q1MergedGroupStats>
      merged;
  for (const auto &entry : days) {
    if (entry.first > cutoff_days)
      continue;
    for (const auto &group_entry : entry.second.groups)
      ::accelerator_codegen::MergeQ1DayGroupStats(
          merged[group_entry.first], group_entry.second);
  }
  return ::accelerator_codegen::FinalizeQ1MergedGroups(merged);
}

bool TpchQ1Module::LoadPartitions(
    core::AuxEngineServices &services, int32_t cutoff_days,
    std::unordered_map<int32_t, Q1DayPartition> &days,
    std::map<int32_t, core::Version> *versions) const {
  std::vector<core::PartitionId> partitions;
  CollectQ1DayPartitions(cutoff_days, partitions);
  if (partitions.empty())
    return true;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  for (const auto &image : snapshot.partitions) {
    const auto day = core::TpchQ1Layout::DaysFromPartition(image.partition);
    if (day > cutoff_days)
      continue;
    const auto *typed = AsQ1Image(image);
    if (!typed || !typed->partition || typed->shipdate_days != day)
      return false;
    days.emplace(day, *typed->partition);
    if (versions)
      (*versions)[day] = image.version;
  }
  return true;
}

void TpchQ1Module::EmitResult(
    const std::vector<::accelerator_codegen::Q1OutputRow> &rows,
    core::AuxRowSink &sink) const {
  sink.Begin({{{"l_returnflag", core::AuxType::String, false},
               {"l_linestatus", core::AuxType::String, false},
               {"sum_qty", core::AuxType::ScaledDecimal, false},
               {"sum_base_price", core::AuxType::ScaledDecimal, false},
               {"sum_disc_price", core::AuxType::ScaledDecimal, false},
               {"sum_charge", core::AuxType::ScaledDecimal, false},
               {"avg_qty", core::AuxType::Double, false},
               {"avg_price", core::AuxType::Double, false},
               {"avg_disc", core::AuxType::Double, false},
               {"count_order", core::AuxType::Int64, false}}});
  for (const auto &row : rows) {
    sink.WriteRow(
        {{core::AuxValue::Of(
              core::AuxType::String,
              ::accelerator_codegen::UnpackQ1Returnflag(row.group_key)),
          core::AuxValue::Of(
              core::AuxType::String,
              ::accelerator_codegen::UnpackQ1Linestatus(row.group_key)),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{HugeintLower(row.sum_qty), 2}),
          core::AuxValue::Of(
              core::AuxType::ScaledDecimal,
              core::ScaledDecimal{HugeintLower(row.sum_base_price), 2}),
          core::AuxValue::Of(
              core::AuxType::ScaledDecimal,
              core::ScaledDecimal{HugeintLower(row.sum_disc_price), 4}),
          core::AuxValue::Of(
              core::AuxType::ScaledDecimal,
              core::ScaledDecimal{HugeintLower(row.sum_charge), 6}),
          core::AuxValue::Of(core::AuxType::Double, row.avg_qty),
          core::AuxValue::Of(core::AuxType::Double, row.avg_price),
          core::AuxValue::Of(core::AuxType::Double, row.avg_disc),
          core::AuxValue::Of(core::AuxType::Int64, row.count_order)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
