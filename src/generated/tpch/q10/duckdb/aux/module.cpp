#include "tpch/q10/duckdb/aux/module.hpp"
#include "tpch/q10/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
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

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kSideMagic = 0x53303151;   // Q10S
constexpr uint32_t kOrdersMagic = 0x4f303151; // Q10O
constexpr uint32_t kMonthMagic = 0x4d303151;  // Q10M
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
  return payload && payload->kind == kind && payload->state ? payload
                                                              : nullptr;
}

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

bool RankLess(const std::vector<int64_t> &keys, const std::vector<int64_t> &vals,
              uint32_t a, uint32_t b) {
  if (vals[a] != vals[b])
    return vals[a] > vals[b];
  return keys[a] < keys[b];
}

std::shared_ptr<const TpchQ10MonthBase>
MakeMonthBase(const std::vector<std::pair<int64_t, int64_t>> &sorted_pairs) {
  auto base = std::make_shared<TpchQ10MonthBase>();
  base->keys.reserve(sorted_pairs.size());
  base->vals.reserve(sorted_pairs.size());
  for (const auto &[key, val] : sorted_pairs) {
    if (val == 0)
      continue;
    base->keys.push_back(key);
    base->vals.push_back(val);
    if (val < 0)
      base->has_negative = true;
  }
  base->rank.resize(base->keys.size());
  std::iota(base->rank.begin(), base->rank.end(), 0u);
  std::sort(base->rank.begin(), base->rank.end(),
            [&](uint32_t a, uint32_t b) {
              return RankLess(base->keys, base->vals, a, b);
            });
  return base;
}

} // namespace

std::shared_ptr<const TpchQ10MonthData>
TpchQ10MonthFromMap(const std::unordered_map<int64_t, int64_t> &revenue) {
  std::vector<std::pair<int64_t, int64_t>> pairs;
  pairs.reserve(revenue.size());
  for (const auto &entry : revenue)
    if (entry.second != 0)
      pairs.emplace_back(entry.first, entry.second);
  std::sort(pairs.begin(), pairs.end());
  auto data = std::make_shared<TpchQ10MonthData>();
  data->base = MakeMonthBase(pairs);
  return data;
}

std::vector<std::pair<int64_t, int64_t>>
TpchQ10MonthEffective(const TpchQ10MonthData &month) {
  const auto &b = *month.base;
  std::vector<std::pair<int64_t, int64_t>> out;
  out.reserve(b.keys.size() + month.ov_keys.size());
  std::size_t i = 0, j = 0;
  while (i < b.keys.size() || j < month.ov_keys.size()) {
    if (j >= month.ov_keys.size() ||
        (i < b.keys.size() && b.keys[i] < month.ov_keys[j])) {
      out.emplace_back(b.keys[i], b.vals[i]);
      ++i;
    } else {
      if (i < b.keys.size() && b.keys[i] == month.ov_keys[j])
        ++i;
      if (month.ov_vals[j] != 0)
        out.emplace_back(month.ov_keys[j], month.ov_vals[j]);
      ++j;
    }
  }
  return out;
}

std::shared_ptr<const TpchQ10MonthData>
TpchQ10MonthApply(const TpchQ10MonthData *prev,
                  const std::vector<std::pair<int64_t, int64_t>> &deltas) {
  static const TpchQ10MonthData kEmpty;
  const TpchQ10MonthData &p = prev ? *prev : kEmpty;
  std::map<int64_t, int64_t> agg;
  for (const auto &[key, delta] : deltas)
    agg[key] += delta;
  std::map<int64_t, int64_t> ov;
  for (std::size_t i = 0; i < p.ov_keys.size(); ++i)
    ov.emplace(p.ov_keys[i], p.ov_vals[i]);
  for (const auto &[key, delta] : agg)
    ov[key] = p.Lookup(key) + delta;

  auto next = std::make_shared<TpchQ10MonthData>();
  if (ov.size() > kMonthOverlayCap) {
    // Compact: fold the overlay into a new base (linear merge + one sort).
    TpchQ10MonthData folded;
    folded.base = p.base;
    folded.ov_keys.reserve(ov.size());
    folded.ov_vals.reserve(ov.size());
    for (const auto &[key, val] : ov) {
      folded.ov_keys.push_back(key);
      folded.ov_vals.push_back(val);
    }
    next->base = MakeMonthBase(TpchQ10MonthEffective(folded));
    return next;
  }
  next->base = p.base;
  next->ov_keys.reserve(ov.size());
  next->ov_vals.reserve(ov.size());
  for (const auto &[key, val] : ov) {
    next->ov_keys.push_back(key);
    next->ov_vals.push_back(val);
    if (val < 0)
      next->has_negative = true;
  }
  for (uint32_t i = 0; i < next->ov_keys.size(); ++i)
    if (next->ov_vals[i] != 0)
      next->ov_rank.push_back(i);
  std::sort(next->ov_rank.begin(), next->ov_rank.end(),
            [&](uint32_t a, uint32_t b) {
              return RankLess(next->ov_keys, next->ov_vals, a, b);
            });
  return next;
}

