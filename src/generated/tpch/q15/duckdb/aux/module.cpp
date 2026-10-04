#include "tpch/q15/duckdb/aux/module.hpp"
#include "tpch/q15/calibration_writes.hpp"
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

constexpr uint32_t kVersion = 2;
constexpr uint32_t kDirMagic = 0x44313551;   // Q15D
constexpr uint32_t kWindowMagic = 0x57313551; // Q15W
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

int32_t StartOfMonthDays(int32_t days) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days, year, month, day);
  return DaysFromCivil(year, month, 1);
}

bool IsFirstOfMonth(int32_t days) {
  return days == StartOfMonthDays(days);
}

int32_t AddMonthsDays(int32_t days, int32_t months) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days, year, month, day);
  const int total = year * 12 + static_cast<int>(month) - 1 + months;
  const int new_year = (total >= 0 ? total : total - 11) / 12;
  const unsigned new_month = static_cast<unsigned>(total - new_year * 12 + 1);
  const unsigned last_day = (new_month == 1 || new_month == 3 || new_month == 5 ||
                             new_month == 7 || new_month == 8 || new_month == 10 ||
                             new_month == 12)
                                ? 31
                            : new_month == 4 || new_month == 6 ||
                                    new_month == 9 || new_month == 11
                                ? 30
                            : (new_year % 4 == 0 && (new_year % 100 != 0 ||
                                                     new_year % 400 == 0))
                                ? 29
                                : 28;
  return DaysFromCivil(new_year, new_month,
                       std::min(day, static_cast<unsigned>(last_day)));
}

// Each lineitem with shipdate days S falls into up to three month-start
// 3-month rolling windows: those starting at StartOfMonth(S-2m),
// StartOfMonth(S-1m), and StartOfMonth(S).
std::vector<int32_t> CoveringWindowMonths(int32_t shipdate_days) {
  std::vector<int32_t> months;
  const int32_t m0 = StartOfMonthDays(shipdate_days);
  months.push_back(MonthKey(m0));
  const int32_t m_1 = StartOfMonthDays(AddMonthsDays(m0, -1));
  months.push_back(MonthKey(m_1));
  const int32_t m_2 = StartOfMonthDays(AddMonthsDays(m_1, -1));
  months.push_back(MonthKey(m_2));
  return months;
}

int32_t PrevMonthKey(int32_t key) {
  return key % 100 == 1 ? key - 100 + 11 : key - 1;
}

// Window keys (month-start YYYYMM) of the three rolling windows containing a
// month: the month itself and the two before it.
void CoveringWindowKeysOfMonth(int32_t month_key, int32_t out[3]) {
  out[0] = month_key;
  out[1] = PrevMonthKey(out[0]);
  out[2] = PrevMonthKey(out[1]);
}

std::shared_ptr<const TpchQ15StateImage>
MakeDirImage(const std::shared_ptr<const TpchQ15State> &state) {
  auto image = std::make_shared<TpchQ15StateImage>();
  image->kind = TpchQ15ImageKind::SupplierDirectory;
  image->month_key = 0;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ15StateImage>
MakeWindowImage(int32_t month_key,
                const std::shared_ptr<const TpchQ15State> &state) {
  auto image = std::make_shared<TpchQ15StateImage>();
  image->kind = TpchQ15ImageKind::MonthWindow;
  image->month_key = month_key;
  image->state = state;
  return image;
}

const TpchQ15StateImage *AsQ15Image(const core::StateImage &image,
                                     TpchQ15ImageKind kind) {
  const auto *payload = image.As<TpchQ15StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ15LineitemInput &out) {
  int64_t suppkey = 0;
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  // Mutation rows are [shipdate, suppkey, extendedprice, discount].
  if (!DateCell(row, 0, out.shipdate_days) ||
      !IntCell(row, 1, suppkey) ||
      !RawDecimalCell(row, 2, extended_raw, extended_scale) ||
      !RawDecimalCell(row, 3, discount_raw, discount_scale))
    return false;
  out.suppkey = suppkey;
  out.extended_price_raw = extended_raw;
  out.extended_price_scale = extended_scale;
  out.discount_raw = discount_raw;
  out.discount_scale = discount_scale;
  return true;
}

bool ReadSupplier(const core::AuxRow &row, TpchQ15SupplierInput &out) {
  int64_t suppkey = 0;
  if (!IntCell(row, 0, suppkey))
    return false;
  out.suppkey = suppkey;
  if (!StringCell(row, 1, out.name))
    return false;
  if (!StringCell(row, 2, out.address))
    return false;
  if (!StringCell(row, 3, out.phone))
    return false;
  return true;
}

} // namespace

