#include "tpch/q14/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kPartMagic = 0x50313451;  // Q14P
constexpr uint32_t kMonthMagic = 0x4d313451; // Q14M
constexpr int32_t kEpoch = 719468;

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

int32_t MonthKey(int32_t days) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days, year, month, day);
  return year * 100 + static_cast<int32_t>(month);
}

std::shared_ptr<const TpchQ14StateImage>
MakeStateImage(TpchQ14ImageKind kind, int32_t month,
               const std::shared_ptr<const TpchQ14State> &state) {
  auto image = std::make_shared<TpchQ14StateImage>();
  image->kind = kind;
  image->month = month;
  image->state = state;
  return image;
}

const TpchQ14StateImage *AsQ14Image(const core::StateImage &image,
                                    TpchQ14ImageKind kind) {
  const auto *payload = image.As<TpchQ14StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ14LineitemInput &out) {
  int64_t partkey = 0;
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  if (!IntCell(row, 0, partkey) ||
      !RawDecimalCell(row, 1, extended_raw, extended_scale) ||
      !RawDecimalCell(row, 2, discount_raw, discount_scale) ||
      !DateCell(row, 3, out.shipdate_days))
    return false;
  out.partkey = partkey;
  out.extended_price_raw = extended_raw;
  out.extended_price_scale = extended_scale;
  out.discount_raw = discount_raw;
  out.discount_scale = discount_scale;
  return true;
}

} // namespace

int32_t TpchQ14DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return DaysFromCivil(year, month, day);
}

int32_t TpchQ14AddMonths(int32_t days_since_epoch, int32_t months) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  const int total = year * 12 + static_cast<int>(month) - 1 + months;
  const int new_year = total / 12;
  const unsigned new_month = static_cast<unsigned>(total % 12 + 1);
  return DaysFromCivil(new_year, new_month, day);
}

int32_t TpchQ14MonthKeyFromDays(int32_t days_since_epoch) {
  return MonthKey(days_since_epoch);
}

bool TpchQ14IsPromoType(const std::string &type) {
  static constexpr const char kPromo[] = "PROMO";
  return type.size() >= sizeof(kPromo) - 1 &&
         type.compare(0, sizeof(kPromo) - 1, kPromo) == 0;
}

int64_t TpchQ14RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                          int64_t discount_raw, int32_t discount_scale) {
  if (extended_scale == 2 && discount_scale == 2)
    return extended_price_raw * (100 - discount_raw);
  double price = static_cast<double>(extended_price_raw);
  double discount = static_cast<double>(discount_raw);
  for (int32_t i = 0; i < extended_scale; ++i)
    price /= 10.0;
  for (int32_t i = 0; i < discount_scale; ++i)
    discount /= 10.0;
  return static_cast<int64_t>(std::llround(price * (1.0 - discount) * 10000.0));
}

double TpchQ14FinalizePromoRevenue(const TpchQ14MonthTotals &totals) {
  if (!totals.saw_contribution)
    return std::numeric_limits<double>::quiet_NaN();
  if (totals.total_revenue_raw == 0) {
    if (totals.promo_revenue_raw == 0)
      return std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0);
    return std::numeric_limits<double>::infinity();
  }
  const double numerator =
      100.0 * static_cast<double>(totals.promo_revenue_raw) / 10000.0;
  const double denominator =
      static_cast<double>(totals.total_revenue_raw) / 10000.0;
  return numerator / denominator;
}

std::vector<uint8_t> EncodeTpchQ14PartPromo(const TpchQ14State &state) {
  ByteWriter writer;
  std::vector<int64_t> keys;
  for (const auto &entry : state.part_promo)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  core::WriteImageHeader(writer, kPartMagic, kVersion, static_cast<uint32_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.U8(static_cast<uint8_t>(state.part_promo.at(key)));
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ14Month(int32_t month,
                                        const TpchQ14State &state) {
  ByteWriter writer;
  const auto it = state.month_totals.find(month);
  TpchQ14MonthTotals totals;
  if (it != state.month_totals.end())
    totals = it->second;
  core::WriteImageHeader(writer, kMonthMagic, kVersion, 1);
  writer.I64(totals.total_revenue_raw);
  writer.I64(totals.promo_revenue_raw);
  writer.I64(totals.row_count);
  writer.U8(totals.saw_contribution ? 1 : 0);
  return std::move(writer.bytes);
}

bool DecodeTpchQ14PartPromo(const std::vector<uint8_t> &bytes,
                            TpchQ14State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kPartMagic, kVersion, count))
    return false;
  state.part_promo.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint8_t promo = 0;
    if (!reader.I64(key) || !reader.U8(promo))
      return false;
    state.part_promo.emplace(key, static_cast<TpchQ14PromoClass>(promo));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ14Month(const std::vector<uint8_t> &bytes, int32_t month,
                          TpchQ14State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kMonthMagic, kVersion, count) || count != 1)
    return false;
  TpchQ14MonthTotals totals;
  uint8_t saw = 0;
  if (!reader.I64(totals.total_revenue_raw) ||
      !reader.I64(totals.promo_revenue_raw) || !reader.I64(totals.row_count) ||
      !reader.U8(saw))
    return false;
  totals.saw_contribution = saw != 0;
  state.month_totals[month] = totals;
  return reader.offset == bytes.size();
}

