#include "tpch/q14/duckdb/aux/module.hpp"
#include "tpch/q14/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/typed_row_batch.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include "tpch/q14/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kPartMagic = 0x50313451;  // Q14P
constexpr uint32_t kMonthMagic = 0x4d313451; // Q14M
constexpr int32_t kEpoch = 719468;
// Per maintenance slice: classifying and summing one lineitem delta is ~30 ns.
constexpr std::size_t kDeltaGrain = 16384;

struct PartRow {
  int64_t key;
  bool promo;
};

void AddTotals(TpchQ14MonthTotals &into, const TpchQ14MonthTotals &from) {
  into.total_revenue_raw += from.total_revenue_raw;
  into.promo_revenue_raw += from.promo_revenue_raw;
  into.row_count += from.row_count;
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
  if (!payload || payload->kind != kind)
    return nullptr;
  return kind == TpchQ14ImageKind::PartPromo && !payload->state ? nullptr
                                                                : payload;
}

std::shared_ptr<const TpchQ14StateImage>
MakeMonthImage(int32_t month, const TpchQ14MonthTotals &totals) {
  auto image = std::make_shared<TpchQ14StateImage>();
  image->kind = TpchQ14ImageKind::Month;
  image->month = month;
  image->totals = totals;
  return image;
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

bool TpchQ14IsPromoType(std::string_view type) {
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

std::vector<uint8_t> EncodeTpchQ14PartPromo(const TpchQ14State &state) {
  ByteWriter writer;
  std::vector<int64_t> keys;
  for (std::size_t p = 0; p < state.classes.NumChunks(); ++p) {
    const auto *page = state.classes.ChunkAt(p);
    if (!page)
      continue;
    for (std::size_t i = 0; i < page->size(); ++i) {
      if (((*page)[i] & TpchQ14State::kClassMask) != 0)
        keys.push_back(state.classes.ChunkBase(p) + static_cast<int64_t>(i));
    }
  }
  const auto dense = keys.size();
  for (const auto &[key, raw] : state.promo_overflow) {
    if ((raw & TpchQ14State::kClassMask) != 0)
      keys.push_back(key);
  }
  std::sort(keys.begin() + static_cast<std::ptrdiff_t>(dense), keys.end());
  core::WriteImageHeader(writer, kPartMagic, kVersion, static_cast<uint64_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.U8(static_cast<uint8_t>(state.PromoOf(key)));
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
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kPartMagic, kVersion, count))
    return false;
  state.classes.Clear();
  state.promo_overflow.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint8_t promo = 0;
    if (!reader.I64(key) || !reader.U8(promo))
      return false;
    state.SetPromo(key, static_cast<TpchQ14PromoClass>(promo));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ14Month(const std::vector<uint8_t> &bytes, int32_t month,
                          TpchQ14State &state) {
  ByteReader reader{bytes};
  uint64_t count = 0;
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
  uint64_t count = 0;
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
  part_events.clear();
}
bool TpchQ14TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ14LineitemDelta> &TpchQ14TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ14TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  part_events.clear();
}
const std::vector<TpchQ14PartEvent> &TpchQ14TxnBuffer::PartEvents() const {
  return part_events;
}
void TpchQ14TxnBuffer::AddPartEvent(TpchQ14PartEvent event) {
  pending = true;
  part_events.push_back(event);
}
void TpchQ14TxnBuffer::AddDelta(TpchQ14LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ14Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ14Module::Dependencies() const {
  return {{config.part_table, {0, 1}, {0, 4}},
          {config.lineitem_table, {0, 1, 2, 3}, {1, 5, 6, 10}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ14Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ14TxnBuffer>();
}

void TpchQ14Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = BuildState(services);
  for (const auto &entry : state.month_totals) {
    builder.Write(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator,
        static_cast<core::PartitionId>(entry.first), 0,
        MakeMonthImage(entry.first, entry.second)));
  }
  state.month_totals.clear(); // the part image carries classes only
  const auto typed_state =
      std::make_shared<const TpchQ14State>(std::move(state));
  builder.Write(core::StateImage::Make(
      core::TpchQ14Layout::kAccelerator, core::TpchQ14Layout::kPartPromo, 0,
      MakeStateImage(TpchQ14ImageKind::PartPromo, 0, typed_state)));
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
  if (mutation.table == config.part_table) {
    // Point part writes are maintained locally (PartPromo + month images);
    // StageMaintenance names what the commit touches.
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ14Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ14TxnBuffer &>(base_buffer);
  if (mutation.table == config.part_table) {
    // A part's type decides whether its revenue counts as promo. Point
    // inserts/deletes/retypes are applied locally in StageMaintenance.
    auto read = [&](const core::AuxRow &row, TpchQ14PartEvent &event) {
      std::string type;
      if (!IntCell(row, 0, event.key) || !StringCell(row, 1, type))
        return false;
      event.new_class = TpchQ14IsPromoType(type) ? TpchQ14PromoClass::Promo
                                                 : TpchQ14PromoClass::NonPromo;
      return true;
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        TpchQ14PartEvent event;
        event.kind = core::MutationKind::Insert;
        if (!read(row, event))
          return buffer.MarkRebuild();
        buffer.AddPartEvent(event);
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        TpchQ14PartEvent event;
        event.kind = core::MutationKind::Delete;
        if (!read(row, event))
          return buffer.MarkRebuild();
        buffer.AddPartEvent(event);
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size())
        return buffer.MarkRebuild();
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        TpchQ14PartEvent old_event, new_event;
        if (!read(mutation.old_rows[i], old_event) ||
            !read(mutation.new_rows[i], new_event) ||
            old_event.key != new_event.key)
          return buffer.MarkRebuild();
        new_event.kind = core::MutationKind::Update;
        buffer.AddPartEvent(new_event);
      }
      break;
    }
    return;
  }
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
  auto rebuild_all = [&]() {
    core::StagedMaintenanceOutcome out;
    out.action = core::MaintenanceAction::RebuildAll;
    out.invalidations = All();
    out.pending_rebuild = out.invalidations;
    return out;
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
  const auto &events = buffer.PartEvents();
  if (buffer.Deltas().empty() && events.empty())
    return outcome;

  // Distinct months of the lineitem deltas (sorted).
  std::vector<int32_t> touched_vec;
  touched_vec.reserve(buffer.Deltas().size());
  for (const auto &delta : buffer.Deltas())
    touched_vec.push_back(delta.month);
  std::sort(touched_vec.begin(), touched_vec.end());
  touched_vec.erase(std::unique(touched_vec.begin(), touched_vec.end()),
                    touched_vec.end());

  // Small batches (and every batch carrying part writes) are applied as
  // deltas; a policy-chosen partition rebuild is kept for big batches only.
  constexpr std::size_t kLocalDeltaLimit = 50000;
  auto action = core::MaintenanceAction::ApplyDelta;
  if (!buffer.Deltas().empty()) {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(buffer.Deltas().size()),
         static_cast<uint64_t>(touched_vec.size()), true, true, true, false,
         false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    action = decision.action;
    if (!core::ForcedMaintenanceAction() &&
        (buffer.Deltas().size() < kLocalDeltaLimit || !events.empty()))
      action = core::MaintenanceAction::ApplyDelta;
  }

  const auto visibility = services.CurrentVisibility();
  // One read for the part classes and the months the deltas touch.
  std::vector<core::PartitionId> wanted;
  wanted.reserve(touched_vec.size() + 1);
  wanted.push_back(core::TpchQ14Layout::kPartPromo);
  for (const auto month : touched_vec)
    wanted.push_back(static_cast<core::PartitionId>(month));
  const auto part_snapshot =
      services.StateStore().ReadPartitions(visibility, wanted);
  const TpchQ14State *part_state = nullptr;
  core::Version part_version = 0;
  for (const auto &image : part_snapshot.partitions) {
    if (image.partition == core::TpchQ14Layout::kPartPromo) {
      const auto *typed = AsQ14Image(image, TpchQ14ImageKind::PartPromo);
      part_state = typed ? typed->state.get() : nullptr;
      part_version = image.version;
      break;
    }
  }

  if (action == core::MaintenanceAction::RebuildPartition) {
    if (!part_state || !initial_scan)
      return rebuild_all();
    const std::set<int32_t> touched_months(touched_vec.begin(),
                                           touched_vec.end());
    TpchQ14State rebuilt;
    const bool aggregated = initial_scan->ScanMonthAggregates(
        touched_months,
        [&](int32_t month, int64_t total, int64_t promo, int64_t count) {
          auto &totals = rebuilt.month_totals[month];
          totals.total_revenue_raw += total;
          totals.promo_revenue_raw += promo;
          totals.row_count += count;
          totals.saw_contribution = totals.saw_contribution || count > 0;
        });
    if (!aggregated) {
      initial_scan->ScanLineitemsForMonths(
          touched_months, [&](const TpchQ14LineitemInput &input) {
            const auto promo = part_state->PromoOf(input.partkey);
            if (promo == TpchQ14PromoClass::Absent)
              return;
            const auto revenue = TpchQ14RevenueRaw(
                input.extended_price_raw, input.extended_price_scale,
                input.discount_raw, input.discount_scale);
            const auto month = TpchQ14MonthKeyFromDays(input.shipdate_days);
            auto &totals = rebuilt.month_totals[month];
            totals.total_revenue_raw += revenue;
            totals.row_count++;
            totals.saw_contribution = true;
            if (promo == TpchQ14PromoClass::Promo)
              totals.promo_revenue_raw += revenue;
          });
    }
    for (const auto month : touched_months) {
      const auto it = rebuilt.month_totals.find(month);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ14Layout::kAccelerator,
          static_cast<core::PartitionId>(month), 0,
          MakeMonthImage(month, it != rebuilt.month_totals.end()
                                    ? it->second
                                    : TpchQ14MonthTotals{})));
    }
    core::FinishRebuildPartition(outcome);
    return outcome;
  }
  if (action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  if (!part_state)
    return rebuild_all();

  // The part classes are only copied (chunk directory; chunks stay shared until
  // SetRaw touches one) when a write actually changes a class: lineitem-only
  // batches read the published state and copy nothing.
  struct WorkState {
    const TpchQ14State *published;
    TpchQ14State copy;
    bool copied = false;
    uint8_t RawOf(int64_t key) const {
      return copied ? copy.RawOf(key) : published->RawOf(key);
    }
    TpchQ14PromoClass PromoOf(int64_t key) const {
      return static_cast<TpchQ14PromoClass>(RawOf(key) &
                                            TpchQ14State::kClassMask);
    }
    void SetRaw(int64_t key, uint8_t raw) {
      if (!copied) {
        copy.build_max_partkey = published->build_max_partkey;
        copy.classes = published->classes;
        copy.promo_overflow = published->promo_overflow;
        copied = true;
      }
      copy.SetRaw(key, raw);
    }
    int64_t BuildMax() const {
      return copied ? copy.build_max_partkey : published->build_max_partkey;
    }
  } work{part_state, {}, false};
  bool part_changed = false;
  // Month-total adjustments (signed deltas; saw_contribution unused), flat and
  // sorted by month.
  struct Adjust {
    int32_t month;
    TpchQ14MonthTotals totals;
  };
  std::vector<Adjust> adj;
  auto adjust_of = [](std::vector<Adjust> &sorted,
                      int32_t month) -> TpchQ14MonthTotals & {
    auto it = std::lower_bound(
        sorted.begin(), sorted.end(), month,
        [](const Adjust &a, int32_t m) { return a.month < m; });
    if (it == sorted.end() || it->month != month)
      it = sorted.insert(it, Adjust{month, {}});
    return it->totals;
  };

  // Parts whose lineitems may exist (not fresh): old/new class, resolved with
  // one lineitem aggregate scan.
  struct Transition {
    TpchQ14PromoClass from;
    TpchQ14PromoClass to;
  };
  std::map<int64_t, Transition> transitions;
  constexpr uint8_t kFresh = TpchQ14State::kFreshBit;
  constexpr uint8_t kGone = TpchQ14State::kGoneBit;
  constexpr uint8_t kDead = TpchQ14State::kDeadBit;
  for (const auto &event : events) {
    const auto cls = static_cast<uint8_t>(event.new_class);
    const auto raw = work.RawOf(event.key);
    const auto current = work.PromoOf(event.key);
    const auto tr = transitions.find(event.key);
    switch (event.kind) {
    case core::MutationKind::Insert:
      if (current != TpchQ14PromoClass::Absent) {
        // Another row of an already inserted key: exact only while the key
        // has no lineitems (fresh); the class stays last-wins like Build.
        if (!(raw & kFresh) || tr != transitions.end())
          return rebuild_all();
        work.SetRaw(event.key, static_cast<uint8_t>(cls | kFresh));
        part_changed = true;
        break;
      }
      if (tr != transitions.end()) {
        tr->second.to = event.new_class;
        work.SetRaw(event.key, cls);
      } else if ((raw & kDead) ||
                 (!(raw & kGone) && event.key > work.BuildMax())) {
        // Known to have no lineitems: nothing to add.
        work.SetRaw(event.key, static_cast<uint8_t>(cls | kFresh));
      } else {
        // Deleted before with lineitems left behind (or a never-seen key
        // inside the built range): add its existing lineitems' revenue.
        transitions.emplace(event.key,
                            Transition{TpchQ14PromoClass::Absent, event.new_class});
        work.SetRaw(event.key, cls);
      }
      part_changed = true;
      break;
    case core::MutationKind::Delete:
      if (current == TpchQ14PromoClass::Absent) {
        // Another row of a key deleted earlier (bag delete): nothing left.
        if (!(raw & (kDead | kGone)))
          return rebuild_all();
        break;
      }
      if (tr != transitions.end()) {
        tr->second.to = TpchQ14PromoClass::Absent;
        work.SetRaw(event.key, kGone);
      } else if (raw & kFresh) {
        work.SetRaw(event.key, kDead);
      } else {
        transitions.emplace(
            event.key, Transition{current, TpchQ14PromoClass::Absent});
        work.SetRaw(event.key, kGone);
      }
      part_changed = true;
      break;
    case core::MutationKind::Update:
      if (current == TpchQ14PromoClass::Absent)
        return rebuild_all();
      if (current == event.new_class)
        break;
      if (tr != transitions.end())
        tr->second.to = event.new_class;
      else if (!(raw & kFresh))
        transitions.emplace(event.key, Transition{current, event.new_class});
      work.SetRaw(event.key,
                  static_cast<uint8_t>(cls | (raw & kFresh)));
      part_changed = true;
      break;
    }
  }

  std::vector<int64_t> scan_keys;
  for (const auto &[key, tr] : transitions) {
    if (tr.from != tr.to)
      scan_keys.push_back(key);
  }
  if (!scan_keys.empty()) {
    // A part that may have lineitems changed class/presence: its months'
    // totals must move. Mixed with lineitem deltas, rebuild instead.
    if (!initial_scan || !buffer.Deltas().empty())
      return rebuild_all();
    initial_scan->ScanPartMonthRevenue(
        scan_keys,
        [&](int64_t key, int32_t month, int64_t total, int64_t count) {
          const auto it = transitions.find(key);
          if (it == transitions.end())
            return;
          const auto from = it->second.from, to = it->second.to;
          auto &a = adjust_of(adj, month);
          if (to == TpchQ14PromoClass::Absent) {
            a.total_revenue_raw -= total;
            a.row_count -= count;
          } else if (from == TpchQ14PromoClass::Absent) {
            a.total_revenue_raw += total;
            a.row_count += count;
          }
          if (from == TpchQ14PromoClass::Promo)
            a.promo_revenue_raw -= total;
          if (to == TpchQ14PromoClass::Promo)
            a.promo_revenue_raw += total;
        });
  }

  // A delta's class never changes here, so the totals are summed per slice
  // against the unchanged part state; the key-state writes (a lineitem now
  // references the key, or exists for a part not yet inserted) follow serially.
  struct DeltaPass {
    std::vector<Adjust> adj;
    std::vector<uint32_t> writes;
  };
  DeltaPass pass;
  const auto &deltas = buffer.Deltas();
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, pass,
      [&](DeltaPass &acc, std::size_t first, std::size_t last) {
        for (std::size_t i = first; i < last; ++i) {
          const auto &delta = deltas[i];
          const auto raw = work.RawOf(delta.partkey);
          const auto promo =
              static_cast<TpchQ14PromoClass>(raw & TpchQ14State::kClassMask);
          if ((promo == TpchQ14PromoClass::Absent && !(raw & kGone)) ||
              (promo != TpchQ14PromoClass::Absent && (raw & kFresh)))
            acc.writes.push_back(static_cast<uint32_t>(i));
          if (promo == TpchQ14PromoClass::Absent)
            continue;
          auto &a = adjust_of(acc.adj, delta.month);
          const auto signed_revenue =
              static_cast<int64_t>(delta.sign) * delta.revenue_raw;
          a.total_revenue_raw += signed_revenue;
          a.row_count += delta.sign;
          if (promo == TpchQ14PromoClass::Promo)
            a.promo_revenue_raw += signed_revenue;
        }
      },
      [&](DeltaPass &into, DeltaPass &&from) {
        into.writes.insert(into.writes.end(), from.writes.begin(),
                           from.writes.end());
        for (const auto &entry : from.adj)
          AddTotals(adjust_of(into.adj, entry.month), entry.totals);
      });
  for (const auto &entry : pass.adj)
    AddTotals(adjust_of(adj, entry.month), entry.totals);
  for (const auto i : pass.writes) {
    const auto partkey = deltas[i].partkey;
    const auto raw = work.RawOf(partkey);
    if ((raw & TpchQ14State::kClassMask) ==
        static_cast<uint8_t>(TpchQ14PromoClass::Absent)) {
      if (!(raw & kGone)) {
        work.SetRaw(partkey, kGone);
        part_changed = true;
      }
    } else if (raw & kFresh) { // now referenced by lineitems
      work.SetRaw(partkey, static_cast<uint8_t>(raw & ~kFresh));
      part_changed = true;
    }
  }

  // Current month totals + versions: from the combined read; months only
  // reachable through the scan (not touched by a delta) are read now.
  struct Base {
    core::Version version = 0;
    const TpchQ14StateImage *image = nullptr;
  };
  std::vector<Base> base(adj.size());
  core::StateSnapshot extra_snapshot;
  if (!adj.empty()) {
    std::vector<core::PartitionId> missing;
    auto find_month = [&](int32_t month,
                          const core::StateSnapshot &snap) -> const core::StateImage * {
      for (const auto &image : snap.partitions)
        if (image.partition == static_cast<core::PartitionId>(month) &&
            image.partition != core::TpchQ14Layout::kPartPromo)
          return &image;
      return nullptr;
    };
    for (std::size_t i = 0; i < adj.size(); ++i) {
      const auto *image = find_month(adj[i].month, part_snapshot);
      if (!image)
        missing.push_back(static_cast<core::PartitionId>(adj[i].month));
    }
    if (!missing.empty())
      extra_snapshot = services.StateStore().ReadPartitions(visibility, missing);
    for (std::size_t i = 0; i < adj.size(); ++i) {
      const auto *image = find_month(adj[i].month, part_snapshot);
      if (!image)
        image = find_month(adj[i].month, extra_snapshot);
      if (!image)
        return rebuild_all();
      const auto *typed = AsQ14Image(*image, TpchQ14ImageKind::Month);
      if (!typed || typed->month != adj[i].month)
        return rebuild_all();
      base[i].image = typed;
      base[i].version = image->version;
    }
  }

  if (adj.empty() && !part_changed)
    return outcome;
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.reserve(adj.size() + 1);
  for (std::size_t i = 0; i < adj.size(); ++i) {
    const auto month = adj[i].month;
    auto totals = base[i].image->totals;
    totals.total_revenue_raw += adj[i].totals.total_revenue_raw;
    totals.promo_revenue_raw += adj[i].totals.promo_revenue_raw;
    totals.row_count += adj[i].totals.row_count;
    totals.saw_contribution = totals.row_count > 0;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator, static_cast<core::PartitionId>(month),
        base[i].version, MakeMonthImage(month, totals)));
  }
  if (part_changed) {
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ14Layout::kAccelerator, core::TpchQ14Layout::kPartPromo,
        part_version,
        MakeStateImage(TpchQ14ImageKind::PartPromo, 0,
                       std::make_shared<const TpchQ14State>(
                           work.copied ? std::move(work.copy)
                                       : TpchQ14State(*part_state)))));
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
  TpchQ14MonthTotals totals;
  if (!LoadMonth(services, month, totals)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(totals, sink);
}

