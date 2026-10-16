#include "tpch/q10/duckdb/aux/module.hpp"
#include "tpch/q10/calibration_writes.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include "tpch/q10/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr int32_t kEpoch = 719468;

// Lineitem deltas per slice when resolving them against the orders map.
constexpr std::size_t kResolveGrain = 4096;
// Month delta entries below which the per-month / per-window images are built inline.
constexpr std::size_t kImageWorkGrain = 2048;

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

int32_t MonthStart(int32_t month_key) {
  return DaysFromCivil(month_key / 100, static_cast<unsigned>(month_key % 100),
                       1);
}

std::shared_ptr<const TpchQ10StateImage>
MakeStateImage(TpchQ10ImageKind kind, int32_t month,
               const std::shared_ptr<const TpchQ10State> &state) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = kind;
  image->month = month;
  image->state = state;
  return image;
}

const TpchQ10StateImage *AsQ10Image(const core::StateImage &image,
                                    TpchQ10ImageKind kind) {
  const auto *payload = image.As<TpchQ10StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  if ((kind == TpchQ10ImageKind::Side || kind == TpchQ10ImageKind::Orders) &&
      !payload->state)
    return nullptr;
  return payload;
}

// Month ordinal arithmetic on YYYYMM keys.
int32_t MonthOrd(int32_t month_key) {
  return (month_key / 100) * 12 + (month_key % 100) - 1;
}
int32_t MonthFromOrd(int32_t ord) { return (ord / 12) * 100 + ord % 12 + 1; }

constexpr uint32_t kMetaPartition = 4;
constexpr uint32_t kFirstWindow = 3000000; // + YYYYMM of the window start

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool IsReturned(const core::AuxRow &row) {
  std::string flag;
  return StringCell(row, 3, flag) && flag == "R";
}

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &revenue_raw, bool &returned) {
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  if (!IntCell(row, 0, orderkey) ||
      !RawDecimalCell(row, 1, extended_raw, extended_scale) ||
      !RawDecimalCell(row, 2, discount_raw, discount_scale))
    return false;
  if (extended_scale == 2 && discount_scale == 2) {
    revenue_raw = extended_raw * (100 - discount_raw);
  } else {
    double price = 0.0, discount = 0.0;
    if (!DoubleCell(row, 1, price) || !DoubleCell(row, 2, discount))
      return false;
    revenue_raw = TpchQ10RevenueRaw(price, discount);
  }
  returned = IsReturned(row);
  return true;
}

// Matches ScanOrders: o_orderkey, o_custkey, o_orderdate
bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, int64_t &custkey,
               int32_t &orderdate_days) {
  return IntCell(row, 0, orderkey) && IntCell(row, 1, custkey) &&
         DateCell(row, 2, orderdate_days);
}

uint32_t MonthPartition(int32_t month) {
  return static_cast<uint32_t>(core::TpchQ10Layout::kFirstMonth +
                               static_cast<uint64_t>(month));
}

void CollectCustomerRankKeys(const TpchQ10SideBase &base,
                             std::vector<int64_t> &keys) {
  keys.clear();
  for (std::size_t chunk_idx = 0; chunk_idx < base.ranks.chunks.size();
       ++chunk_idx) {
    const auto &chunk = base.ranks.chunks[chunk_idx];
    const auto key_base = static_cast<int64_t>(
        chunk_idx << core::DenseIdArray<TpchQ10CustomerRank>::kShift);
    for (std::size_t slot = 0; slot < chunk.size(); ++slot) {
      if (chunk[slot].present == 0)
        continue;
      keys.push_back(key_base + static_cast<int64_t>(slot));
    }
  }
  std::sort(keys.begin(), keys.end());
}

// Fold the overlay into a fresh base (deep copy; only on compaction/encode).
std::shared_ptr<const TpchQ10SideBase>
CompactSideBase(const TpchQ10State &state) {
  auto base = std::make_shared<TpchQ10SideBase>(*state.side_base);
  if (state.customer_overlay) {
    for (const auto &[key, slot] : *state.customer_overlay) {
      if (slot.present) {
        base->SetCustomerRank(key, slot.customer.acctbal_raw,
                              slot.customer.nationkey);
        base->customers[key] = slot.customer;
      } else {
        if (base->ranks.Get(key).present != 0)
          base->ranks.Set(key, TpchQ10CustomerRank{});
        base->customers.erase(key);
      }
    }
  }
  return base;
}

constexpr std::size_t kCustomerOverlayCap = 2048;
constexpr std::size_t kMonthOverlayCap = 2048;

std::shared_ptr<const TpchQ10State>
ApplyCustomerDeltas(const TpchQ10State &prev,
                    const std::vector<TpchQ10CustomerDelta> &deltas) {
  auto overlay = prev.customer_overlay
                     ? std::make_shared<TpchQ10CustomerOverlay>(
                           *prev.customer_overlay)
                     : std::make_shared<TpchQ10CustomerOverlay>();
  for (const auto &delta : deltas) {
    if (!core::DenseIdArray<TpchQ10CustomerRank>::Fits(delta.custkey))
      return nullptr;
    auto &slot = (*overlay)[delta.custkey];
    if (delta.sign < 0) {
      slot.present = 0;
      slot.customer = TpchQ10Customer{};
    } else {
      slot.present = 1;
      slot.customer = delta.customer;
    }
  }
  auto next = std::make_shared<TpchQ10State>();
  next->nations = prev.nations;
  if (overlay->size() > kCustomerOverlayCap) {
    TpchQ10State folded;
    folded.side_base = prev.side_base;
    folded.customer_overlay = std::move(overlay);
    next->side_base = CompactSideBase(folded);
  } else {
    next->side_base = prev.side_base;
    next->customer_overlay = std::move(overlay);
  }
  return next;
}

} // namespace