std::vector<uint8_t> EncodeTpchQ10Side(const TpchQ10State &input) {
  TpchQ10State folded;
  const TpchQ10State *stateptr = &input;
  if (input.customer_overlay && !input.customer_overlay->empty()) {
    folded.side_base = CompactSideBase(input);
    folded.nations = input.nations;
    stateptr = &folded;
  }
  const auto &state = *stateptr;
  ByteWriter writer;
  std::vector<int64_t> keys;
  CollectCustomerRankKeys(*state.side_base, keys);
  core::WriteImageHeader(writer, kSideMagic, kVersion, static_cast<uint32_t>(keys.size()));
  static const TpchQ10Customer kNoCustomer;
  for (const auto key : keys) {
    const auto rank = state.side_base->ranks.Get(key);
    const auto it = state.side_base->customers.find(key);
    const auto &customer =
        it == state.side_base->customers.end() ? kNoCustomer : it->second;
    writer.I64(key);
    writer.String(customer.name);
    writer.I64(rank.acctbal_raw);
    writer.I32(rank.nationkey);
    writer.String(customer.address);
    writer.String(customer.phone);
    writer.String(customer.comment);
  }
  writer.U32(static_cast<uint32_t>(state.nations.size()));
  std::vector<int32_t> nations;
  for (const auto &entry : state.nations)
    nations.push_back(entry.first);
  std::sort(nations.begin(), nations.end());
  for (const auto key : nations) {
    writer.I32(key);
    writer.String(state.nations.at(key));
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ10Orders(const TpchQ10State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kOrdersMagic, kVersion, static_cast<uint32_t>(state.orders.size()));
  std::vector<int64_t> keys;
  for (const auto &entry : state.orders)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  for (const auto key : keys) {
    writer.I64(key);
    writer.I64(state.orders.at(key).custkey);
    writer.I32(state.orders.at(key).orderdate_days);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ10Month(int32_t /*month*/,
                                        const TpchQ10State &state) {
  std::vector<std::pair<int64_t, int64_t>> pairs;
  if (state.month_data)
    pairs = TpchQ10MonthEffective(*state.month_data);
  ByteWriter writer;
  core::WriteImageHeader(writer, kMonthMagic, kVersion, static_cast<uint32_t>(pairs.size()));
  for (const auto &[key, val] : pairs) {
    writer.I64(key);
    writer.I64(val);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ10Side(const std::vector<uint8_t> &bytes, TpchQ10State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kSideMagic, kVersion, count))
    return false;
  state.nations.clear();
  state.customer_overlay = nullptr;
  auto base = std::make_shared<TpchQ10SideBase>();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    TpchQ10Customer customer;
    if (!reader.I64(key) || !reader.String(customer.name) ||
        !reader.I64(customer.acctbal_raw) || !reader.I32(customer.nationkey) ||
        !reader.String(customer.address) || !reader.String(customer.phone) ||
        !reader.String(customer.comment))
      return false;
    base->SetCustomerRank(key, customer.acctbal_raw, customer.nationkey);
    base->customers.emplace(key, std::move(customer));
  }
  state.side_base = std::move(base);
  if (!reader.U32(count))
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    int32_t key = 0;
    std::string name;
    if (!reader.I32(key) || !reader.String(name))
      return false;
    state.nations.emplace(key, std::move(name));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ10Orders(const std::vector<uint8_t> &bytes,
                         TpchQ10State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kOrdersMagic, kVersion, count))
    return false;
  state.orders.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    TpchQ10Order order;
    if (!reader.I64(key) || !reader.I64(order.custkey) ||
        !reader.I32(order.orderdate_days))
      return false;
    state.orders.emplace(key, order);
  }
  return reader.offset == bytes.size();
}

