#include "tpch/q15/mysql/aux/module.hpp"
#include "tpch/q15/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/sql_date.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

namespace sql_date = core::sql_date;
using Int128 = __int128;

// Partition ids: kSupplierDirectory = 0; month partitions are MonthIndex + bias.
constexpr int64_t kMonthPartitionBias = 1000000;
// Result scale: extendedprice (2) * (1 - discount) (2).
constexpr int32_t kRevenueScale = 4;
// Dates this close to the int32 limits are PostgreSQL +/-infinity sentinels.
constexpr int64_t kFiniteDateLimit = std::numeric_limits<int32_t>::max() - 1;

core::PartitionId MonthPartition(int64_t month_index) {
  return static_cast<core::PartitionId>(month_index + kMonthPartitionBias);
}

int64_t MonthStartDays(int64_t month_index) {
  int64_t y = month_index / 12, m = month_index % 12;
  if (m < 0) {
    m += 12;
    --y;
  }
  return sql_date::DaysFromCivil(y, static_cast<int>(m) + 1, 1);
}

// ── Row decoding ──────────────────────────────────────────────────────────

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
  if (!std::holds_alternative<core::ScaledDecimal>(cell.payload))
    return false;
  const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
  raw = decimal.value;
  scale = decimal.scale;
  return true;
}

// A row is "readable" when every column is present and well typed. A row with
// a NULL shipdate / suppkey / price / discount never contributes to any window
// (NULL comparison, NULL group, NULL sum) and is skipped by the caller.
enum class RowStatus { Ok, Skip, Bad };

RowStatus ReadLineitem(const core::AuxRow &row, TpchQ15LineitemInput &out) {
  if (row.values.size() < 4)
    return RowStatus::Bad;
  for (std::size_t i = 0; i < 4; ++i)
    if (row.values[i].is_null)
      return RowStatus::Skip;
  int64_t shipdate = 0;
  // Mutation rows are [shipdate, suppkey, extendedprice, discount].
  if (!IntCell(row, 0, shipdate) || !IntCell(row, 1, out.suppkey) ||
      !RawDecimalCell(row, 2, out.extended_price_raw,
                      out.extended_price_scale) ||
      !RawDecimalCell(row, 3, out.discount_raw, out.discount_scale))
    return RowStatus::Bad;
  if (shipdate < std::numeric_limits<int32_t>::min() ||
      shipdate > std::numeric_limits<int32_t>::max())
    return RowStatus::Bad;
  out.shipdate_days = static_cast<int32_t>(shipdate);
  return RowStatus::Ok;
}

bool ReadSupplier(const core::AuxRow &row, TpchQ15SupplierInput &out) {
  return IntCell(row, 0, out.suppkey) && StringCell(row, 1, out.name) &&
         StringCell(row, 2, out.address) && StringCell(row, 3, out.phone);
}

// ── Supplier directory packing ───────────────────────────────────────────

void PackField(std::string &out, const std::string &s) {
  const uint32_t n = static_cast<uint32_t>(s.size());
  out.append(reinterpret_cast<const char *>(&n), sizeof(n));
  out.append(s);
}

std::string PackSupplier(const std::string &name, const std::string &address,
                         const std::string &phone) {
  std::string out;
  out.reserve(12 + name.size() + address.size() + phone.size());
  PackField(out, name);
  PackField(out, address);
  PackField(out, phone);
  return out;
}

void UnpackSupplier(const std::string &packed, std::string &name,
                    std::string &address, std::string &phone) {
  std::size_t pos = 0;
  std::string *fields[3] = {&name, &address, &phone};
  for (auto *field : fields) {
    uint32_t n = 0;
    std::memcpy(&n, packed.data() + pos, sizeof(n));
    pos += sizeof(n);
    field->assign(packed, pos, n);
    pos += n;
  }
}

// ── Per-day lists and per-month columns ──────────────────────────────────

struct DayPosition {
  int64_t month_index = 0;
  int day_offset = 0; // 0..30
};

DayPosition PositionOf(int64_t days) {
  const auto civil = sql_date::CivilFromDays(days);
  return {static_cast<int64_t>(civil.year) * 12 + (civil.month - 1),
          civil.day - 1};
}

bool FitsNarrow(int64_t suppkey, int64_t revenue, int64_t count) {
  return count == 1 && suppkey >= 0 && suppkey <= UINT32_MAX && revenue >= 0 &&
         revenue <= UINT32_MAX;
}

