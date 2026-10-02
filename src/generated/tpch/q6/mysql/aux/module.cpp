#include "tpch/q6/mysql/aux/module.hpp"
#include "tpch/q6/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include "duckdb/common/types/hugeint.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "tpch/q6/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

using Q6AccT = ::accelerator_codegen::Q6AccT;
using Q6RevenueMatrix = ::accelerator_codegen::Q6RevenueMatrix;

constexpr uint32_t kVersion = 2;
constexpr uint32_t kMatrixMagic = 0x51364D58; // Q6MX
constexpr int32_t kEpoch = 719468;

void WriteQ6Acc(ByteWriter &writer, Q6AccT value) {
#if defined(__SIZEOF_INT128__)
  writer.U32(static_cast<uint32_t>(value));
  writer.U32(static_cast<uint32_t>(value >> 32));
  writer.U32(static_cast<uint32_t>(value >> 64));
  writer.U32(static_cast<uint32_t>(value >> 96));
#else
  writer.U32(static_cast<uint32_t>(value.lower));
  writer.U32(static_cast<uint32_t>(value.lower >> 32));
  writer.U32(static_cast<uint32_t>(value.upper));
  writer.U32(static_cast<uint32_t>(value.upper >> 32));
#endif
}

bool ReadQ6Acc(ByteReader &reader, Q6AccT &value) {
#if defined(__SIZEOF_INT128__)
  uint32_t w0 = 0;
  uint32_t w1 = 0;
  uint32_t w2 = 0;
  uint32_t w3 = 0;
  if (!reader.U32(w0) || !reader.U32(w1) || !reader.U32(w2) || !reader.U32(w3)) {
    return false;
  }
  value = static_cast<Q6AccT>(w0) | (static_cast<Q6AccT>(w1) << 32) |
          (static_cast<Q6AccT>(w2) << 64) | (static_cast<Q6AccT>(w3) << 96);
  return true;
#else
  uint32_t lo_lo = 0;
  uint32_t lo_hi = 0;
  uint32_t hi_lo = 0;
  uint32_t hi_hi = 0;
  if (!reader.U32(lo_lo) || !reader.U32(lo_hi) || !reader.U32(hi_lo) ||
      !reader.U32(hi_hi)) {
    return false;
  }
  value.lower = static_cast<uint64_t>(lo_lo) | (static_cast<uint64_t>(lo_hi) << 32);
  value.upper = static_cast<int64_t>(static_cast<uint64_t>(hi_lo) |
                                     (static_cast<uint64_t>(hi_hi) << 32));
  return true;
#endif
}

int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

void CivilFromDays(int32_t days, int32_t &year, unsigned &month,
                   unsigned &day) {
  int32_t z = days + kEpoch;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int32_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
}

std::shared_ptr<const TpchQ6StateImage>
MakeStateImage(TpchQ6PartitionKind kind, int32_t key,
               const std::shared_ptr<const Q6RevenueMatrix> &matrix) {
  auto image = std::make_shared<TpchQ6StateImage>();
  image->kind = kind;
  image->key = key;
  image->matrix = matrix;
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

bool DoubleCell(const core::AuxRow &row, std::size_t index, double &out) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<double>(cell.payload)) {
    out = std::get<double>(cell.payload);
    return true;
  }
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = static_cast<double>(std::get<int64_t>(cell.payload));
    return true;
  }
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
    out = static_cast<double>(decimal.value);
    for (int32_t i = 0; i < decimal.scale; ++i)
      out /= 10.0;
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
  double value = 0.0;
  if (!DoubleCell(row, index, value))
    return false;
  raw = static_cast<int64_t>(std::llround(value * 100.0));
  scale = 2;
  return true;
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
  delta.month_key = TpchQ6MonthKeyFromDays(shipdate_days);
  delta.discount_cent = static_cast<int8_t>(discount_raw);
  delta.quantity_scaled = quantity_raw;
  delta.contribution =
      ::accelerator_codegen::Q6RowContributionRaw(extended_raw, discount_raw);
  delta.sign = sign;
  return true;
}

int64_t HugeintLower(const duckdb::hugeint_t &value) {
  return static_cast<int64_t>(value.lower);
}

} // namespace

std::vector<uint8_t> EncodeTpchQ6Matrix(const Q6RevenueMatrix &matrix) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kMatrixMagic, kVersion,
                         static_cast<uint32_t>(matrix.cells.size()));
  for (const auto &cell : matrix.cells) {
    WriteQ6Acc(writer, cell);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ6Matrix(const std::vector<uint8_t> &bytes,
                        Q6RevenueMatrix &matrix) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kMatrixMagic, kVersion, count) ||
      count != matrix.cells.size()) {
    return false;
  }
  for (auto &cell : matrix.cells) {
    if (!ReadQ6Acc(reader, cell)) {
      return false;
    }
  }
  return reader.FullyConsumed();
}