bool FindTpchQ14PartPromo(const std::vector<uint8_t> &bytes, int64_t partkey,
                            TpchQ14PromoClass &promo) {
  ByteReader header{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(header, kPartMagic, kVersion, count))
    return false;
  constexpr std::size_t kHeaderBytes = 12;
  constexpr std::size_t kRecordBytes = 9;
  if (count > (std::numeric_limits<std::size_t>::max() - kHeaderBytes) /
                 kRecordBytes ||
      bytes.size() != kHeaderBytes +
                          static_cast<std::size_t>(count) * kRecordBytes)
    return false;
  std::size_t first = 0;
  std::size_t last = count;
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    ByteReader reader{bytes};
    reader.offset = kHeaderBytes + middle * kRecordBytes;
    int64_t key = 0;
    if (!reader.I64(key))
      return false;
    if (key < partkey) {
      first = middle + 1;
      continue;
    }
    if (key > partkey) {
      last = middle;
      continue;
    }
    uint8_t value = 0;
    if (!reader.U8(value))
      return false;
    promo = static_cast<TpchQ14PromoClass>(value);
    return true;
  }
  return false;
}

bool TpchQ14TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ14TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ14TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ14LineitemDelta> &TpchQ14TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ14TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ14TxnBuffer::AddDelta(TpchQ14LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ14Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ14Module::Dependencies() const {
  return {{config.part_table, {0, 1}},
          {config.lineitem_table, {0, 1, 2, 3}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ14Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ14TxnBuffer>();
}

void TpchQ14Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ14State>(state);
  builder.Write(core::StateImage::Make(
      core::TpchQ14Layout::kAccelerator, core::TpchQ14Layout::kPartPromo, 0,
      MakeStateImage(TpchQ14ImageKind::PartPromo, 0, typed_state)));
  for (const auto &entry : state.month_totals) {
    builder.Write(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator,
        static_cast<core::PartitionId>(entry.first), 0,
        MakeStateImage(TpchQ14ImageKind::Month, entry.first, typed_state)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ14Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.part_table)
    return All();
  return {};
}

void TpchQ14Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ14TxnBuffer &>(base_buffer);
  if (mutation.table != config.lineitem_table)
    return;
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ14LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    TpchQ14LineitemDelta delta;
    delta.month = TpchQ14MonthKeyFromDays(input.shipdate_days);
    delta.partkey = input.partkey;
    delta.revenue_raw =
        TpchQ14RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                          input.discount_raw, input.discount_scale);
    delta.sign = sign;
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
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ14Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ14TxnBuffer &>(base_buffer);
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

  std::set<int32_t> touched_months;
  for (const auto &delta : buffer.Deltas())
    touched_months.insert(delta.month);

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()),
       static_cast<uint64_t>(touched_months.size()), true, true, true, false,
       false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  const auto part_snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ14Layout::kPartPromo});
  const TpchQ14State *part_state = nullptr;
  for (const auto &image : part_snapshot.partitions) {
    if (image.partition == core::TpchQ14Layout::kPartPromo) {
      const auto *typed = AsQ14Image(image, TpchQ14ImageKind::PartPromo);
      part_state = typed ? typed->state.get() : nullptr;
      break;
    }
  }
  if (!part_state) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  std::vector<core::PartitionId> month_partitions;
  month_partitions.reserve(touched_months.size());
  for (const auto month : touched_months)
    month_partitions.push_back(static_cast<core::PartitionId>(month));

  TpchQ14State state;
  std::map<int32_t, core::Version> versions;
  const auto month_snapshot =
      services.StateStore().ReadPartitions(visibility, month_partitions);
  for (const auto &image : month_snapshot.partitions) {
    const auto month = static_cast<int32_t>(image.partition);
    const auto *typed = AsQ14Image(image, TpchQ14ImageKind::Month);
    if (!typed || typed->month != month) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    const auto month_it = typed->state->month_totals.find(month);
    if (month_it != typed->state->month_totals.end())
      state.month_totals[month] = month_it->second;
    versions[month] = image.version;
  }
  if (versions.size() != touched_months.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto &delta : buffer.Deltas()) {
    const auto part_it = part_state->part_promo.find(delta.partkey);
    if (part_it == part_state->part_promo.end()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    if (part_it->second == TpchQ14PromoClass::Absent) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    auto &totals = state.month_totals[delta.month];
    const auto signed_revenue =
        static_cast<int64_t>(delta.sign) * delta.revenue_raw;
    totals.total_revenue_raw += signed_revenue;
    totals.row_count += delta.sign;
    totals.saw_contribution = totals.row_count > 0;
    if (part_it->second == TpchQ14PromoClass::Promo)
      totals.promo_revenue_raw += signed_revenue;
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  for (const auto month : touched_months) {
    const auto partition = static_cast<core::PartitionId>(month);
    const auto version = versions.count(month) ? versions[month] : 0;
    TpchQ14State month_state;
    const auto month_it = state.month_totals.find(month);
    if (month_it != state.month_totals.end())
      month_state.month_totals.emplace(month, month_it->second);
    const auto typed_state = std::make_shared<const TpchQ14State>(month_state);
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator, partition, version,
        MakeStateImage(TpchQ14ImageKind::Month, month, typed_state)));
  }
  return outcome;
}