TpchQ14State TpchQ14Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q14 requires a typed initial scan source");
  TpchQ14State state = core::PartitionedBuild<TpchQ14State>(
      services, [] { return TpchQ14State{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanParts(services, parts, [&](std::size_t p, int64_t key, bool promo) {
          const PartRow part{key, promo};
          sink(p, TypedRowBatch<PartRow>{part});
        });
      },
      [](TpchQ14State &part_state, const PartRow &part) {
        part_state.build_max_partkey =
            part_state.build_max_partkey == std::numeric_limits<int64_t>::max()
                ? part.key
                : std::max(part_state.build_max_partkey, part.key);
        part_state.SetPromo(part.key, part.promo ? TpchQ14PromoClass::Promo
                                                 : TpchQ14PromoClass::NonPromo);
      },
      [](TpchQ14State &into, TpchQ14State &&from) {
        if (from.build_max_partkey != std::numeric_limits<int64_t>::max())
          into.build_max_partkey =
              into.build_max_partkey == std::numeric_limits<int64_t>::max()
                  ? from.build_max_partkey
                  : std::max(into.build_max_partkey, from.build_max_partkey);
        into.classes.MergeFrom(std::move(from.classes), [](uint8_t mine, uint8_t theirs) {
          return theirs ? theirs : mine; // the last part row of a key wins
        });
        for (const auto &[key, raw] : from.promo_overflow)
          into.promo_overflow[key] = raw;
      });

  state.month_totals = core::PartitionedBuild<std::map<int32_t, TpchQ14MonthTotals>>(
      services, [] { return std::map<int32_t, TpchQ14MonthTotals>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ14LineitemInput &input) {
                                      sink(p, TypedRowBatch<TpchQ14LineitemInput>{input});
                                    });
      },
      [&](std::map<int32_t, TpchQ14MonthTotals> &months, const TpchQ14LineitemInput &input) {
        const auto promo = state.PromoOf(input.partkey);
        if (promo == TpchQ14PromoClass::Absent)
          return;
        const auto revenue =
            TpchQ14RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                              input.discount_raw, input.discount_scale);
        auto &totals = months[TpchQ14MonthKeyFromDays(input.shipdate_days)];
        totals.total_revenue_raw += revenue;
        totals.row_count++;
        totals.saw_contribution = true;
        if (promo == TpchQ14PromoClass::Promo)
          totals.promo_revenue_raw += revenue;
      },
      [](std::map<int32_t, TpchQ14MonthTotals> &into,
         std::map<int32_t, TpchQ14MonthTotals> &&from) {
        for (const auto &[month, totals] : from) {
          AddTotals(into[month], totals);
          into[month].saw_contribution = true;
        }
      });
  return state;
}