int32_t TpchQ6MonthKeyFromDays(int32_t days_since_epoch) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  return year * 12 + static_cast<int32_t>(month) - 1;
}

int32_t TpchQ6StartOfMonthDays(int32_t days_since_epoch) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  return DaysFromCivil(year, month, 1);
}

int32_t TpchQ6StartOfNextMonthDays(int32_t days_since_epoch) {
  return TpchQ6AddMonths(TpchQ6StartOfMonthDays(days_since_epoch), 1);
}

int32_t TpchQ6AddMonths(int32_t days_since_epoch, int32_t months) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  const int total = year * 12 + static_cast<int>(month) - 1 + months;
  const int new_year = total / 12;
  const unsigned new_month = static_cast<unsigned>(total % 12 + 1);
  return DaysFromCivil(new_year, new_month, day);
}

int32_t TpchQ6ExclusiveEndDays(int32_t start_days) {
  return TpchQ6AddMonths(start_days, 12);
}

bool ParseTpchQ6QueryParams(const core::QueryArguments &arguments,
                            TpchQ6QueryParams &params) {
  if (arguments.values.size() < 3)
    return false;
  if (arguments.values[0].is_null || arguments.values[1].is_null ||
      arguments.values[2].is_null)
    return false;

  if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
    params.start_days =
        std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
  else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
    params.start_days =
        static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  else
    return false;

  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  if (std::holds_alternative<core::ScaledDecimal>(
          arguments.values[1].payload)) {
    const auto decimal =
        std::get<core::ScaledDecimal>(arguments.values[1].payload);
    discount_raw = decimal.value;
    discount_scale = decimal.scale;
  } else if (std::holds_alternative<double>(arguments.values[1].payload)) {
    const auto value = std::get<double>(arguments.values[1].payload);
    discount_raw = static_cast<int64_t>(std::llround(value * 100.0));
    discount_scale = 2;
  } else if (std::holds_alternative<int64_t>(arguments.values[1].payload)) {
    discount_raw = std::get<int64_t>(arguments.values[1].payload);
    discount_scale = 2;
  } else {
    return false;
  }
  discount_raw = ScaleToRaw(discount_raw, discount_scale, 2);
  params.discount_lower_raw = discount_raw - 1;
  params.discount_upper_raw = discount_raw + 1;

  int64_t quantity_raw = 0;
  int32_t quantity_scale = 2;
  if (std::holds_alternative<core::ScaledDecimal>(
          arguments.values[2].payload)) {
    const auto decimal =
        std::get<core::ScaledDecimal>(arguments.values[2].payload);
    quantity_raw = decimal.value;
    quantity_scale = decimal.scale;
  } else if (std::holds_alternative<double>(arguments.values[2].payload)) {
    const auto value = std::get<double>(arguments.values[2].payload);
    quantity_raw = static_cast<int64_t>(std::llround(value * 100.0));
    quantity_scale = 2;
  } else if (std::holds_alternative<int64_t>(arguments.values[2].payload)) {
    quantity_raw = std::get<int64_t>(arguments.values[2].payload);
    quantity_scale = 2;
  } else {
    return false;
  }
  params.quantity_upper_raw = ScaleToRaw(quantity_raw, quantity_scale, 2);
  params.end_days = TpchQ6ExclusiveEndDays(params.start_days);
  return true;
}

core::ScaledDecimal TpchQ6RevenueOutputValue(Q6AccT raw) {
  const auto hugeint = ::accelerator_codegen::Q6RevenueFromAccumulator(raw);
  return core::ScaledDecimal{HugeintLower(hugeint),
                             ::accelerator_codegen::kQ6RevenueDecimalScale};
}

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

void TpchQ6Module::ApplyLineitemInput(
    const TpchQ6LineitemInput &input,
    std::unordered_map<core::PartitionId, Q6RevenueMatrix> &matrices) const {
  const auto discount_raw =
      ScaleToRaw(input.discount_raw, input.discount_scale, 2);
  const auto quantity_raw =
      ScaleToRaw(input.quantity_raw, input.quantity_scale, 2);
  const auto extended_raw =
      ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2);
  const auto contribution =
      ::accelerator_codegen::Q6RowContributionRaw(extended_raw, discount_raw);
  const auto discount_cent = static_cast<int8_t>(discount_raw);
  const auto month_partition = core::TpchQ6Layout::MonthPartition(
      TpchQ6MonthKeyFromDays(input.shipdate_days));
  matrices[month_partition].AddCell(discount_cent, quantity_raw, contribution, 1);
}