uint64_t PackNarrow(int64_t suppkey, int64_t revenue) {
  return (static_cast<uint64_t>(suppkey) << 32) | static_cast<uint64_t>(revenue);
}

// Adds a signed revenue / row-count delta of one supplier to a month's dense
// columns, keeping the exact value when it exceeds int64.
void AddToMonthColumns(TpchQ15MonthState &month, int64_t suppkey,
                       int64_t revenue, int32_t count) {
  int64_t &slot = month.revenue.Mutable(suppkey);
  Int128 current = slot;
  if (slot == kTpchQ15WideRevenue) {
    current = month.wide_revenue[suppkey];
  }
  Int128 total = current;
  if (__builtin_add_overflow(current, static_cast<Int128>(revenue), &total))
    throw std::overflow_error("TPC-H Q15 revenue overflow");
  if (total > kTpchQ15WideRevenue && total <= std::numeric_limits<int64_t>::max()) {
    slot = static_cast<int64_t>(total);
    month.wide_revenue.erase(suppkey);
  } else {
    slot = kTpchQ15WideRevenue;
    month.wide_revenue[suppkey] = total;
  }
  month.count.Mutable(suppkey) += count;
}

template <typename F>
void ForEachDayEntry(const TpchQ15DayList &list, F &&fn) {
  if (list.narrow)
    for (const uint64_t packed : *list.narrow)
      fn(static_cast<int64_t>(packed >> 32),
         static_cast<int64_t>(packed & 0xffffffffu), int64_t{1});
  if (list.wide)
    for (const auto &e : *list.wide)
      fn(e.suppkey, e.revenue_raw, e.count);
  if (list.overlay)
    for (const auto &e : *list.overlay)
      fn(e.suppkey, e.revenue_raw, e.count);
}

// Merges base + overlay into a fresh base (grouped by supplier, sorted).
void CompactDay(TpchQ15DayList &list) {
  std::vector<TpchQ15DayEntry> all;
  ForEachDayEntry(list, [&](int64_t s, int64_t r, int64_t c) {
    all.push_back({s, r, c});
  });
  std::sort(all.begin(), all.end(),
            [](const auto &a, const auto &b) { return a.suppkey < b.suppkey; });
  std::vector<uint64_t> narrow;
  std::vector<TpchQ15DayEntry> wide;
  for (std::size_t i = 0; i < all.size();) {
    std::size_t j = i;
    Int128 revenue = 0;
    int64_t count = 0;
    for (; j < all.size() && all[j].suppkey == all[i].suppkey; ++j) {
      revenue += all[j].revenue_raw;
      count += all[j].count;
    }
    if (revenue > std::numeric_limits<int64_t>::max() ||
        revenue < std::numeric_limits<int64_t>::min())
      throw std::overflow_error("TPC-H Q15 day revenue overflow");
    const auto rev = static_cast<int64_t>(revenue);
    if (count == 0 && rev == 0) {
      // fully cancelled
    } else if (FitsNarrow(all[i].suppkey, rev, count)) {
      narrow.push_back(PackNarrow(all[i].suppkey, rev));
    } else {
      wide.push_back({all[i].suppkey, rev, count});
    }
    i = j;
  }
  list.narrow = narrow.empty() ? nullptr
                               : std::make_shared<const std::vector<uint64_t>>(
                                     std::move(narrow));
  list.wide = wide.empty()
                  ? nullptr
                  : std::make_shared<const std::vector<TpchQ15DayEntry>>(
                        std::move(wide));
  list.overlay = nullptr;
}

// Appends signed entries to a day's overlay; compacts when it outgrows its base.
void FoldIntoDay(TpchQ15DayList &list, std::vector<TpchQ15DayEntry> &&adds) {
  auto overlay = std::make_shared<std::vector<TpchQ15DayEntry>>();
  if (list.overlay)
    *overlay = *list.overlay;
  overlay->insert(overlay->end(), adds.begin(), adds.end());
  const std::size_t base = (list.narrow ? list.narrow->size() : 0) +
                           (list.wide ? list.wide->size() : 0);
  list.overlay = std::move(overlay);
  if (list.overlay->size() > 64 + base / 16)
    CompactDay(list);
}

// ── Time/revenue helpers ──────────────────────────────────────────────────