// Validity and the month's image in one linearized observation (a missing
// month, an invalid store or a foreign payload all mean "use the fallback").
bool TpchQ14Module::LoadMonth(core::AuxEngineServices &services, int32_t month,
                              TpchQ14MonthTotals &totals) const {
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = static_cast<core::PartitionId>(month);
  const auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, wanted);
  if (!snapshot.available || snapshot.partitions.size() != 1)
    return false;
  const auto *typed =
      AsQ14Image(snapshot.partitions[0], TpchQ14ImageKind::Month);
  if (!typed || typed->month != month)
    return false;
  totals = typed->totals;
  return true;
}

void TpchQ14Module::EmitResult(const TpchQ14MonthTotals &totals,
                               core::AuxRowSink &sink) const {
  const auto value = TpchQ14FinalizePromoRevenue(totals);
  static const core::OutputSchema kSchema{
      {{"promo_revenue", core::AuxType::Double, true}}};
  sink.Begin(kSchema);
  core::AuxRow row;
  row.values.resize(1);
  row.values[0].type = core::AuxType::Double;
  if (std::isnan(value)) {
    row.values[0].is_null = true;
  } else {
    row.values[0].is_null = false;
    row.values[0].payload = value;
  }
  sink.WriteRow(row);
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ14Module::CalibrationWrites() const {
  return TpchQ14CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