// ── State containers ──────────────────────────────────────────────────────

int64_t TpchQ15RevArray::Get(int64_t suppkey) const {
  const auto it = chunks.find(suppkey >> kQ15RevChunkShift);
  return it == chunks.end() ? 0 : it->second->v[suppkey & (kQ15RevChunkSize - 1)];
}

int64_t TpchQ15RevArray::Add(int64_t suppkey, int64_t delta) {
  auto &slot = chunks[suppkey >> kQ15RevChunkShift];
  if (!slot)
    slot = std::make_shared<TpchQ15RevChunk>();
  else if (slot.use_count() > 1)
    slot = std::make_shared<TpchQ15RevChunk>(*slot);
  int64_t &cell = slot->v[suppkey & (kQ15RevChunkSize - 1)];
  const int64_t old = cell;
  cell += delta;
  return old;
}

void TpchQ15WindowState::Rescan() {
  int64_t count = 0;
  int64_t max_rev = std::numeric_limits<int64_t>::min();
  winners.clear();
  for (const auto &[cid, chunk] : rev.chunks) {
    for (int64_t i = 0; i < kQ15RevChunkSize; ++i) {
      const int64_t r = chunk->v[i];
      if (r == 0)
        continue;
      ++count;
      if (r > max_rev) {
        max_rev = r;
        winners.clear();
        winners.push_back((cid << kQ15RevChunkShift) | i);
      } else if (r == max_rev) {
        winners.push_back((cid << kQ15RevChunkShift) | i);
      }
    }
  }
  nonzero = count;
  empty = count == 0;
  max_revenue = count == 0 ? 0 : max_rev;
}

const TpchQ15SupplierRecord *
TpchQ15SupplierDirectory::Find(int64_t suppkey) const {
  const auto it = chunks.find(suppkey >> kQ15DirChunkShift);
  if (it == chunks.end())
    return nullptr;
  const auto &items = it->second->items;
  const auto pos = std::lower_bound(
      items.begin(), items.end(), suppkey,
      [](const auto &e, int64_t k) { return e.first < k; });
  return pos != items.end() && pos->first == suppkey ? pos->second.get()
                                                     : nullptr;
}

void TpchQ15SupplierDirectory::Set(int64_t suppkey, TpchQ15SupplierRecord record) {
  auto &slot = chunks[suppkey >> kQ15DirChunkShift];
  if (!slot)
    slot = std::make_shared<TpchQ15DirChunk>();
  else if (slot.use_count() > 1)
    slot = std::make_shared<TpchQ15DirChunk>(*slot);
  auto &items = slot->items;
  auto rec = std::make_shared<const TpchQ15SupplierRecord>(std::move(record));
  const auto pos = std::lower_bound(
      items.begin(), items.end(), suppkey,
      [](const auto &e, int64_t k) { return e.first < k; });
  if (pos != items.end() && pos->first == suppkey)
    pos->second = std::move(rec);
  else
    items.insert(pos, {suppkey, std::move(rec)});
}

void TpchQ15SupplierDirectory::Erase(int64_t suppkey) {
  const auto it = chunks.find(suppkey >> kQ15DirChunkShift);
  if (it == chunks.end() || !Find(suppkey))
    return;
  if (it->second.use_count() > 1)
    it->second = std::make_shared<TpchQ15DirChunk>(*it->second);
  auto &items = it->second->items;
  const auto pos = std::lower_bound(
      items.begin(), items.end(), suppkey,
      [](const auto &e, int64_t k) { return e.first < k; });
  items.erase(pos);
  if (items.empty())
    chunks.erase(it);
}

std::size_t TpchQ15SupplierDirectory::Size() const {
  std::size_t n = 0;
  for (const auto &[cid, chunk] : chunks)
    n += chunk->items.size();
  return n;
}