namespace {

uint32_t WindowPartition(int32_t start_month) {
  return kFirstWindow + static_cast<uint32_t>(start_month);
}

// Sum of the key-sorted month tables `parts` (nonzero sums only), key-sorted.
void SumMonths(const std::vector<const std::vector<TpchQ10KV> *> &parts,
               std::vector<TpchQ10KV> &out) {
  out.clear();
  std::vector<std::size_t> pos(parts.size(), 0);
  for (;;) {
    int64_t best = std::numeric_limits<int64_t>::max();
    bool any = false;
    for (std::size_t i = 0; i < parts.size(); ++i)
      if (parts[i] && pos[i] < parts[i]->size()) {
        best = std::min(best, (*parts[i])[pos[i]].key);
        any = true;
      }
    if (!any)
      break;
    int64_t sum = 0;
    for (std::size_t i = 0; i < parts.size(); ++i)
      if (parts[i] && pos[i] < parts[i]->size() &&
          (*parts[i])[pos[i]].key == best) {
        sum += (*parts[i])[pos[i]].val;
        ++pos[i];
      }
    if (sum != 0)
      out.push_back({best, sum});
  }
}

std::shared_ptr<const TpchQ10StateImage> MakeMonthImage(int32_t month,
                                                        TpchQ10KeyIndex data) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = TpchQ10ImageKind::Month;
  image->month = month;
  image->by_key = std::move(data);
  return image;
}

std::shared_ptr<const TpchQ10StateImage> MakeWindowImage(int32_t start,
                                                         TpchQ10RankIndex data) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = TpchQ10ImageKind::Window;
  image->month = start;
  image->ranked = std::move(data);
  return image;
}

std::shared_ptr<const TpchQ10StateImage> MakeMetaImage(int32_t lo, int32_t hi) {
  auto image = std::make_shared<TpchQ10StateImage>();
  image->kind = TpchQ10ImageKind::Meta;
  image->lo_month = lo;
  image->hi_month = hi;
  return image;
}

TpchQ10RankIndex RankedFromSums(std::vector<TpchQ10KV> &&sums) {
  std::sort(sums.begin(), sums.end(), TpchQ10ByRank{});
  return TpchQ10RankIndex::FromSorted(std::move(sums));
}

} // namespace

std::vector<std::pair<int64_t, int64_t>>
TpchQ10WindowTop(const TpchQ10State &side, const TpchQ10RankIndex &window,
                 std::size_t limit) {
  std::vector<std::pair<int64_t, int64_t>> out;
  out.reserve(limit);
  for (const auto &chunk : window.chunks) {
    for (const auto &e : chunk->e) {
      if (e.val <= 0)
        return out;
      if (!side.HasCustomer(e.key))
        continue;
      out.emplace_back(e.key, e.val);
      if (out.size() >= limit)
        return out;
    }
  }
  return out;
}

void TpchQ10EmitRows(const TpchQ10State &side,
                     const std::vector<std::pair<int64_t, int64_t>> &ranked,
                     core::AuxRowSink &sink) {
  sink.Begin({{{"c_custkey", core::AuxType::Int64, false},
               {"c_name", core::AuxType::String, false},
               {"revenue", core::AuxType::ScaledDecimal, false},
               {"c_acctbal", core::AuxType::ScaledDecimal, false},
               {"n_name", core::AuxType::String, false},
               {"c_address", core::AuxType::String, false},
               {"c_phone", core::AuxType::String, false},
               {"c_comment", core::AuxType::String, false}}});
  const std::string empty;
  for (const auto &[custkey, revenue] : ranked) {
    const auto rank = side.RankOf(custkey);
    const auto *strings = side.CustomerOf(custkey);
    const auto nation = side.nations.find(rank.nationkey);
    const auto &name = strings ? strings->name : empty;
    const auto &address = strings ? strings->address : empty;
    const auto &phone = strings ? strings->phone : empty;
    const auto &comment = strings ? strings->comment : empty;
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, custkey),
          core::AuxValue::Of(core::AuxType::String, name),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{revenue, 4}),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{rank.acctbal_raw, 2}),
          core::AuxValue::Of(core::AuxType::String,
                             nation == side.nations.end() ? empty
                                                          : nation->second),
          core::AuxValue::Of(core::AuxType::String, address),
          core::AuxValue::Of(core::AuxType::String, phone),
          core::AuxValue::Of(core::AuxType::String, comment)}});
  }
  sink.End();
}

int32_t TpchQ10MonthKeyFromDays(int32_t days_since_epoch) {
  return MonthKey(days_since_epoch);
}
int32_t TpchQ10MonthStartDays(int32_t month_key) {
  return MonthStart(month_key);
}
int32_t TpchQ10AddMonths(int32_t days_since_epoch, int32_t months) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  const int total = year * 12 + static_cast<int>(month) - 1 + months;
  const int new_year = total / 12;
  const unsigned new_month = static_cast<unsigned>(total % 12 + 1);
  return DaysFromCivil(new_year, new_month, day);
}

int64_t TpchQ10RevenueRaw(double extended_price, double discount) {
  return static_cast<int64_t>(
      std::llround(extended_price * (1.0 - discount) * 10000.0));
}