bool FindTpchQ10Order(const std::vector<uint8_t> &bytes, int64_t orderkey,
                      TpchQ10Order &order) {
  ByteReader header{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(header, kOrdersMagic, kVersion, count)) {
    return false;
  }

  // EncodeTpchQ10Orders writes key, custkey, and date as three little-endian
  // fixed-width values. Validate the complete image before doing indexed
  // reads so malformed state falls back to a rebuild instead of being
  // partially consumed.
  constexpr std::size_t kHeaderBytes = 12;
  constexpr std::size_t kRecordBytes = 20;
  if (count > (std::numeric_limits<std::size_t>::max() - kHeaderBytes) /
                 kRecordBytes ||
      bytes.size() != kHeaderBytes +
                          static_cast<std::size_t>(count) * kRecordBytes) {
    return false;
  }

  std::size_t first = 0;
  std::size_t last = count;
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    ByteReader reader{bytes};
    reader.offset = kHeaderBytes + middle * kRecordBytes;
    int64_t key = 0;
    if (!reader.I64(key)) {
      return false;
    }
    if (key < orderkey) {
      first = middle + 1;
      continue;
    }
    if (key > orderkey) {
      last = middle;
      continue;
    }
    if (!reader.I64(order.custkey) || !reader.I32(order.orderdate_days)) {
      return false;
    }
    return true;
  }
  return false;
}