void TpchQ6Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  (void)services;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q6 requires a typed initial scan source");
  std::unordered_map<core::PartitionId, Q6RevenueMatrix> matrices;
  initial_scan->ScanLineitems(
      [&](const TpchQ6LineitemInput &input) { ApplyLineitemInput(input, matrices); });
  for (const auto &entry : matrices) {
    const auto matrix = std::make_shared<const Q6RevenueMatrix>(entry.second);
    const auto kind = core::TpchQ6Layout::IsMonthPartition(entry.first)
                          ? TpchQ6PartitionKind::Month
                          : TpchQ6PartitionKind::Day;
    const auto key =
        kind == TpchQ6PartitionKind::Month
            ? core::TpchQ6Layout::MonthKeyFromPartition(entry.first)
            : core::TpchQ6Layout::DaysFromPartition(entry.first);
    builder.Write(core::StateImage::Make(
        core::TpchQ6Layout::kAccelerator, entry.first, 0,
        MakeStateImage(kind, key, matrix)));
  }
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
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      TpchQ6LineitemDelta old_delta, new_delta;
      if (!ReadLineitem(mutation.old_rows[i], old_delta, -1) ||
          !ReadLineitem(mutation.new_rows[i], new_delta, 1)) {
        buffer.MarkRebuild();
        return;
      }
      if (old_delta.shipdate_days != new_delta.shipdate_days ||
          old_delta.month_key != new_delta.month_key) {
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
TpchQ6Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ6TxnBuffer &>(base_buffer);
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

  std::map<core::PartitionId, std::vector<TpchQ6LineitemDelta>> grouped;
  for (const auto &delta : buffer.Deltas()) {
    grouped[core::TpchQ6Layout::MonthPartition(delta.month_key)].push_back(delta);
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()),
       static_cast<uint64_t>(grouped.size()), true, true, true, false, false,
       core::MaintenanceAction::NoOp, services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    std::set<int32_t> month_keys;
    for (const auto &entry : grouped) {
      if (core::TpchQ6Layout::IsMonthPartition(entry.first))
        month_keys.insert(
            core::TpchQ6Layout::MonthKeyFromPartition(entry.first));
    }
    std::unordered_map<core::PartitionId, Q6RevenueMatrix> rebuilt;
    if (initial_scan) {
      initial_scan->ScanLineitemsForMonths(
          month_keys, [&](const TpchQ6LineitemInput &input) {
            ApplyLineitemInput(input, rebuilt);
          });
    }
    for (const auto &entry : grouped) {
      Q6RevenueMatrix matrix;
      const auto it = rebuilt.find(entry.first);
      if (it != rebuilt.end())
        matrix = it->second;
      const auto kind = core::TpchQ6Layout::IsMonthPartition(entry.first)
                            ? TpchQ6PartitionKind::Month
                            : TpchQ6PartitionKind::Day;
      const auto key =
          kind == TpchQ6PartitionKind::Month
              ? core::TpchQ6Layout::MonthKeyFromPartition(entry.first)
              : core::TpchQ6Layout::DaysFromPartition(entry.first);
      const auto typed = std::make_shared<const Q6RevenueMatrix>(matrix);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ6Layout::kAccelerator, entry.first, 0,
          MakeStateImage(kind, key, typed)));
    }
    core::FinishRebuildPartition(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  std::vector<core::PartitionId> partitions;
  partitions.reserve(grouped.size());
  for (const auto &entry : grouped)
    partitions.push_back(entry.first);

  std::map<core::PartitionId, core::Version> versions;
  std::unordered_map<core::PartitionId, Q6RevenueMatrix> state;
  const auto visibility = services.CurrentVisibility();
  const auto snapshot =
      services.StateStore().ReadPartitions(visibility, partitions);
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ6Image(image);
    if (!typed || !typed->matrix) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    const auto expected_kind =
        core::TpchQ6Layout::IsMonthPartition(image.partition)
            ? TpchQ6PartitionKind::Month
            : TpchQ6PartitionKind::Day;
    const auto expected_key =
        expected_kind == TpchQ6PartitionKind::Month
            ? core::TpchQ6Layout::MonthKeyFromPartition(image.partition)
            : core::TpchQ6Layout::DaysFromPartition(image.partition);
    if (typed->kind != expected_kind || typed->key != expected_key) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    state[image.partition] = *typed->matrix;
    versions[image.partition] = image.version;
  }
  if (versions.size() != grouped.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto &entry : grouped) {
    auto &matrix = state.at(entry.first);
    for (const auto &delta : entry.second) {
      matrix.AddCell(delta.discount_cent, delta.quantity_scaled,
                     delta.contribution, delta.sign);
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  for (const auto &entry : grouped) {
    const auto partition = entry.first;
    const auto version = versions.at(partition);
    const auto expected_kind =
        core::TpchQ6Layout::IsMonthPartition(partition)
            ? TpchQ6PartitionKind::Month
            : TpchQ6PartitionKind::Day;
    const auto expected_key =
        expected_kind == TpchQ6PartitionKind::Month
            ? core::TpchQ6Layout::MonthKeyFromPartition(partition)
            : core::TpchQ6Layout::DaysFromPartition(partition);
    const auto matrix =
        std::make_shared<const Q6RevenueMatrix>(state.at(partition));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ6Layout::kAccelerator, partition, version,
        MakeStateImage(expected_kind, expected_key, matrix)));
  }
  return outcome;
}