bool TpchQ10TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ10TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
  customer_deltas.clear();
}
bool TpchQ10TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ10LineitemDelta> &TpchQ10TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ10OrderDelta> &TpchQ10TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
const std::vector<TpchQ10CustomerDelta> &
TpchQ10TxnBuffer::CustomerDeltas() const {
  return customer_deltas;
}
void TpchQ10TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
  customer_deltas.clear();
}
void TpchQ10TxnBuffer::AddCustomerDelta(TpchQ10CustomerDelta delta) {
  pending = true;
  customer_deltas.push_back(std::move(delta));
}
void TpchQ10TxnBuffer::AddDelta(TpchQ10LineitemDelta delta) {
  pending = true;
  if (delta.revenue_raw != 0)
    deltas.push_back(delta);
}
void TpchQ10TxnBuffer::AddOrderDelta(TpchQ10OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}

core::AcceleratorId TpchQ10Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ10Module::Dependencies() const {
  return {{config.customer_table, {0, 1, 2, 3, 4, 5, 6}, {0, 1, 5, 3, 2, 4, 7}},
          {config.nation_table, {0, 1}, {0, 1}},
          {config.orders_table, {0, 1, 2}, {0, 1, 4}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 5, 6, 8}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ10Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ10TxnBuffer>();
}
void TpchQ10Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto data = BuildState(services);

  // Publish lean per-kind images so Side/Month payloads do not retain the
  // full orders map (orders are write-maintenance only).
  auto side_state = std::make_shared<TpchQ10State>();
  side_state->side_base = std::move(data.state.side_base);
  side_state->nations = std::move(data.state.nations);
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator,
      core::TpchQ10Layout::kCustomerDimensions, 0,
      MakeStateImage(TpchQ10ImageKind::Side, 0,
                     std::shared_ptr<const TpchQ10State>(side_state))));

  // Orders partition is optional: SQL-pushdown builds skip it and resolve
  // orderkeys via LookupOrders during maintenance.
  if (data.state.orders.size() != 0) {
    auto orders_state = std::make_shared<TpchQ10State>();
    orders_state->orders = std::move(data.state.orders);
    builder.Write(core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator,
        core::TpchQ10Layout::kOrderMetadata, 0,
        MakeStateImage(TpchQ10ImageKind::Orders, 0,
                       std::shared_ptr<const TpchQ10State>(orders_state))));
  }

  // Month range: every ordinal in [lo, hi] gets a month image (possibly
  // empty) so a missing month image inside the range never happens.
  int32_t lo_ord = 0, hi_ord = -1;
  if (!data.months.empty()) {
    lo_ord = MonthOrd(data.months.begin()->first);
    hi_ord = MonthOrd(data.months.rbegin()->first);
  }
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator, kMetaPartition, 0,
      hi_ord < lo_ord ? MakeMetaImage(0, -1)
                      : MakeMetaImage(MonthFromOrd(lo_ord), MonthFromOrd(hi_ord))));
  if (hi_ord >= lo_ord) {
    // Flat key-sorted tables for every ordinal (null = empty month).
    const int32_t span = hi_ord - lo_ord + 1;
    std::vector<const std::vector<TpchQ10KV> *> tables(
        static_cast<std::size_t>(span), nullptr);
    for (const auto &[month, vec] : data.months)
      tables[static_cast<std::size_t>(MonthOrd(month) - lo_ord)] = &vec;
    // Windows start at ordinals [lo-2, hi]: every window containing a month
    // with data. Windows and month images are independent: built in parallel,
    // written in ordinal order.
    std::vector<std::shared_ptr<const TpchQ10StateImage>> windows(
        static_cast<std::size_t>(span + 2));
    std::vector<std::shared_ptr<const TpchQ10StateImage>> month_images(
        static_cast<std::size_t>(span));
    services.ParallelFor(windows.size(), [&](std::size_t i) {
      const int32_t w_ord = lo_ord - 2 + static_cast<int32_t>(i);
      std::vector<const std::vector<TpchQ10KV> *> parts;
      for (int32_t j = 0; j < 3; ++j) {
        const int32_t o = w_ord + j;
        parts.push_back(o >= lo_ord && o <= hi_ord
                            ? tables[static_cast<std::size_t>(o - lo_ord)]
                            : nullptr);
      }
      std::vector<TpchQ10KV> sums;
      SumMonths(parts, sums);
      windows[i] =
          MakeWindowImage(MonthFromOrd(w_ord), RankedFromSums(std::move(sums)));
    });
    services.ParallelFor(month_images.size(), [&](std::size_t i) {
      const auto *table = tables[i];
      std::vector<TpchQ10KV> copy;
      if (table)
        copy = *table;
      month_images[i] = MakeMonthImage(
          MonthFromOrd(lo_ord + static_cast<int32_t>(i)),
          TpchQ10KeyIndex::FromSorted(std::move(copy)));
    });
    for (int32_t o = lo_ord; o <= hi_ord; ++o)
      builder.Write(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(MonthFromOrd(o)), 0,
          std::move(month_images[static_cast<std::size_t>(o - lo_ord)])));
    {
      std::size_t month_entries = 0, window_entries = 0;
      for (const auto &[m, vec] : data.months)
        month_entries += vec.size();
      for (const auto &win : windows)
        if (win)
          window_entries += win->ranked.count;
      std::cerr << "tpch_q10_aux_build: " << (span + 2) << " windows, "
                << window_entries << " window entries, " << month_entries
                << " month entries\n"
                << std::flush;
    }
    for (int32_t w = lo_ord - 2; w <= hi_ord; ++w) {
      auto &win = windows[static_cast<std::size_t>(w - (lo_ord - 2))];
      builder.Write(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, WindowPartition(MonthFromOrd(w)), 0,
          std::move(win)));
    }
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ10Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.customer_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.nation_table)
    return All();
  return {};
}