int64_t Pow10(int32_t n) {
  int64_t v = 1;
  while (n-- > 0)
    v *= 10;
  return v;
}

const TpchQ15StateImage *AsQ15Image(const core::StateImage &image,
                                    TpchQ15ImageKind kind) {
  const auto *payload = image.As<TpchQ15StateImage>();
  return payload && payload->kind == kind ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::StateImage MakeDirImage(std::shared_ptr<const TpchQ15SupplierDirectory> d) {
  auto image = std::make_shared<TpchQ15StateImage>();
  image->kind = TpchQ15ImageKind::SupplierDirectory;
  image->directory = std::move(d);
  return core::StateImage::Make(TpchQ15Layout::kAccelerator,
                                TpchQ15Layout::kSupplierDirectory, 0,
                                std::shared_ptr<const TpchQ15StateImage>(image));
}

core::StateImage MakeMonthImage(int64_t month_index,
                                std::shared_ptr<const TpchQ15MonthState> m) {
  auto image = std::make_shared<TpchQ15StateImage>();
  image->kind = TpchQ15ImageKind::MonthWindow;
  image->month_index = static_cast<int32_t>(month_index);
  image->month = std::move(m);
  return core::StateImage::Make(TpchQ15Layout::kAccelerator,
                                MonthPartition(month_index), 0,
                                std::shared_ptr<const TpchQ15StateImage>(image));
}

// Reads the query argument; nullopt for SQL NULL (and for infinite dates):
// the window is then empty.
std::optional<int32_t> StartArgument(const core::QueryArguments &arguments) {
  if (arguments.values.empty() || arguments.values[0].is_null)
    return std::nullopt;
  int64_t days = 0;
  const auto &payload = arguments.values[0].payload;
  if (std::holds_alternative<core::AuxDate>(payload))
    days = std::get<core::AuxDate>(payload).days_since_epoch;
  else if (std::holds_alternative<int64_t>(payload))
    days = std::get<int64_t>(payload);
  else
    return std::nullopt;
  if (days > kFiniteDateLimit || days < -kFiniteDateLimit)
    return std::nullopt;
  return static_cast<int32_t>(days);
}

core::OutputSchema Schema() {
  return {{{"s_suppkey", core::AuxType::Int64, true},
           {"s_name", core::AuxType::String, true},
           {"s_address", core::AuxType::String, true},
           {"s_phone", core::AuxType::String, true},
           {"total_revenue", core::AuxType::ScaledDecimal, true}}};
}

void EmitRow(core::AuxRowSink &sink, int64_t suppkey, const std::string &name,
             const std::string &address, const std::string &phone,
             Int128 revenue) {
  if (revenue > std::numeric_limits<int64_t>::max() ||
      revenue < std::numeric_limits<int64_t>::min())
    throw std::overflow_error("TPC-H Q15 total_revenue exceeds int64");
  core::ScaledDecimal decimal{static_cast<int64_t>(revenue), kRevenueScale};
  sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, suppkey),
                  core::AuxValue::Of(core::AuxType::String, name),
                  core::AuxValue::Of(core::AuxType::String, address),
                  core::AuxValue::Of(core::AuxType::String, phone),
                  core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal)}});
}

} // namespace

// ── Helpers ───────────────────────────────────────────────────────────────

int32_t TpchQ15MonthKeyFromDays(int32_t days_since_epoch) {
  return static_cast<int32_t>(sql_date::MonthIndex(days_since_epoch));
}

int32_t TpchQ15DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return static_cast<int32_t>(sql_date::DaysFromCivil(year, month, day));
}

bool TpchQ15IsFirstOfMonth(int32_t days_since_epoch) {
  return sql_date::IsMonthStart(days_since_epoch);
}

int32_t TpchQ15AddMonths(int32_t days_since_epoch, int32_t months) {
  return static_cast<int32_t>(sql_date::AddMonths(days_since_epoch, months));
}

// extendedprice * (1 - discount) as an exact scale-4 integer.
int64_t TpchQ15RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                          int64_t discount_raw, int32_t discount_scale) {
  Int128 value = static_cast<Int128>(extended_price_raw) *
                 (Pow10(discount_scale) - static_cast<Int128>(discount_raw));
  int32_t scale = extended_scale + discount_scale;
  if (scale > kRevenueScale)
    throw std::logic_error("TPC-H Q15: revenue scale above 4 is unsupported");
  for (; scale < kRevenueScale; ++scale)
    value *= 10;
  if (value > std::numeric_limits<int64_t>::max() ||
      value <= std::numeric_limits<int64_t>::min())
    throw std::overflow_error("TPC-H Q15 lineitem revenue overflow");
  return static_cast<int64_t>(value);
}