bool DecodeTpchQ10Month(const std::vector<uint8_t> &bytes, int32_t /*month*/,
                        TpchQ10State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kMonthMagic, kVersion, count))
    return false;
  std::vector<std::pair<int64_t, int64_t>> pairs;
  pairs.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0, raw = 0;
    if (!reader.I64(key) || !reader.I64(raw))
      return false;
    if (raw != 0)
      pairs.emplace_back(key, raw);
  }
  if (reader.offset != bytes.size())
    return false;
  if (!std::is_sorted(pairs.begin(), pairs.end()))
    std::sort(pairs.begin(), pairs.end());
  auto data = std::make_shared<TpchQ10MonthData>();
  data->base = MakeMonthBase(pairs);
  state.month_data = std::move(data);
  return true;
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
  auto state = BuildState(services);

  // Publish lean per-kind images so Side/Month payloads do not retain the
  // full orders map (orders are write-maintenance only).
  auto side_state = std::make_shared<TpchQ10State>();
  side_state->side_base = std::move(state.side_base);
  side_state->nations = std::move(state.nations);
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator,
      core::TpchQ10Layout::kCustomerDimensions, 0,
      MakeStateImage(TpchQ10ImageKind::Side, 0,
                     std::shared_ptr<const TpchQ10State>(side_state))));

  // Orders partition is optional: SQL-pushdown builds skip it and resolve
  // orderkeys via LookupOrders during maintenance.
  if (!state.orders.empty()) {
    auto orders_state = std::make_shared<TpchQ10State>();
    orders_state->orders = std::move(state.orders);
    builder.Write(core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator,
        core::TpchQ10Layout::kOrderMetadata, 0,
        MakeStateImage(TpchQ10ImageKind::Orders, 0,
                       std::shared_ptr<const TpchQ10State>(orders_state))));
  }

  for (auto &entry : state.month_revenue) {
    auto month_state = std::make_shared<TpchQ10State>();
    month_state->month_data = TpchQ10MonthFromMap(entry.second);
    entry.second = {}; // release the hash map as soon as it is flattened
    builder.Write(core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator, MonthPartition(entry.first), 0,
        MakeStateImage(TpchQ10ImageKind::Month, entry.first,
                       std::shared_ptr<const TpchQ10State>(month_state))));
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
        order_metadata->orders[delta.orderkey] =
            TpchQ10Order{delta.custkey, delta.orderdate_days};
        orders_changed = true;
      }
    }
  }

  // When build skipped the Orders partition, resolve only the touched keys.
  // Same-txn RF1 inserts (and RF2 deletes, which carry their old values) are
  // taken from the order deltas themselves.
  std::unordered_map<int64_t, TpchQ10Order> looked_up_orders;
  std::unordered_map<int64_t, TpchQ10Order> overlay_orders;
  const std::unordered_map<int64_t, TpchQ10Order> *orders_by_key = nullptr;
  if (have_orders_partition && order_metadata) {
    orders_by_key = &order_metadata->orders;
  } else if (!buffer.Deltas().empty()) {
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
    orders_by_key = &overlay_orders;
  }

  std::set<int32_t> rebuild_months;
  auto add_month_from_days = [&](int32_t days) {
    rebuild_months.insert(TpchQ10MonthKeyFromDays(days));
  };
  for (const auto &delta : buffer.OrderDeltas())
    add_month_from_days(delta.orderdate_days);
  // month -> (custkey, signed revenue) for this buffer's lineitem deltas.
  std::map<int32_t, std::vector<std::pair<int64_t, int64_t>>> month_deltas;
  if (!buffer.Deltas().empty()) {
    if (!orders_by_key)
      return rebuild_all();
    for (const auto &delta : buffer.Deltas()) {
      const auto order_it = orders_by_key->find(delta.orderkey);
      if (order_it == orders_by_key->end())
        return rebuild_all();
      const auto month = TpchQ10MonthKeyFromDays(order_it->second.orderdate_days);
      rebuild_months.insert(month);
      month_deltas[month].emplace_back(
          order_it->second.custkey,
          static_cast<int64_t>(delta.sign) * delta.revenue_raw);
    }
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas),
       static_cast<uint64_t>(std::max<std::size_t>(1, rebuild_months.size())),
       true, true, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    if (!initial_scan || rebuild_months.empty() ||
        !initial_scan->ScanReturnedCustomerRevenueForMonths(
            rebuild_months, [&](const TpchQ10ReturnedRevenueInput &input) {
              if (input.revenue_raw == 0)
                return;
              state.month_revenue[input.month_key][input.custkey] +=
                  input.revenue_raw;
            }))
      return rebuild_all();
    for (const auto month : rebuild_months) {
      auto month_state = std::make_shared<TpchQ10State>();
      const auto month_it = state.month_revenue.find(month);
      month_state->month_data =
          month_it == state.month_revenue.end()
              ? TpchQ10MonthFromMap({})
              : TpchQ10MonthFromMap(month_it->second);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(month), 0,
          MakeStateImage(TpchQ10ImageKind::Month, month,
                         std::shared_ptr<const TpchQ10State>(month_state))));
    }
    if (orders_changed && order_metadata) {
      for (const auto &delta : buffer.OrderDeltas()) {
        if (delta.sign < 0)
          order_metadata->orders.erase(delta.orderkey);
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
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (!month_deltas.empty()) {
    std::vector<core::PartitionId> month_partitions;
    month_partitions.reserve(month_deltas.size());
    for (const auto &entry : month_deltas)
      month_partitions.push_back(MonthPartition(entry.first));
    std::map<int32_t, core::Version> versions;
    std::map<int32_t, std::shared_ptr<const TpchQ10State>> previous;
    const auto month_snapshot =
        services.StateStore().ReadPartitions(visibility, month_partitions);
    for (const auto &image : month_snapshot.partitions) {
      if (image.partition < core::TpchQ10Layout::kFirstMonth)
        continue;
      const auto month = static_cast<int32_t>(
          image.partition - core::TpchQ10Layout::kFirstMonth);
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Month);
      if (!typed || typed->month != month)
        return rebuild_all();
      previous[month] = typed->state;
      versions[month] = image.version;
    }
    if (versions.size() != month_deltas.size())
      return rebuild_all();

    outcome.action = core::MaintenanceAction::ApplyDelta;
    for (const auto &[month, deltas] : month_deltas) {
      auto month_state = std::make_shared<TpchQ10State>();
      month_state->month_data =
          TpchQ10MonthApply(previous[month]->month_data.get(), deltas);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(month),
          versions[month],
          MakeStateImage(TpchQ10ImageKind::Month, month,
                         std::shared_ptr<const TpchQ10State>(month_state))));
    }
  }

  // RF2: remove order metadata after lineitem deltas have resolved against it.
  if (order_metadata) {
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign < 0) {
        order_metadata->orders.erase(delta.orderkey);
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
  if (TpchQ10MonthStartDays(TpchQ10MonthKeyFromDays(start)) != start) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // Keep Side/month partitions by shared_ptr — do not deep-copy the SF-scale
  // customer map or month revenue tables on the hit path.
  std::vector<core::PartitionId> partitions = {
      core::TpchQ10Layout::kCustomerDimensions};
  const auto end_days = TpchQ10AddMonths(start, 3);
  for (int32_t cursor = start; cursor < end_days;) {
    const auto month = TpchQ10MonthKeyFromDays(cursor);
    partitions.push_back(MonthPartition(month));
    const auto next = TpchQ10AddMonths(TpchQ10MonthStartDays(month), 1);
    if (next <= cursor) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    cursor = next;
  }
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  std::shared_ptr<const TpchQ10State> side;
  // Hold shared ownership of the month images for the duration of finalize.
  std::vector<std::shared_ptr<const TpchQ10State>> month_holders;
  std::vector<const TpchQ10MonthData *> months;
  month_holders.reserve(3);
  months.reserve(3);
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ10Layout::kCustomerDimensions) {
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Side);
      if (typed)
        side = typed->state;
    } else if (image.partition >= core::TpchQ10Layout::kFirstMonth) {
      const auto month = static_cast<int32_t>(image.partition -
                                              core::TpchQ10Layout::kFirstMonth);
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Month);
      if (!typed || typed->month != month) {
        ExecuteFallback(services, arguments, sink);
        return;
      }
      month_holders.push_back(typed->state);
      if (typed->state->month_data)
        months.push_back(typed->state->month_data.get());
    }
  }
  if (!side) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  TpchQ10FinalizeAndEmit(*side, months, sink, kTpchQ10ResultLimit);
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