void TpchQ10Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
  if (mutation.table == config.nation_table) {
    buffer.MarkRebuild();
    return;
  }
  if (mutation.table == config.customer_table) {
    // Row layout: custkey, name, acctbal, nationkey, address, phone, comment.
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ10CustomerDelta delta;
      delta.sign = sign;
      int64_t key = 0, acct = 0, nation = 0;
      int32_t scale = 0;
      if (!IntCell(row, 0, key) || !StringCell(row, 1, delta.customer.name) ||
          !RawDecimalCell(row, 2, acct, scale) || scale != 2 ||
          !IntCell(row, 3, nation) ||
          nation < std::numeric_limits<int32_t>::min() ||
          nation > std::numeric_limits<int32_t>::max() ||
          !StringCell(row, 4, delta.customer.address) ||
          !StringCell(row, 5, delta.customer.phone) ||
          !StringCell(row, 6, delta.customer.comment)) {
        buffer.MarkRebuild();
        return;
      }
      delta.custkey = key;
      delta.customer.acctbal_raw = acct;
      delta.customer.nationkey = static_cast<int32_t>(nation);
      buffer.AddCustomerDelta(std::move(delta));
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
    return;
  }
  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0, custkey = 0;
      int32_t orderdate = 0;
      if (!ReadOrder(row, orderkey, custkey, orderdate)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta({orderkey, custkey, orderdate, sign});
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
    return;
  }
  if (mutation.table != config.lineitem_table)
    return;
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0, revenue = 0;
    bool returned = false;
    if (!ReadLineitem(row, orderkey, revenue, returned)) {
      buffer.MarkRebuild();
      return;
    }
    if (returned)
      buffer.AddDelta({orderkey, revenue, sign});
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
TpchQ10Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
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
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty() &&
      buffer.CustomerDeltas().empty())
    return outcome;

  const auto rebuild_all = [&]() {
    core::FinishRebuildAll(outcome);
    return outcome;
  };
  const auto visibility = services.CurrentVisibility();

  // Customer writes touch only the Side partition (revenue is keyed by custkey
  // and never joins customer): copy-on-write overlay, compacted when large.
  std::optional<core::StateImage> side_update;
  if (!buffer.CustomerDeltas().empty()) {
    const auto side_snapshot = services.StateStore().ReadPartitions(
        visibility, {core::TpchQ10Layout::kCustomerDimensions});
    const TpchQ10StateImage *typed = nullptr;
    core::Version side_version = 0;
    for (const auto &image : side_snapshot.partitions) {
      if (image.partition == core::TpchQ10Layout::kCustomerDimensions) {
        typed = AsQ10Image(image, TpchQ10ImageKind::Side);
        side_version = image.version;
        break;
      }
    }
    if (!typed)
      return rebuild_all();
    const auto next = ApplyCustomerDeltas(*typed->state, buffer.CustomerDeltas());
    if (!next)
      return rebuild_all();
    side_update = core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator,
        core::TpchQ10Layout::kCustomerDimensions, side_version,
        MakeStateImage(TpchQ10ImageKind::Side, 0, next));
  }
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty()) {
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(std::move(*side_update));
    return outcome;
  }

  // Order changes. An order whose (custkey, orderdate) changes moves all its
  // lineitems' revenue, which lineitem deltas cannot express: rebuild. A pure
  // delete is a delta only when the lineitem deltas cancel its revenue, i.e.
  // no returned lineitem of that order remains in the base table (RF2).
  {
    struct Change {
      bool has_old = false, has_new = false;
      TpchQ10Order old_order, new_order;
    };
    std::unordered_map<int64_t, Change> changes;
    for (const auto &delta : buffer.OrderDeltas()) {
      auto &c = changes[delta.orderkey];
      const TpchQ10Order order{delta.custkey, delta.orderdate_days};
      if (delta.sign < 0) {
        c.has_old = true;
        c.old_order = order;
      } else {
        c.has_new = true;
        c.new_order = order;
      }
    }
    std::vector<int64_t> deleted_only;
    for (const auto &[key, c] : changes) {
      if (c.has_old && c.has_new) {
        if (c.old_order.custkey != c.new_order.custkey ||
            c.old_order.orderdate_days != c.new_order.orderdate_days)
          return rebuild_all();
      } else if (c.has_old) {
        deleted_only.push_back(key);
      }
    }
    if (!deleted_only.empty()) {
      std::set<int64_t> remaining;
      if (!initial_scan ||
          !initial_scan->OrdersWithReturnedLineitems(deleted_only, remaining) ||
          !remaining.empty())
        return rebuild_all();
    }
  }

  const auto total_deltas =
      buffer.Deltas().size() + buffer.OrderDeltas().size();

  TpchQ10State state;
  const auto order_snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ10Layout::kOrderMetadata});
  std::shared_ptr<TpchQ10State> order_metadata;
  core::Version order_version = 0;
  bool have_orders_partition = false;
  for (const auto &image : order_snapshot.partitions) {
    if (image.partition == core::TpchQ10Layout::kOrderMetadata) {
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Orders);
      if (!typed)
        return rebuild_all();
      order_metadata = std::make_shared<TpchQ10State>(*typed->state);
      order_version = image.version;
      have_orders_partition = true;
      break;
    }
  }

  bool orders_changed = false;
  // Only mutate an existing Orders partition. SQL-pushdown builds skip it and
  // resolve via LookupOrders; publishing a sparse RF-only map would break
  // later lookups for baseline orderkeys.
  if (!buffer.OrderDeltas().empty() && have_orders_partition && order_metadata) {
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign > 0) {
        order_metadata->orders.Upsert(
            delta.orderkey, TpchQ10Order{delta.custkey, delta.orderdate_days});
        orders_changed = true;
      }
    }
  }

  // When build skipped the Orders partition, resolve only the touched keys.
  // Same-txn RF1 inserts (and RF2 deletes, which carry their old values) are
  // taken from the order deltas themselves.
  std::unordered_map<int64_t, TpchQ10Order> looked_up_orders;
  std::unordered_map<int64_t, TpchQ10Order> overlay_orders;
  if (!(have_orders_partition && order_metadata) && !buffer.Deltas().empty()) {
    for (const auto &delta : buffer.OrderDeltas())
      overlay_orders[delta.orderkey] =
          TpchQ10Order{delta.custkey, delta.orderdate_days};
    std::vector<int64_t> orderkeys;
    orderkeys.reserve(buffer.Deltas().size());
    for (const auto &delta : buffer.Deltas()) {
      if (overlay_orders.find(delta.orderkey) == overlay_orders.end())
        orderkeys.push_back(delta.orderkey);
    }
    if (!orderkeys.empty()) {
      if (!initial_scan ||
          !initial_scan->LookupOrders(orderkeys, looked_up_orders))
        return rebuild_all();
    }
    for (const auto &[key, order] : looked_up_orders)
      overlay_orders.emplace(key, order);
  }
  const auto find_order = [&](int64_t orderkey) -> const TpchQ10Order * {
    if (have_orders_partition && order_metadata)
      return order_metadata->orders.Find(orderkey);
    const auto it = overlay_orders.find(orderkey);
    return it == overlay_orders.end() ? nullptr : &it->second;
  };

  std::set<int32_t> rebuild_months;
  auto add_month_from_days = [&](int32_t days) {
    rebuild_months.insert(TpchQ10MonthKeyFromDays(days));
  };
  for (const auto &delta : buffer.OrderDeltas())
    add_month_from_days(delta.orderdate_days);
  // month -> (custkey, signed revenue) for this buffer's lineitem deltas.
  std::map<int32_t, std::vector<std::pair<int64_t, int64_t>>> month_deltas;
  if (!buffer.Deltas().empty()) {
    struct Resolved {
      int32_t month;
      int64_t custkey, revenue;
    };
    struct Resolution {
      std::vector<Resolved> deltas;
      bool ok = true;
    } resolution;
    const auto &deltas = buffer.Deltas();
    core::ParallelReduce(
        services, deltas.size(), kResolveGrain, resolution,
        [&](Resolution &acc, std::size_t begin, std::size_t end) {
          for (std::size_t i = begin; i < end && acc.ok; ++i) {
            const TpchQ10Order *order = find_order(deltas[i].orderkey);
            if (!order) {
              acc.ok = false;
              return;
            }
            acc.deltas.push_back(
                {TpchQ10MonthKeyFromDays(order->orderdate_days), order->custkey,
                 static_cast<int64_t>(deltas[i].sign) * deltas[i].revenue_raw});
          }
        },
        [](Resolution &into, Resolution &&from) {
          into.ok = into.ok && from.ok;
          into.deltas.insert(into.deltas.end(), from.deltas.begin(),
                             from.deltas.end());
        });
    if (!resolution.ok)
      return rebuild_all();
    for (const auto &d : resolution.deltas) {
      rebuild_months.insert(d.month);
      month_deltas[d.month].emplace_back(d.custkey, d.revenue);
    }
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas),
       static_cast<uint64_t>(std::max<std::size_t>(1, rebuild_months.size())),
       true, true, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta &&
      decision.action != core::MaintenanceAction::RebuildPartition) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Month range of the published images (Meta partition).
  int32_t lo_ord = 0, hi_ord = -1;
  {
    const auto meta_snapshot = services.StateStore().ReadPartitions(
        visibility, {kMetaPartition});
    bool have_meta = false;
    for (const auto &image : meta_snapshot.partitions) {
      if (image.partition != kMetaPartition)
        continue;
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Meta);
      if (!typed)
        return rebuild_all();
      if (typed->hi_month >= typed->lo_month && typed->hi_month > 0) {
        lo_ord = MonthOrd(typed->lo_month);
        hi_ord = MonthOrd(typed->hi_month);
      }
      have_meta = true;
    }
    if (!have_meta)
      return rebuild_all();
  }
  // A touched month outside the imaged range needs new month / window images:
  // rebuild (rare: the data's month range is fixed between rebuilds).
  // Rebuild touches every order / lineitem month; deltas only the months that
  // carry a revenue delta.
  std::set<int32_t> touched_months;
  if (decision.action == core::MaintenanceAction::RebuildPartition)
    touched_months = rebuild_months;
  else
    for (const auto &entry : month_deltas)
      touched_months.insert(entry.first);
  for (const auto month : touched_months) {
    const auto o = MonthOrd(month);
    if (o < lo_ord || o > hi_ord)
      return rebuild_all();
  }
  // Windows containing a touched month, and the months they sum.
  std::set<int32_t> touched_window_ords, needed_month_ords;
  for (const auto month : touched_months)
    for (int32_t k = 0; k < 3; ++k)
      touched_window_ords.insert(MonthOrd(month) - k);
  for (const auto w : touched_window_ords)
    for (int32_t k = 0; k < 3; ++k) {
      const auto o = w + k;
      if (o >= lo_ord && o <= hi_ord)
        needed_month_ords.insert(o);
    }

  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    // Rescan every month the affected windows sum; republish the touched
    // months and all affected windows.
    std::set<int32_t> scan_months;
    for (const auto o : needed_month_ords)
      scan_months.insert(MonthFromOrd(o));
    std::map<int32_t, std::vector<TpchQ10KV>> scanned;
    if (!initial_scan || scan_months.empty() ||
        !initial_scan->ScanReturnedCustomerRevenueForMonths(
            scan_months, [&](const TpchQ10ReturnedRevenueInput &input) {
              if (input.revenue_raw != 0)
                scanned[input.month_key].push_back(
                    {input.custkey, input.revenue_raw});
            }))
      return rebuild_all();
    for (auto &[month, vec] : scanned) {
      std::sort(vec.begin(), vec.end(), TpchQ10ByKey{});
      // merge duplicates (defensive; the scan groups by (month, custkey))
      std::size_t w = 0;
      for (std::size_t i = 0; i < vec.size(); ++i) {
        if (w > 0 && vec[w - 1].key == vec[i].key)
          vec[w - 1].val += vec[i].val;
        else
          vec[w++] = vec[i];
      }
      vec.resize(w);
      vec.erase(std::remove_if(vec.begin(), vec.end(),
                               [](const TpchQ10KV &e) { return e.val == 0; }),
                vec.end());
    }
    static const std::vector<TpchQ10KV> kNone;
    const auto table = [&](int32_t ord) -> const std::vector<TpchQ10KV> * {
      if (ord < lo_ord || ord > hi_ord)
        return nullptr;
      const auto it = scanned.find(MonthFromOrd(ord));
      return it == scanned.end() ? &kNone : &it->second;
    };
    for (const auto month : touched_months) {
      std::vector<TpchQ10KV> copy = *table(MonthOrd(month));
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(month), 0,
          MakeMonthImage(month, TpchQ10KeyIndex::FromSorted(std::move(copy)))));
    }
    for (const auto w : touched_window_ords) {
      std::vector<const std::vector<TpchQ10KV> *> parts;
      for (int32_t k = 0; k < 3; ++k)
        parts.push_back(table(w + k));
      std::vector<TpchQ10KV> sums;
      SumMonths(parts, sums);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, WindowPartition(MonthFromOrd(w)), 0,
          MakeWindowImage(MonthFromOrd(w), RankedFromSums(std::move(sums)))));
    }
    if (orders_changed && order_metadata) {
      for (const auto &delta : buffer.OrderDeltas()) {
        if (delta.sign < 0)
          order_metadata->orders.Erase(delta.orderkey);
      }
      const auto typed_orders =
          std::static_pointer_cast<const TpchQ10State>(std::move(order_metadata));
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, core::TpchQ10Layout::kOrderMetadata,
          0, MakeStateImage(TpchQ10ImageKind::Orders, 0, typed_orders)));
    }
    if (side_update)
      outcome.updates.push_back(std::move(*side_update));
    core::FinishRebuildPartition(outcome);
    return outcome;
  }

  if (!month_deltas.empty()) {
    // Aggregate to per-month sorted (custkey, delta), dropping zero sums.
    std::vector<int32_t> months;
    std::size_t entries = 0;
    for (const auto &[month, deltas] : month_deltas) {
      months.push_back(month);
      entries += deltas.size();
    }
    // Months, windows and their images are independent; below the work grain
    // they run as one inline slice.
    const auto for_each_image = [&](std::size_t n, auto &&fn) {
      core::ParallelSlices(
          services, n, entries < kImageWorkGrain ? std::max<std::size_t>(n, 1) : 1,
          [&](std::size_t, std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i)
              fn(i);
          });
    };
    std::vector<std::vector<TpchQ10KV>> month_agg(months.size());
    for_each_image(months.size(), [&](std::size_t i) {
      auto &v = month_agg[i];
      const auto &deltas = month_deltas.at(months[i]);
      v.reserve(deltas.size());
      for (const auto &[key, d] : deltas)
        v.push_back({key, d});
      std::sort(v.begin(), v.end(), TpchQ10ByKey{});
      std::size_t w = 0;
      for (std::size_t j = 0; j < v.size(); ++j) {
        if (w > 0 && v[w - 1].key == v[j].key)
          v[w - 1].val += v[j].val;
        else
          v[w++] = v[j];
      }
      v.resize(w);
      v.erase(std::remove_if(v.begin(), v.end(),
                             [](const TpchQ10KV &e) { return e.val == 0; }),
              v.end());
    });
    std::map<int32_t, std::vector<TpchQ10KV>> agg;
    for (std::size_t i = 0; i < months.size(); ++i)
      if (!month_agg[i].empty())
        agg.emplace(months[i], std::move(month_agg[i]));

    std::vector<core::PartitionId> partitions;
    for (const auto o : needed_month_ords)
      partitions.push_back(MonthPartition(MonthFromOrd(o)));
    for (const auto w : touched_window_ords)
      partitions.push_back(WindowPartition(MonthFromOrd(w)));
    const auto snapshot =
        services.StateStore().ReadPartitions(visibility, partitions);
    std::map<int32_t, const TpchQ10StateImage *> month_imgs, win_imgs;
    std::map<int32_t, core::Version> month_versions, win_versions;
    for (const auto &image : snapshot.partitions) {
      if (image.partition >= kFirstWindow) {
        const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Window);
        if (!typed)
          return rebuild_all();
        const auto w = MonthOrd(typed->month);
        win_imgs[w] = typed;
        win_versions[w] = image.version;
      } else if (image.partition >= core::TpchQ10Layout::kFirstMonth) {
        const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Month);
        if (!typed)
          return rebuild_all();
        const auto o = MonthOrd(typed->month);
        month_imgs[o] = typed;
        month_versions[o] = image.version;
      }
    }
    if (month_imgs.size() != needed_month_ords.size() ||
        win_imgs.size() != touched_window_ords.size())
      return rebuild_all();

    // Windows: for each key with a delta in any of the window's months,
    // old window revenue = sum of the (published) month values.
    const std::vector<int32_t> window_ords(touched_window_ords.begin(),
                                           touched_window_ords.end());
    std::vector<std::optional<core::StateImage>> window_updates(
        window_ords.size());
    std::atomic<bool> failed{false};
    for_each_image(window_ords.size(), [&](std::size_t i) {
      const auto w = window_ords[i];
      std::vector<int64_t> keys;
      for (int32_t k = 0; k < 3; ++k) {
        const auto it = agg.find(MonthFromOrd(w + k));
        if (it == agg.end())
          continue;
        for (const auto &e : it->second)
          keys.push_back(e.key);
      }
      if (keys.empty())
        return;
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      std::vector<TpchQ10KV> rem, ins;
      for (const auto key : keys) {
        int64_t old_sum = 0, delta = 0;
        for (int32_t k = 0; k < 3; ++k) {
          const auto mit = month_imgs.find(w + k);
          if (mit != month_imgs.end())
            old_sum += mit->second->by_key.FindVal(key);
          const auto it = agg.find(MonthFromOrd(w + k));
          if (it == agg.end())
            continue;
          const auto pos = std::lower_bound(
              it->second.begin(), it->second.end(), TpchQ10KV{key, 0},
              TpchQ10ByKey{});
          if (pos != it->second.end() && pos->key == key)
            delta += pos->val;
        }
        if (delta == 0)
          continue;
        if (old_sum != 0)
          rem.push_back({key, old_sum});
        if (old_sum + delta != 0)
          ins.push_back({key, old_sum + delta});
      }
      std::sort(rem.begin(), rem.end(), TpchQ10ByRank{});
      std::sort(ins.begin(), ins.end(), TpchQ10ByRank{});
      TpchQ10RankIndex next;
      if (!TpchQ10RankIndex::Apply(win_imgs.at(w)->ranked, rem, ins, next)) {
        failed = true;
        return;
      }
      window_updates[i] = core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, WindowPartition(MonthFromOrd(w)),
          win_versions.at(w), MakeWindowImage(MonthFromOrd(w), std::move(next)));
    });

    // Months.
    std::vector<std::pair<int32_t, const std::vector<TpchQ10KV> *>> month_list;
    for (const auto &[month, deltas] : agg)
      month_list.emplace_back(month, &deltas);
    std::vector<std::optional<core::StateImage>> month_updates(
        month_list.size());
    for_each_image(month_list.size(), [&](std::size_t i) {
      const auto month = month_list[i].first;
      const auto o = MonthOrd(month);
      std::vector<TpchQ10KV> rem, ins;
      const auto &prev = month_imgs.at(o)->by_key;
      for (const auto &e : *month_list[i].second) {
        const int64_t old_val = prev.FindVal(e.key);
        if (old_val != 0)
          rem.push_back({e.key, old_val});
        if (old_val + e.val != 0)
          ins.push_back({e.key, old_val + e.val});
      }
      TpchQ10KeyIndex next;
      if (!TpchQ10KeyIndex::Apply(prev, rem, ins, next)) {
        failed = true;
        return;
      }
      month_updates[i] = core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(month),
          month_versions.at(o), MakeMonthImage(month, std::move(next)));
    });
    if (failed)
      return rebuild_all();
    for (auto *updates : {&window_updates, &month_updates})
      for (auto &update : *updates)
        if (update)
          outcome.updates.push_back(std::move(*update));
    if (!outcome.updates.empty())
      outcome.action = core::MaintenanceAction::ApplyDelta;
  }

  // RF2: remove order metadata after lineitem deltas have resolved against it.
  if (order_metadata) {
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign < 0) {
        order_metadata->orders.Erase(delta.orderkey);
        orders_changed = true;
      }
    }
  }

  if (orders_changed && order_metadata) {
    outcome.action = core::MaintenanceAction::ApplyDelta;
    const auto typed_orders =
        std::static_pointer_cast<const TpchQ10State>(std::move(order_metadata));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator, core::TpchQ10Layout::kOrderMetadata,
        order_version,
        MakeStateImage(TpchQ10ImageKind::Orders, 0, typed_orders)));
  }
  if (side_update) {
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(std::move(*side_update));
  }
  return outcome;
}