void TpchQ15InitialScan::ScanMonthRevenue(
    const std::function<void(int32_t, int64_t, int64_t)> &consumer) {
  std::map<std::pair<int32_t, int64_t>, int64_t> groups;
  ScanLineitems([&](const TpchQ15LineitemInput &input) {
    groups[{MonthKey(input.shipdate_days), input.suppkey}] += TpchQ15RevenueRaw(
        input.extended_price_raw, input.extended_price_scale,
        input.discount_raw, input.discount_scale);
  });
  for (const auto &[key, rev] : groups)
    consumer(key.first, key.second, rev);
}

// ── Helpers ───────────────────────────────────────────────────────────────

int32_t TpchQ15MonthKeyFromDays(int32_t days_since_epoch) {
  return MonthKey(days_since_epoch);
}

int32_t TpchQ15DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return DaysFromCivil(year, month, day);
}

bool TpchQ15IsFirstOfMonth(int32_t days_since_epoch) {
  return IsFirstOfMonth(days_since_epoch);
}

int32_t TpchQ15AddMonths(int32_t days_since_epoch, int32_t months) {
  return AddMonthsDays(days_since_epoch, months);
}

std::vector<int32_t> TpchQ15WindowMonths(int32_t, int32_t) {
  // Not used; retained for API completeness.
  return {};
}

int64_t TpchQ15RevenueRaw(int64_t extended_price_raw, int32_t,
                          int64_t discount_raw, int32_t) {
  return extended_price_raw * (100 - discount_raw);
}

// ── Encode / Decode ───────────────────────────────────────────────────────

std::vector<uint8_t> EncodeTpchQ15Directory(
    const TpchQ15SupplierDirectory &dir) {
  ByteWriter writer;
  writer.U32(kVersion);
  writer.U32(kDirMagic);
  writer.I64(static_cast<int64_t>(dir.Size()));
  for (const auto &[cid, chunk] : dir.chunks) {
    for (const auto &[key, record] : chunk->items) {
      writer.I64(key);
      writer.String(record->name);
      writer.String(record->address);
      writer.String(record->phone);
    }
  }
  return writer.bytes;
}

bool DecodeTpchQ15Directory(const std::vector<uint8_t> &bytes,
                             TpchQ15SupplierDirectory &dir) {
  ByteReader reader{bytes};
  uint32_t version = 0, magic = 0;
  if (!reader.U32(version) || !reader.U32(magic) ||
      version != kVersion || magic != kDirMagic)
    return false;
  int64_t count = 0;
  if (!reader.I64(count) || count < 0)
    return false;
  dir.chunks.clear();
  for (int64_t i = 0; i < count; ++i) {
    int64_t key = 0;
    if (!reader.I64(key))
      return false;
    TpchQ15SupplierRecord rec;
    if (!reader.String(rec.name))
      return false;
    if (!reader.String(rec.address))
      return false;
    if (!reader.String(rec.phone))
      return false;
    dir.Set(key, std::move(rec));
  }
  return true;
}

std::vector<uint8_t> EncodeTpchQ15Window(int32_t month_key,
                                          const TpchQ15WindowState &win) {
  ByteWriter writer;
  writer.U32(kVersion);
  writer.U32(kWindowMagic);
  writer.I32(month_key);
  writer.U32(static_cast<uint32_t>(win.winners.empty() ? 0 : 1)); // has_winners
  writer.I64(win.nonzero);
  for (const auto &[cid, chunk] : win.rev.chunks) {
    for (int64_t i = 0; i < kQ15RevChunkSize; ++i) {
      if (chunk->v[i] == 0)
        continue;
      writer.I64((cid << kQ15RevChunkShift) | i);
      writer.I64(chunk->v[i]);
    }
  }
  writer.I64(win.max_revenue);
  writer.I64(static_cast<int64_t>(win.winners.size()));
  for (const auto sk : win.winners)
    writer.I64(sk);
  return writer.bytes;
}