// ── TxnBuffer ─────────────────────────────────────────────────────────────

bool TpchQ15TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ15TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitem_deltas.clear();
  supplier_deltas.clear();
}
bool TpchQ15TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ15LineitemDelta> &TpchQ15TxnBuffer::LineitemDeltas() const {
  return lineitem_deltas;
}
const std::vector<TpchQ15SupplierDelta> &TpchQ15TxnBuffer::SupplierDeltas() const {
  return supplier_deltas;
}
void TpchQ15TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  lineitem_deltas.clear();
  supplier_deltas.clear();
}
void TpchQ15TxnBuffer::AddLineitemDelta(TpchQ15LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ15TxnBuffer::AddSupplierDelta(TpchQ15SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(std::move(delta));
}

// ── Module ────────────────────────────────────────────────────────────────

core::AcceleratorId TpchQ15Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ15Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3}, {10, 2, 5, 6}},
          {config.supplier_table, {0, 1, 2, 3}, {0, 1, 2, 4}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ15Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ15TxnBuffer>();
}

void TpchQ15Module::Build(core::AuxEngineServices &,
                          core::StateBuilder &builder) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q15 requires a typed initial scan source");

  auto directory = std::make_shared<TpchQ15SupplierDirectory>();
  initial_scan->ScanSuppliers([&](const TpchQ15SupplierInput &input) {
    directory->entries.Set(input.suppkey,
                           PackSupplier(input.name, input.address, input.phone));
  });
  builder.Write(MakeDirImage(std::move(directory)));

  // Month columns are filled in place; the per-day lists accumulate in plain
  // vectors and are frozen (shrunk to size) when the scan ends.
  struct MonthBuild {
    TpchQ15MonthState state;
    std::array<std::vector<uint64_t>, 31> narrow;
    std::array<std::vector<TpchQ15DayEntry>, 31> wide;
  };
  std::map<int64_t, MonthBuild> months;
  initial_scan->ScanLineitems([&](const TpchQ15LineitemInput &input) {
    const int64_t revenue = TpchQ15RevenueRaw(
        input.extended_price_raw, input.extended_price_scale,
        input.discount_raw, input.discount_scale);
    const auto pos = PositionOf(input.shipdate_days);
    auto &mb = months[pos.month_index];
    AddToMonthColumns(mb.state, input.suppkey, revenue, 1);
    if (FitsNarrow(input.suppkey, revenue, 1))
      mb.narrow[pos.day_offset].push_back(PackNarrow(input.suppkey, revenue));
    else
      mb.wide[pos.day_offset].push_back({input.suppkey, revenue, 1});
  });
  for (auto it = months.begin(); it != months.end();) {
    auto &mb = it->second;
    for (int d = 0; d < 31; ++d) {
      if (!mb.narrow[d].empty()) {
        mb.state.days[d].narrow = std::make_shared<const std::vector<uint64_t>>(
            mb.narrow[d].begin(), mb.narrow[d].end());
        std::vector<uint64_t>().swap(mb.narrow[d]);
      }
      if (!mb.wide[d].empty())
        mb.state.days[d].wide =
            std::make_shared<const std::vector<TpchQ15DayEntry>>(
                std::move(mb.wide[d]));
    }
    builder.Write(MakeMonthImage(
        it->first, std::make_shared<const TpchQ15MonthState>(std::move(mb.state))));
    it = months.erase(it);
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ15Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.supplier_table)
    return All();
  return {};
}