bool TpchQ6Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ6Module::CollectQueryPartitions(
    const TpchQ6QueryParams &params,
    std::vector<core::PartitionId> &partitions) const {
  std::unordered_set<core::PartitionId> unique;
  auto add_day = [&](int32_t day) {
    unique.insert(core::TpchQ6Layout::DayPartition(day));
  };
  auto add_month = [&](int32_t day) {
    unique.insert(core::TpchQ6Layout::MonthPartition(
        TpchQ6MonthKeyFromDays(day)));
  };

  int32_t cursor = params.start_days;
  const int32_t end = params.end_days;
  while (cursor < end) {
    if (cursor == TpchQ6StartOfMonthDays(cursor)) {
      const int32_t month_end = TpchQ6StartOfNextMonthDays(cursor);
      if (month_end <= end) {
        add_month(cursor);
        cursor = month_end;
        continue;
      }
    }
    add_day(cursor);
    ++cursor;
  }

  partitions.assign(unique.begin(), unique.end());
  std::sort(partitions.begin(), partitions.end());
}

bool TpchQ6Module::LoadMatrices(core::AuxEngineServices &services,
                                const TpchQ6QueryParams &params,
                                MatrixLookup &matrices) const {
  std::vector<core::PartitionId> partitions;
  CollectQueryPartitions(params, partitions);
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ6Image(image);
    if (!typed || !typed->matrix)
      return false;
    matrices.emplace(image.partition, typed->matrix);
  }
  return !matrices.empty();
}

Q6AccT TpchQ6Module::ComposeRevenue(const MatrixLookup &matrices,
                                    const TpchQ6QueryParams &params) const {
  Q6AccT revenue = static_cast<Q6AccT>(0);
  auto add_matrix = [&](core::PartitionId partition) {
    const auto it = matrices.find(partition);
    if (it == matrices.end() || !it->second)
      return;
    revenue += it->second->SumRange(params.discount_lower_raw,
                                    params.discount_upper_raw,
                                    params.quantity_upper_raw);
  };

  int32_t cursor = params.start_days;
  const int32_t end = params.end_days;
  while (cursor < end) {
    if (cursor == TpchQ6StartOfMonthDays(cursor)) {
      const int32_t month_end = TpchQ6StartOfNextMonthDays(cursor);
      if (month_end <= end) {
        add_matrix(core::TpchQ6Layout::MonthPartition(
            TpchQ6MonthKeyFromDays(cursor)));
        cursor = month_end;
        continue;
      }
    }
    add_matrix(core::TpchQ6Layout::DayPartition(cursor));
    ++cursor;
  }
  return revenue;
}

void TpchQ6Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  TpchQ6QueryParams params;
  if (!ParseTpchQ6QueryParams(arguments, params))
    return;
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  MatrixLookup matrices;
  if (!LoadMatrices(services, params, matrices)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(ComposeRevenue(matrices, params), sink);
}

void TpchQ6Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  TpchQ6QueryParams params;
  if (!ParseTpchQ6QueryParams(arguments, params))
    return;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q6 requires a typed initial scan source");

  Q6AccT revenue = static_cast<Q6AccT>(0);
  initial_scan->ScanLineitems([&](const TpchQ6LineitemInput &input) {
    if (input.shipdate_days < params.start_days ||
        input.shipdate_days >= params.end_days)
      return;
    const auto discount_raw =
        ScaleToRaw(input.discount_raw, input.discount_scale, 2);
    const auto quantity_raw =
        ScaleToRaw(input.quantity_raw, input.quantity_scale, 2);
    if (discount_raw < params.discount_lower_raw ||
        discount_raw > params.discount_upper_raw ||
        quantity_raw >= params.quantity_upper_raw)
      return;
    const auto extended_raw =
        ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2);
    revenue += ::accelerator_codegen::Q6RowContributionRaw(extended_raw,
                                                           discount_raw);
  });
  (void)services;
  EmitResult(revenue, sink);
}

void TpchQ6Module::EmitResult(Q6AccT revenue, core::AuxRowSink &sink) const {
  sink.Begin({{{"revenue", core::AuxType::ScaledDecimal, false}}});
  sink.WriteRow(
      {{core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           TpchQ6RevenueOutputValue(revenue))}});
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ6Module::CalibrationWrites() const {
  return TpchQ6CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