bool DecodeTpchQ15Window(const std::vector<uint8_t> &bytes, int32_t month_key,
                          TpchQ15WindowState &win) {
  ByteReader reader{bytes};
  uint32_t version = 0, magic = 0;
  if (!reader.U32(version) || !reader.U32(magic) ||
      version != kVersion || magic != kWindowMagic)
    return false;
  int32_t stored_month = 0;
  if (!reader.I32(stored_month) || stored_month != month_key)
    return false;
  uint32_t has_winners = 0;
  reader.U32(has_winners);
  int64_t count = 0;
  if (!reader.I64(count) || count < 0)
    return false;
  win = TpchQ15WindowState{};
  for (int64_t i = 0; i < count; ++i) {
    int64_t key = 0, rev = 0;
    if (!reader.I64(key) || !reader.I64(rev))
      return false;
    win.rev.Add(key, rev);
  }
  win.Rescan();
  int64_t max_rev = 0, winner_count = 0;
  if (!reader.I64(max_rev) || !reader.I64(winner_count) || winner_count < 0)
    return false;
  for (int64_t i = 0; i < winner_count; ++i) {
    int64_t sk = 0;
    if (!reader.I64(sk))
      return false;
  }
  return true;
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
  supplier_deltas.push_back(delta);
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

void TpchQ15Module::Build(core::AuxEngineServices &services,
                           core::StateBuilder &builder) {
  auto built = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ15State>(std::move(built));
  const auto &state = *typed_state;

  builder.Write(core::StateImage::Make(
      TpchQ15Layout::kAccelerator, TpchQ15Layout::kSupplierDirectory, 0,
      MakeDirImage(typed_state)));

  for (const auto &entry : state.windows) {
    builder.Write(core::StateImage::Make(
        TpchQ15Layout::kAccelerator,
        static_cast<core::PartitionId>(entry.first), 0,
        MakeWindowImage(entry.first, typed_state)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ15Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.supplier_table) {
    return All();
  }
  return {};
}

void TpchQ15Module::AccumulateMutation(const core::MutationBatch &mutation,
                                        core::AuxEngineServices &,
                                        core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ15TxnBuffer &>(base_buffer);

  if (mutation.table == config.lineitem_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ15LineitemInput input;
      if (!ReadLineitem(row, input)) {
        buffer.MarkRebuild();
        return;
      }
      const auto revenue =
          TpchQ15RevenueRaw(input.extended_price_raw,
                            input.extended_price_scale,
                            input.discount_raw, input.discount_scale);
      for (const auto month_key : CoveringWindowMonths(input.shipdate_days)) {
        TpchQ15LineitemDelta delta;
        delta.month_key = month_key;
        delta.suppkey = input.suppkey;
        delta.revenue_raw = revenue;
        delta.sign = sign;
        buffer.AddLineitemDelta(delta);
      }
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
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        TpchQ15SupplierInput input;
        if (ReadSupplier(row, input)) {
          TpchQ15SupplierDelta delta;
          delta.suppkey = input.suppkey;
          delta.new_name = input.name;
          delta.new_address = input.address;
          delta.new_phone = input.phone;
          buffer.AddSupplierDelta(delta);
        } else {
          buffer.MarkRebuild();
        }
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        TpchQ15SupplierInput input;
        if (ReadSupplier(row, input)) {
          TpchQ15SupplierDelta delta;
          delta.suppkey = input.suppkey;
          delta.old_name = input.name;
          delta.old_address = input.address;
          delta.old_phone = input.phone;
          buffer.AddSupplierDelta(delta);
        } else {
          buffer.MarkRebuild();
        }
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        TpchQ15SupplierInput old_input, new_input;
        if (!ReadSupplier(mutation.old_rows[i], old_input) ||
            !ReadSupplier(mutation.new_rows[i], new_input)) {
          buffer.MarkRebuild();
          return;
        }
        TpchQ15SupplierDelta delta;
        delta.suppkey = old_input.suppkey;
        delta.old_name = old_input.name;
        delta.old_address = old_input.address;
        delta.old_phone = old_input.phone;
        delta.new_name = new_input.name;
        delta.new_address = new_input.address;
        delta.new_phone = new_input.phone;
        buffer.AddSupplierDelta(delta);
      }
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
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (buffer.LineitemDeltas().empty() && buffer.SupplierDeltas().empty())
    return outcome;

  const auto visibility = services.CurrentVisibility();

  std::set<int32_t> touched_months;
  for (const auto &delta : buffer.LineitemDeltas())
    touched_months.insert(delta.month_key);

  const bool has_supplier_deltas = !buffer.SupplierDeltas().empty();
  const bool can_rebuild_windows =
      !touched_months.empty() && !has_supplier_deltas && initial_scan;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.LineitemDeltas().size() +
                             buffer.SupplierDeltas().size()),
       static_cast<uint64_t>(std::max<std::size_t>(1, touched_months.size())),
       true, can_rebuild_windows, true, has_supplier_deltas, false,
       core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});

  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    if (!can_rebuild_windows) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    int32_t scan_lo = std::numeric_limits<int32_t>::max();
    int32_t scan_hi = std::numeric_limits<int32_t>::min();
    for (const auto month_key : touched_months) {
      const int32_t year = month_key / 100;
      const unsigned month = static_cast<unsigned>(month_key % 100);
      const int32_t start = DaysFromCivil(year, month, 1);
      const int32_t end = AddMonthsDays(start, 3);
      scan_lo = std::min(scan_lo, start);
      scan_hi = std::max(scan_hi, end);
    }
    std::map<int32_t, TpchQ15WindowState> rebuilt;
    initial_scan->ScanLineitemsForShipdateRange(
        scan_lo, scan_hi, [&](const TpchQ15LineitemInput &input) {
          const auto revenue = TpchQ15RevenueRaw(
              input.extended_price_raw, input.extended_price_scale,
              input.discount_raw, input.discount_scale);
          for (const auto month_key :
               CoveringWindowMonths(input.shipdate_days)) {
            if (!touched_months.count(month_key))
              continue;
            rebuilt[month_key].rev.Add(input.suppkey, revenue);
          }
        });
    for (const auto month : touched_months) {
      TpchQ15WindowState ws;
      const auto it = rebuilt.find(month);
      if (it != rebuilt.end())
        ws = std::move(it->second);
      ws.Rescan();
      auto win_state = std::make_shared<const TpchQ15State>(
          TpchQ15State{TpchQ15SupplierDirectory{}, {{month, std::move(ws)}}});
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ15Layout::kAccelerator, static_cast<core::PartitionId>(month), 0,
          MakeWindowImage(month, win_state)));
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

  // Load affected windows
  std::map<int32_t, TpchQ15WindowState> windows;
  for (const auto month : touched_months) {
    TpchQ15WindowState ws;
    if (!LoadMonth(services, month, ws)) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    windows[month] = std::move(ws);
  }

  // Apply lineitem deltas to windows, maintaining the max/winner set
  // incrementally; a full pass over the dense array is needed only when the
  // sole current winner of a window decreases.
  std::set<int32_t> rescan;
  for (const auto &delta : buffer.LineitemDeltas()) {
    auto it = windows.find(delta.month_key);
    if (it == windows.end())
      continue;
    auto &ws = it->second;
    const int64_t signed_rev =
        static_cast<int64_t>(delta.sign) * delta.revenue_raw;
    if (signed_rev == 0)
      continue;
    const int64_t old_rev = ws.rev.Add(delta.suppkey, signed_rev);
    const int64_t new_rev = old_rev + signed_rev;
    if (old_rev == 0)
      ++ws.nonzero;
    if (new_rev == 0)
      --ws.nonzero;
    ws.empty = ws.nonzero == 0;
    if (rescan.count(delta.month_key))
      continue;
    const bool was_winner =
        old_rev != 0 && !ws.winners.empty() && old_rev == ws.max_revenue;
    const auto pos = std::lower_bound(ws.winners.begin(), ws.winners.end(),
                                      delta.suppkey);
    if (was_winner && new_rev < old_rev) {
      if (ws.winners.size() > 1)
        ws.winners.erase(pos); // max unchanged: another supplier holds it
      else
        rescan.insert(delta.month_key);
    } else if (new_rev != 0 &&
               (ws.winners.empty() || new_rev > ws.max_revenue)) {
      ws.max_revenue = new_rev;
      ws.winners.assign(1, delta.suppkey);
    } else if (new_rev != 0 && new_rev == ws.max_revenue && !was_winner) {
      ws.winners.insert(pos, delta.suppkey);
    }
  }
  for (auto &[month, ws] : windows) {
    if (rescan.count(month) || ws.nonzero == 0)
      ws.Rescan();
  }

  // Apply supplier deltas to a directory copy (chunk-level copy on write).
  TpchQ15SupplierDirectory dir_copy;
  if (has_supplier_deltas) {
    if (!LoadDirectory(services, dir_copy)) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    for (const auto &delta : buffer.SupplierDeltas()) {
      if (!delta.old_name.empty() || !delta.old_address.empty() ||
          !delta.old_phone.empty()) {
        dir_copy.Erase(delta.suppkey);
      }
      if (!delta.new_name.empty() || !delta.new_address.empty() ||
          !delta.new_phone.empty()) {
        TpchQ15SupplierRecord rec;
        rec.name = delta.new_name;
        rec.address = delta.new_address;
        rec.phone = delta.new_phone;
        dir_copy.Set(delta.suppkey, std::move(rec));
      }
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;

  if (has_supplier_deltas) {
    auto dir_state = std::make_shared<const TpchQ15State>(
        TpchQ15State{std::move(dir_copy), {}});
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ15Layout::kAccelerator, TpchQ15Layout::kSupplierDirectory, 0,
        MakeDirImage(dir_state)));
  }

  for (auto &[month, ws] : windows) {
    const auto partition = static_cast<core::PartitionId>(month);
    auto win_state = std::make_shared<const TpchQ15State>(
        TpchQ15State{TpchQ15SupplierDirectory{}, {{month, std::move(ws)}}});
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ15Layout::kAccelerator, partition, 0,
        MakeWindowImage(month, win_state)));
  }

  return outcome;
}