void TpchQ15Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ15TxnBuffer &>(base_buffer);

  if (mutation.table == config.lineitem_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ15LineitemInput input;
      switch (ReadLineitem(row, input)) {
      case RowStatus::Skip:
        return;
      case RowStatus::Bad:
        buffer.MarkRebuild();
        return;
      case RowStatus::Ok:
        break;
      }
      TpchQ15LineitemDelta delta;
      delta.shipdate_days = input.shipdate_days;
      delta.suppkey = input.suppkey;
      delta.revenue_raw = TpchQ15RevenueRaw(
          input.extended_price_raw, input.extended_price_scale,
          input.discount_raw, input.discount_scale);
      delta.sign = sign;
      buffer.AddLineitemDelta(delta);
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
  } else if (mutation.table == config.supplier_table) {
    auto make = [&](const core::AuxRow *old_row, const core::AuxRow *new_row) {
      TpchQ15SupplierDelta delta;
      TpchQ15SupplierInput in;
      if (old_row) {
        if (!ReadSupplier(*old_row, in)) {
          buffer.MarkRebuild();
          return;
        }
        delta.has_old = true;
        delta.old_suppkey = in.suppkey;
      }
      if (new_row) {
        if (!ReadSupplier(*new_row, in)) {
          buffer.MarkRebuild();
          return;
        }
        delta.has_new = true;
        delta.new_suppkey = in.suppkey;
        delta.new_name = std::move(in.name);
        delta.new_address = std::move(in.address);
        delta.new_phone = std::move(in.phone);
      }
      buffer.AddSupplierDelta(std::move(delta));
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        make(nullptr, &row);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        make(&row, nullptr);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i)
        make(&mutation.old_rows[i], &mutation.new_rows[i]);
      break;
    }
  }
}

core::StagedMaintenanceOutcome
TpchQ15Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ15TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (buffer.LineitemDeltas().empty() && buffer.SupplierDeltas().empty())
    return outcome;

  // Honor the cost model's (or a forced) choice: this state has no finer
  // partitions, so anything but ApplyDelta is a whole-state rebuild.
  {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(buffer.LineitemDeltas().size() + buffer.SupplierDeltas().size()), 1, true, false, true, false, false,
         core::MaintenanceAction::NoOp,
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
  }

  // Group the lineitem deltas by (month, day).
  struct MonthDeltas {
    std::array<std::vector<TpchQ15DayEntry>, 31> days;
    std::vector<std::pair<int, const TpchQ15LineitemDelta *>> rows;
  };
  std::map<int64_t, MonthDeltas> by_month;
  for (const auto &delta : buffer.LineitemDeltas()) {
    const auto pos = PositionOf(delta.shipdate_days);
    auto &md = by_month[pos.month_index];
    md.days[pos.day_offset].push_back(
        {delta.suppkey, static_cast<int64_t>(delta.sign) * delta.revenue_raw,
         delta.sign});
    md.rows.emplace_back(pos.day_offset, &delta);
  }

  const auto visibility = services.CurrentVisibility();
  std::vector<core::PartitionId> wanted;
  if (!buffer.SupplierDeltas().empty())
    wanted.push_back(TpchQ15Layout::kSupplierDirectory);
  for (const auto &entry : by_month)
    wanted.push_back(MonthPartition(entry.first));
  const auto snapshot = services.StateStore().ReadPartitions(visibility, wanted);

  std::shared_ptr<const TpchQ15SupplierDirectory> old_directory;
  std::map<int64_t, std::shared_ptr<const TpchQ15MonthState>> old_months;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ15Layout::kSupplierDirectory) {
      if (const auto *typed = AsQ15Image(image, TpchQ15ImageKind::SupplierDirectory))
        old_directory = typed->directory;
    } else if (const auto *typed =
                   AsQ15Image(image, TpchQ15ImageKind::MonthWindow)) {
      old_months[typed->month_index] = typed->month;
    }
  }
  if (!buffer.SupplierDeltas().empty() && !old_directory) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;

  if (!buffer.SupplierDeltas().empty()) {
    auto directory = std::make_shared<TpchQ15SupplierDirectory>(*old_directory);
    for (const auto &delta : buffer.SupplierDeltas()) {
      if (delta.has_old)
        directory->entries.Set(delta.old_suppkey, std::string());
      if (delta.has_new)
        directory->entries.Set(
            delta.new_suppkey,
            PackSupplier(delta.new_name, delta.new_address, delta.new_phone));
    }
    outcome.updates.push_back(MakeDirImage(std::move(directory)));
  }

  for (auto &[month_index, md] : by_month) {
    // A copy shares every COW chunk and day list; only touched ones clone.
    const auto old = old_months.find(month_index);
    auto month = old != old_months.end() && old->second
                     ? std::make_shared<TpchQ15MonthState>(*old->second)
                     : std::make_shared<TpchQ15MonthState>();
    for (const auto &[day, delta] : md.rows)
      AddToMonthColumns(*month, delta->suppkey,
                        static_cast<int64_t>(delta->sign) * delta->revenue_raw,
                        delta->sign);
    for (int d = 0; d < 31; ++d)
      if (!md.days[d].empty())
        FoldIntoDay(month->days[d], std::move(md.days[d]));
    outcome.updates.push_back(MakeMonthImage(
        month_index, std::shared_ptr<const TpchQ15MonthState>(std::move(month))));
  }
  return outcome;
}