bool TpchQ10Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}
void TpchQ10Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start =
          std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else {
      int64_t value = 0;
      if (std::holds_alternative<int64_t>(arguments.values[0].payload))
        value = std::get<int64_t>(arguments.values[0].payload);
      start = static_cast<int32_t>(value);
    }
  }
  const auto start_month = TpchQ10MonthKeyFromDays(start);
  if (TpchQ10MonthStartDays(start_month) != start) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // Side + the one precomputed window for this month start: the answer is
  // the window's first 20 entries (skipping customers that no longer exist).
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ10Layout::kCustomerDimensions, WindowPartition(start_month)});
  const TpchQ10State *side = nullptr;
  const TpchQ10StateImage *window = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ10Layout::kCustomerDimensions) {
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Side);
      if (typed)
        side = typed->state.get();
    } else if (image.partition == WindowPartition(start_month)) {
      window = AsQ10Image(image, TpchQ10ImageKind::Window);
    }
  }
  if (!side) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!window) {
    // No image: a window that starts outside [lo - 2, hi] has no revenue at
    // all (empty answer); anything else is unexpected, so recompute.
    const auto meta_snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), {kMetaPartition});
    for (const auto &image : meta_snapshot.partitions) {
      const auto *meta = AsQ10Image(image, TpchQ10ImageKind::Meta);
      if (!meta)
        continue;
      const bool empty_data = meta->hi_month < meta->lo_month;
      const auto ord = MonthOrd(start_month);
      if (empty_data || ord < MonthOrd(meta->lo_month) - 2 ||
          ord > MonthOrd(meta->hi_month)) {
        TpchQ10EmitRows(*side, {}, sink);
        return;
      }
    }
    ExecuteFallback(services, arguments, sink);
    return;
  }
  TpchQ10EmitRows(*side, TpchQ10WindowTop(*side, window->ranked, kTpchQ10ResultLimit),
                  sink);
}
void TpchQ10Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() &&
      std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
    start =
        std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
  EmitResult(BuildState(services), start, sink);
}