bool TpchQ15Module::CheckValidity(core::AuxEngineServices &services,
                                   const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ15Module::Execute(core::AuxEngineServices &services,
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

  if (!TpchQ15IsFirstOfMonth(start)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto month_key = TpchQ15MonthKeyFromDays(start);

  // Direct state access: reference the shared state; only winners +
  // max_revenue are needed (no revenue array access).
  // The window and the supplier directory from one snapshot.
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {static_cast<core::PartitionId>(month_key), TpchQ15Layout::kSupplierDirectory});
  const TpchQ15WindowState *win = nullptr;
  const TpchQ15SupplierDirectory *dir = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ15Layout::kSupplierDirectory) {
      if (const auto *typed = AsQ15Image(image, TpchQ15ImageKind::SupplierDirectory))
        dir = &typed->state->directory;
      continue;
    }
    if (static_cast<int32_t>(image.partition) != month_key)
      continue;
    const auto *typed = AsQ15Image(image, TpchQ15ImageKind::MonthWindow);
    if (!typed || typed->month_key != month_key)
      continue;
    const auto it = typed->state->windows.find(month_key);
    if (it != typed->state->windows.end())
      win = &it->second;
  }
  if (!win || !dir) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (win->empty || win->winners.empty()) {
    sink.Begin({{{"s_suppkey", core::AuxType::Int64, true},
                 {"s_name", core::AuxType::String, true},
                 {"s_address", core::AuxType::String, true},
                 {"s_phone", core::AuxType::String, true},
                 {"total_revenue", core::AuxType::ScaledDecimal, true}}});
    sink.End();
    return;
  }

  // Join the winners with the directory as of this snapshot (a supplier no
  // longer there drops out; renamed ones show their current record).
  std::vector<std::pair<int64_t, TpchQ15SupplierRecord>> winners;
  winners.reserve(win->winners.size());
  for (const auto sk : win->winners) {
    if (const auto *rec = dir->Find(sk))
      winners.emplace_back(sk, *rec);
  }
  EmitResult(winners, win->max_revenue, sink);
}