bool TpchQ15Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ15Module::EmitEmpty(core::AuxRowSink &sink) {
  sink.Begin(Schema());
  sink.End();
}

void TpchQ15Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  const auto start_arg = StartArgument(arguments);
  if (!start_arg) {
    EmitEmpty(sink); // l_shipdate >= NULL is never true
    return;
  }
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const int64_t start = *start_arg;
  const int64_t end = sql_date::AddMonths(start, 3); // exclusive
  const int64_t first_month = sql_date::MonthIndex(start);
  const int64_t last_month = sql_date::MonthIndex(end - 1);

  std::vector<core::PartitionId> wanted{TpchQ15Layout::kSupplierDirectory};
  for (int64_t m = first_month; m <= last_month; ++m)
    wanted.push_back(MonthPartition(m));
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), wanted);

  const TpchQ15SupplierDirectory *directory = nullptr;
  std::map<int64_t, const TpchQ15MonthState *> months;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ15Layout::kSupplierDirectory) {
      if (const auto *typed = AsQ15Image(image, TpchQ15ImageKind::SupplierDirectory))
        directory = typed->directory.get();
    } else if (const auto *typed =
                   AsQ15Image(image, TpchQ15ImageKind::MonthWindow)) {
      months[typed->month_index] = typed->month.get();
    }
  }
  if (!directory) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // Whole months are summed from the dense columns; the edge days of a window
  // that does not start/end on a month boundary are scattered into scratch.
  std::vector<const TpchQ15MonthState *> whole;
  core::AnyKeyCowArray<Int128> edge_revenue{0};
  core::AnyKeyCowArray<int64_t> edge_count{0};
  bool have_edge = false;
  for (int64_t m = first_month; m <= last_month; ++m) {
    const auto it = months.find(m);
    if (it == months.end() || !it->second)
      continue; // no lineitem in that month
    const int64_t month_start = MonthStartDays(m);
    const int64_t month_end = MonthStartDays(m + 1);
    if (start <= month_start && month_end <= end) {
      whole.push_back(it->second);
      continue;
    }
    for (int64_t day = std::max(start, month_start);
         day < std::min(end, month_end); ++day) {
      ForEachDayEntry(it->second->days[day - month_start],
                      [&](int64_t s, int64_t r, int64_t c) {
                        edge_revenue.Mutable(s) += r;
                        edge_count.Mutable(s) += c;
                        have_edge = true;
                      });
    }
  }

  std::vector<std::size_t> chunk_ids;
  for (const auto *ms : whole)
    ms->revenue.ForEachChunk(
        [&](std::size_t ci, const std::vector<int64_t> &) { chunk_ids.push_back(ci); });
  if (have_edge)
    edge_revenue.ForEachChunk(
        [&](std::size_t ci, const std::vector<Int128> &) { chunk_ids.push_back(ci); });
  std::sort(chunk_ids.begin(), chunk_ids.end());
  chunk_ids.erase(std::unique(chunk_ids.begin(), chunk_ids.end()),
                  chunk_ids.end());

  // Sweep: exact (int128) per-supplier sums; a supplier is a group iff it has
  // at least one lineitem in the window. Track the max and its holders.
  constexpr std::size_t kChunk = core::AnyKeyCowArray<int64_t>::kChunkSize;
  std::vector<Int128> sum(kChunk);
  std::vector<int64_t> rows(kChunk);
  Int128 best = 0;
  bool have_best = false;
  std::vector<int64_t> winners;
  const auto consider = [&](int64_t key, const Int128 &total, int64_t n) {
    if (n <= 0)
      return;
    if (!have_best || total > best) {
      best = total;
      have_best = true;
      winners.clear();
    }
    if (total == best)
      winners.push_back(key);
  };
  for (const std::size_t cid : chunk_ids) {
    std::fill(sum.begin(), sum.end(), Int128{0});
    std::fill(rows.begin(), rows.end(), int64_t{0});
    for (const auto *ms : whole) {
      const auto *rc = ms->revenue.ChunkAt(cid);
      const auto *cc = ms->count.ChunkAt(cid);
      if (!rc || !cc)
        continue;
      for (std::size_t i = 0; i < kChunk; ++i) {
        int64_t v = (*rc)[i];
        if (v == kTpchQ15WideRevenue) {
          const int64_t key = core::AnyKeyCowArray<int64_t>::KeyAt(cid, i);
          sum[i] += ms->wide_revenue.at(key);
        } else {
          sum[i] += v;
        }
        rows[i] += (*cc)[i];
      }
    }
    if (have_edge) {
      if (const auto *rc = edge_revenue.ChunkAt(cid)) {
        const auto *cc = edge_count.ChunkAt(cid);
        for (std::size_t i = 0; i < kChunk; ++i) {
          sum[i] += (*rc)[i];
          rows[i] += (*cc)[i];
        }
      }
    }
    for (std::size_t i = 0; i < kChunk; ++i)
      consider(core::AnyKeyCowArray<int64_t>::KeyAt(cid, i), sum[i], rows[i]);
  }
  // Suppliers outside the dense key range (negative or huge keys) live in the
  // arrays' sparse parts; fold them the same way.
  std::map<int64_t, std::pair<Int128, int64_t>> far;
  for (const auto *ms : whole) {
    ms->revenue.ForEachSparse([&](int64_t key, const int64_t &v) {
      far[key].first += v == kTpchQ15WideRevenue ? ms->wide_revenue.at(key) : v;
    });
    ms->count.ForEachSparse(
        [&](int64_t key, const int32_t &c) { far[key].second += c; });
  }
  if (have_edge) {
    edge_revenue.ForEachSparse(
        [&](int64_t key, const Int128 &v) { far[key].first += v; });
    edge_count.ForEachSparse(
        [&](int64_t key, const int64_t &c) { far[key].second += c; });
  }
  for (const auto &entry : far)
    consider(entry.first, entry.second.first, entry.second.second);
  std::sort(winners.begin(), winners.end());

  sink.Begin(Schema());
  // Join supplier attributes at emit time (winners are already by suppkey).
  for (const int64_t suppkey : winners) {
    const std::string *packed = directory->entries.Find(suppkey);
    if (!packed || packed->empty())
      continue;
    std::string name, address, phone;
    UnpackSupplier(*packed, name, address, phone);
    EmitRow(sink, suppkey, name, address, phone, best);
  }
  sink.End();
}