TpchQ10State
TpchQ10Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ10State state;
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
        state.month_revenue[input.month_key][input.custkey] += input.revenue_raw;
      });
  log_phase(used_sql_revenue ? "returned_revenue_sql" : "returned_revenue_skip",
            t);

  if (!used_sql_revenue) {
    t = std::chrono::steady_clock::now();
    initial_scan->ScanOrders([&](const TpchQ10OrderInput &input) {
      state.orders[input.key] = {input.custkey, input.orderdate_days};
    });
    log_phase("orders", t);
    t = std::chrono::steady_clock::now();
    initial_scan->ScanLineitems([&](const TpchQ10LineitemInput &input) {
      if (!input.returned)
        return;
      const auto order = state.orders.find(input.orderkey);
      if (order == state.orders.end())
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
      state.month_revenue[TpchQ10MonthKeyFromDays(
          order->second.orderdate_days)][order->second.custkey] += revenue;
    });
    log_phase("lineitems", t);
  }
  log_phase("total_build_state", t0);
  return state;
}

void TpchQ10Module::EmitResult(const TpchQ10State &state, int32_t start_days,
                               core::AuxRowSink &sink) const {
  const auto end_days = TpchQ10AddMonths(start_days, 3);
  std::vector<std::shared_ptr<const TpchQ10MonthData>> holders;
  for (int32_t cursor = start_days; cursor < end_days;) {
    const auto month = TpchQ10MonthKeyFromDays(cursor);
    const auto it = state.month_revenue.find(month);
    if (it != state.month_revenue.end())
      holders.push_back(TpchQ10MonthFromMap(it->second));
    const auto next = TpchQ10AddMonths(TpchQ10MonthStartDays(month), 1);
    if (next <= cursor)
      break;
    cursor = next;
  }
  std::vector<const TpchQ10MonthData *> months;
  for (const auto &h : holders)
    months.push_back(h.get());
  TpchQ10FinalizeAndEmit(state, months, sink, kTpchQ10ResultLimit);
}

std::vector<core::CalibrationWrite> TpchQ10Module::CalibrationWrites() const {
  return TpchQ10CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