TpchQ10BuildData
TpchQ10Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ10BuildData data;
  TpchQ10State &state = data.state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q10 requires a typed initial scan source");

  const auto t0 = std::chrono::steady_clock::now();
  auto log_phase = [&](const char *phase, auto start) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::cerr << "tpch_q10_aux_build: " << phase << " " << ms << " ms\n"
              << std::flush;
  };

  auto t = std::chrono::steady_clock::now();
  auto base = std::make_shared<TpchQ10SideBase>();
  initial_scan->ScanCustomers([&](const TpchQ10CustomerInput &input) {
    base->SetCustomerRank(input.key, input.acctbal_raw, input.nationkey);
    base->customers[input.key] = {input.name,    input.acctbal_raw,
                                  input.nationkey, input.address,
                                  input.phone,   input.comment};
  });
  state.side_base = std::move(base);
  log_phase("customers", t);

  t = std::chrono::steady_clock::now();
  initial_scan->ScanNations([&](const TpchQ10NationInput &input) {
    state.nations[input.key] = input.name;
  });
  log_phase("nations", t);

  // Prefer engine-side aggregation of returned revenue (DuckDB SQL join).
  // Skip the full orders map in that case; write maintenance uses LookupOrders.
  t = std::chrono::steady_clock::now();
  const bool used_sql_revenue = initial_scan->ScanReturnedCustomerRevenue(
      [&](const TpchQ10ReturnedRevenueInput &input) {
        if (input.revenue_raw == 0)
          return;
        data.months[input.month_key].push_back(
            {input.custkey, input.revenue_raw});
      });
  log_phase(used_sql_revenue ? "returned_revenue_sql" : "returned_revenue_skip",
            t);

  if (!used_sql_revenue) {
    t = std::chrono::steady_clock::now();
    initial_scan->ScanOrders([&](const TpchQ10OrderInput &input) {
      state.orders.Upsert(input.key,
                          TpchQ10Order{input.custkey, input.orderdate_days});
    });
    log_phase("orders", t);
    t = std::chrono::steady_clock::now();
    initial_scan->ScanLineitems([&](const TpchQ10LineitemInput &input) {
      if (!input.returned)
        return;
      const TpchQ10Order *order = state.orders.Find(input.orderkey);
      if (!order)
        return;
      double extended_scale = 1.0;
      double discount_scale = 1.0;
      for (int32_t i = 0; i < input.extended_price_scale; ++i)
        extended_scale *= 10.0;
      for (int32_t i = 0; i < input.discount_scale; ++i)
        discount_scale *= 10.0;
      const auto revenue = TpchQ10RevenueRaw(
          static_cast<double>(input.extended_price_raw) / extended_scale,
          static_cast<double>(input.discount_raw) / discount_scale);
      data.months[TpchQ10MonthKeyFromDays(order->orderdate_days)]
          .push_back({order->custkey, revenue});
    });
    log_phase("lineitems", t);
  }
  // Key-sorted, deduplicated (summed), nonzero month tables.
  std::vector<std::vector<TpchQ10KV> *> tables;
  for (auto &entry : data.months)
    tables.push_back(&entry.second);
  services.ParallelFor(tables.size(), [&](std::size_t i) {
    auto &vec = *tables[i];
    std::sort(vec.begin(), vec.end(), TpchQ10ByKey{});
    std::size_t w = 0;
    for (std::size_t j = 0; j < vec.size(); ++j) {
      if (w > 0 && vec[w - 1].key == vec[j].key)
        vec[w - 1].val += vec[j].val;
      else
        vec[w++] = vec[j];
    }
    vec.resize(w);
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [](const TpchQ10KV &e) { return e.val == 0; }),
              vec.end());
    vec.shrink_to_fit();
  });
  for (auto it = data.months.begin(); it != data.months.end();)
    it = it->second.empty() ? data.months.erase(it) : std::next(it);
  log_phase("total_build_state", t0);
  return data;
}