void TpchQ15Module::ExecuteFallback(core::AuxEngineServices &services,
                                     const core::QueryArguments &arguments,
                                     core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start = std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  }

  if (!initial_scan)
    throw std::logic_error("TPC-H Q15 requires a typed initial scan source");

  const int32_t end = TpchQ15AddMonths(start, 3);

  std::unordered_map<int64_t, int64_t> total_rev;
  TpchQ15SupplierDirectory dir;

  initial_scan->ScanSuppliers([&](const TpchQ15SupplierInput &input) {
    dir.Set(input.suppkey,
            TpchQ15SupplierRecord{input.name, input.address, input.phone});
  });

  initial_scan->ScanLineitems([&](const TpchQ15LineitemInput &input) {
    if (input.shipdate_days < start || input.shipdate_days >= end)
      return;
    const auto revenue = TpchQ15RevenueRaw(
        input.extended_price_raw, input.extended_price_scale,
        input.discount_raw, input.discount_scale);
    total_rev[input.suppkey] += revenue;
  });

  (void)services;

  if (total_rev.empty()) {
    sink.Begin({{{"s_suppkey", core::AuxType::Int64, true},
                 {"s_name", core::AuxType::String, true},
                 {"s_address", core::AuxType::String, true},
                 {"s_phone", core::AuxType::String, true},
                 {"total_revenue", core::AuxType::ScaledDecimal, true}}});
    sink.End();
    return;
  }

  int64_t max_rev = std::numeric_limits<int64_t>::min();
  for (const auto &[sk, rev] : total_rev)
    if (rev > max_rev) max_rev = rev;

  std::vector<std::pair<int64_t, TpchQ15SupplierRecord>> winners;
  for (const auto &[sk, rev] : total_rev) {
    if (rev == max_rev) {
      if (const auto *rec = dir.Find(sk))
        winners.emplace_back(sk, *rec);
    }
  }
  std::sort(winners.begin(), winners.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  EmitResult(winners, max_rev, sink);
}