// Exact SQL-side answer from the tables (own uncommitted writes, no state).
void TpchQ15Module::ExecuteFallback(core::AuxEngineServices &,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  const auto start_arg = StartArgument(arguments);
  if (!start_arg) {
    EmitEmpty(sink);
    return;
  }
  if (!initial_scan)
    throw std::logic_error("TPC-H Q15 requires a typed initial scan source");

  const int32_t start = *start_arg;
  const int32_t end = TpchQ15AddMonths(start, 3);

  struct Group {
    Int128 revenue = 0;
    int64_t rows = 0;
  };
  std::unordered_map<int64_t, Group> groups;
  initial_scan->ScanLineitemsForShipdateRange(
      start, end, [&](const TpchQ15LineitemInput &input) {
        if (input.shipdate_days < start || input.shipdate_days >= end)
          return;
        auto &g = groups[input.suppkey];
        g.revenue += TpchQ15RevenueRaw(
            input.extended_price_raw, input.extended_price_scale,
            input.discount_raw, input.discount_scale);
        ++g.rows;
      });
  if (groups.empty()) {
    EmitEmpty(sink);
    return;
  }
  Int128 best = groups.begin()->second.revenue;
  for (const auto &[sk, g] : groups)
    best = std::max(best, g.revenue);
  std::set<int64_t> winners;
  for (const auto &[sk, g] : groups)
    if (g.revenue == best)
      winners.insert(sk);

  struct Record {
    std::string name, address, phone;
  };
  std::map<int64_t, Record> found;
  initial_scan->ScanSuppliers([&](const TpchQ15SupplierInput &input) {
    if (winners.count(input.suppkey))
      found[input.suppkey] = {input.name, input.address, input.phone};
  });
  sink.Begin(Schema());
  for (const auto &[suppkey, record] : found)
    EmitRow(sink, suppkey, record.name, record.address, record.phone, best);
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ15Module::CalibrationWrites() const {
  return TpchQ15CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