void TpchQ10Module::EmitResult(const TpchQ10BuildData &data, int32_t start_days,
                               core::AuxRowSink &sink) const {
  const auto end_days = TpchQ10AddMonths(start_days, 3);
  std::map<int64_t, int64_t> totals;
  for (int32_t cursor = start_days; cursor < end_days;) {
    const auto month = TpchQ10MonthKeyFromDays(cursor);
    const auto it = data.months.find(month);
    if (it != data.months.end())
      for (const auto &e : it->second)
        totals[e.key] += e.val;
    const auto next = TpchQ10AddMonths(TpchQ10MonthStartDays(month), 1);
    if (next <= cursor)
      break;
    cursor = next;
  }
  std::vector<TpchQ10KV> ranked;
  for (const auto &[key, total] : totals)
    if (total > 0 && data.state.HasCustomer(key))
      ranked.push_back({key, total});
  std::sort(ranked.begin(), ranked.end(), TpchQ10ByRank{});
  if (ranked.size() > kTpchQ10ResultLimit)
    ranked.resize(kTpchQ10ResultLimit);
  std::vector<std::pair<int64_t, int64_t>> out;
  for (const auto &e : ranked)
    out.emplace_back(e.key, e.val);
  TpchQ10EmitRows(data.state, out, sink);
}

std::vector<core::CalibrationWrite> TpchQ10Module::CalibrationWrites() const {
  return TpchQ10CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