TpchQ15State TpchQ15Module::BuildState(
    core::AuxEngineServices &services) const {
  (void)services;
  TpchQ15State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q15 requires a typed initial scan source");

  initial_scan->ScanSuppliers([&](const TpchQ15SupplierInput &input) {
    state.directory.Set(
        input.suppkey,
        TpchQ15SupplierRecord{input.name, input.address, input.phone});
  });

  // One (month, supplier) group per row; each feeds the three rolling windows
  // that contain the month.
  initial_scan->ScanMonthRevenue(
      [&](int32_t month_key, int64_t suppkey, int64_t revenue) {
        int32_t keys[3];
        CoveringWindowKeysOfMonth(month_key, keys);
        for (const auto key : keys)
          state.windows[key].rev.Add(suppkey, revenue);
      });
  for (auto &[month_key, ws] : state.windows)
    ws.Rescan();
  return state;
}

void TpchQ15Module::EmitResult(
    const std::vector<std::pair<int64_t, TpchQ15SupplierRecord>> &rows,
    int64_t revenue_raw, core::AuxRowSink &sink) const {
  constexpr int32_t kScale = 4;
  sink.Begin({{{"s_suppkey", core::AuxType::Int64, true},
               {"s_name", core::AuxType::String, true},
               {"s_address", core::AuxType::String, true},
               {"s_phone", core::AuxType::String, true},
               {"total_revenue", core::AuxType::ScaledDecimal, true}}});
  for (const auto &[suppkey, record] : rows) {
    core::ScaledDecimal revenue{revenue_raw, kScale};
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, suppkey),
                    core::AuxValue::Of(core::AuxType::String, record.name),
                    core::AuxValue::Of(core::AuxType::String, record.address),
                    core::AuxValue::Of(core::AuxType::String, record.phone),
                    core::AuxValue::Of(core::AuxType::ScaledDecimal,
                                       revenue)}});
  }
  sink.End();
}

bool TpchQ15Module::LoadMonth(core::AuxEngineServices &services,
                               int32_t month_key,
                               TpchQ15WindowState &win) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {static_cast<core::PartitionId>(month_key)});
  for (const auto &image : snapshot.partitions) {
    if (static_cast<int32_t>(image.partition) != month_key)
      continue;
    const auto *typed = AsQ15Image(image, TpchQ15ImageKind::MonthWindow);
    if (!typed || typed->month_key != month_key)
      return false;
    const auto it = typed->state->windows.find(month_key);
    if (it != typed->state->windows.end())
      win = it->second; // shares the array chunks; they are cloned on write
    return true;
  }
  // No image yet: a month without data has no window partition (Build writes
  // only non-empty windows), so it starts empty.
  win = TpchQ15WindowState{};
  return true;
}

bool TpchQ15Module::LoadDirectory(core::AuxEngineServices &services,
                                   TpchQ15SupplierDirectory &dir) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ15Layout::kSupplierDirectory});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ15Layout::kSupplierDirectory)
      continue;
    const auto *typed = AsQ15Image(image, TpchQ15ImageKind::SupplierDirectory);
    if (!typed)
      return false;
    dir = typed->state->directory; // shares chunks (COW)
    return true;
  }
  return false;
}

std::vector<core::CalibrationWrite> TpchQ15Module::CalibrationWrites() const {
  return TpchQ15CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