bool TpchQ14Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ14Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start =
          std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  }
  const auto month = TpchQ14MonthKeyFromDays(start);
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  TpchQ14MonthTotals totals;
  if (!LoadMonth(services, month, totals)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(totals, sink);
}

void TpchQ14Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
    start =
        std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
  const auto state = BuildState(services);
  const auto month = TpchQ14MonthKeyFromDays(start);
  const auto it = state.month_totals.find(month);
  EmitResult(it != state.month_totals.end() ? it->second : TpchQ14MonthTotals{},
               sink);
}

TpchQ14State TpchQ14Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ14State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q14 requires a typed initial scan source");
  initial_scan->ScanParts([&](const TpchQ14PartInput &input) {
    state.part_promo[input.key] =
        TpchQ14IsPromoType(input.type) ? TpchQ14PromoClass::Promo
                                       : TpchQ14PromoClass::NonPromo;
  });
  initial_scan->ScanLineitems([&](const TpchQ14LineitemInput &input) {
    const auto part_it = state.part_promo.find(input.partkey);
    if (part_it == state.part_promo.end())
      return;
    const auto revenue =
        TpchQ14RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                          input.discount_raw, input.discount_scale);
    const auto month = TpchQ14MonthKeyFromDays(input.shipdate_days);
    auto &totals = state.month_totals[month];
    totals.total_revenue_raw += revenue;
    totals.row_count++;
    totals.saw_contribution = true;
    if (part_it->second == TpchQ14PromoClass::Promo)
      totals.promo_revenue_raw += revenue;
  });
  return state;
}

bool TpchQ14Module::LoadMonth(core::AuxEngineServices &services, int32_t month,
                              TpchQ14MonthTotals &totals) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {static_cast<core::PartitionId>(month)});
  for (const auto &image : snapshot.partitions) {
    if (static_cast<int32_t>(image.partition) != month)
      continue;
    const auto *typed = AsQ14Image(image, TpchQ14ImageKind::Month);
    if (!typed || typed->month != month)
      return false;
    const auto it = typed->state->month_totals.find(month);
    if (it != typed->state->month_totals.end())
      totals = it->second;
    return true;
  }
  return false;
}

void TpchQ14Module::EmitResult(const TpchQ14MonthTotals &totals,
                               core::AuxRowSink &sink) const {
  const auto value = TpchQ14FinalizePromoRevenue(totals);
  sink.Begin({{{"promo_revenue", core::AuxType::Double, true}}});
  if (std::isnan(value))
    sink.WriteRow({{core::AuxValue::Null(core::AuxType::Double)}});
  else
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Double, value)}});
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
