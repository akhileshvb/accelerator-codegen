#include "tpch/q10/postgres/aux/module.hpp"
#include "tpch/q10/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
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

void CollectCustomerRankKeys(const TpchQ10State &state,
                             std::vector<int64_t> &keys) {
  keys.clear();
  for (std::size_t chunk_idx = 0;
       chunk_idx < state.customer_ranks.chunks.size(); ++chunk_idx) {
    const auto &chunk = state.customer_ranks.chunks[chunk_idx];
    const int64_t base = static_cast<int64_t>(
        chunk_idx << core::DenseIdArray<TpchQ10CustomerRank>::kShift);
    for (std::size_t slot = 0; slot < chunk.size(); ++slot) {
      if (chunk[slot].present == 0)
        continue;
      keys.push_back(base + static_cast<int64_t>(slot));
    }
  }
  std::sort(keys.begin(), keys.end());
}

} // namespace

std::vector<uint8_t> EncodeTpchQ10Side(const TpchQ10State &state) {
  ByteWriter writer;
  std::vector<int64_t> keys;
  CollectCustomerRankKeys(state, keys);
  core::WriteImageHeader(writer, kSideMagic, kVersion, static_cast<uint32_t>(keys.size()));
  static const TpchQ10Customer kNoCustomer;
  for (const auto key : keys) {
    const auto rank = state.RankOf(key);
    const auto it = state.customers->find(key);
    const auto &customer = it == state.customers->end() ? kNoCustomer : it->second;
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

std::vector<uint8_t> EncodeTpchQ10Month(int32_t month,
                                        const TpchQ10State &state) {
  const auto it = state.month_revenue.find(month);
  std::vector<int64_t> keys;
  if (it != state.month_revenue.end()) {
    for (const auto &entry : it->second)
      if (entry.second != 0)
        keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
  }
  ByteWriter writer;
  core::WriteImageHeader(writer, kMonthMagic, kVersion, static_cast<uint32_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.I64(it->second.at(key));
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ10Side(const std::vector<uint8_t> &bytes, TpchQ10State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kSideMagic, kVersion, count))
    return false;
  state.customer_ranks.Clear();
  state.nations.clear();
  auto customers = std::make_shared<TpchQ10CustomerMap>();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    TpchQ10Customer customer;
    if (!reader.I64(key) || !reader.String(customer.name) ||
        !reader.I64(customer.acctbal_raw) || !reader.I32(customer.nationkey) ||
        !reader.String(customer.address) || !reader.String(customer.phone) ||
        !reader.String(customer.comment))
      return false;
    state.SetCustomerRank(key, customer.acctbal_raw, customer.nationkey);
    customers->emplace(key, std::move(customer));
  }
  state.customers = std::move(customers);
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

bool DecodeTpchQ10Month(const std::vector<uint8_t> &bytes, int32_t month,
                        TpchQ10State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kMonthMagic, kVersion, count))
    return false;
  auto &revenue = state.month_revenue[month];
  revenue.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0, raw = 0;
    if (!reader.I64(key) || !reader.I64(raw))
      return false;
    if (raw != 0)
      revenue[key] = raw;
  }
  return reader.offset == bytes.size();
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
}
bool TpchQ10TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ10LineitemDelta> &TpchQ10TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ10OrderDelta> &TpchQ10TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ10TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
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
  side_state->customer_ranks = std::move(state.customer_ranks);
  side_state->customers = std::move(state.customers);
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
    month_state->month_revenue.emplace(entry.first, std::move(entry.second));
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
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.customer_table ||
      mutation.table == config.nation_table)
    return All();
  return {};
}

void TpchQ10Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
  if (mutation.table == config.customer_table ||
      mutation.table == config.nation_table) {
    buffer.MarkRebuild();
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
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty())
    return outcome;

  // An updated or deleted order moves (or drops) the revenue of all its
  // existing lineitems, which the deltas cannot do: rebuild.
  for (const auto &delta : buffer.OrderDeltas()) {
    if (delta.sign < 0) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
  }

  const auto total_deltas =
      buffer.Deltas().size() + buffer.OrderDeltas().size();

  TpchQ10State state;
  const auto visibility = services.CurrentVisibility();
  const auto order_snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ10Layout::kOrderMetadata});
  std::shared_ptr<TpchQ10State> order_metadata;
  core::Version order_version = 0;
  bool have_orders_partition = false;
  for (const auto &image : order_snapshot.partitions) {
    if (image.partition == core::TpchQ10Layout::kOrderMetadata) {
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Orders);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
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
  // Same-txn RF1 inserts are visible to LookupOrders after the base write.
  std::unordered_map<int64_t, TpchQ10Order> looked_up_orders;
  // Overlay same-txn order inserts when resolving without a full map, or when
  // the partition exists but inserts arrived in this buffer.
  std::unordered_map<int64_t, TpchQ10Order> overlay_orders;
  const std::unordered_map<int64_t, TpchQ10Order> *orders_by_key = nullptr;
  if (have_orders_partition && order_metadata) {
    orders_by_key = &order_metadata->orders;
  } else if (!buffer.Deltas().empty()) {
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign > 0)
        overlay_orders[delta.orderkey] =
            TpchQ10Order{delta.custkey, delta.orderdate_days};
    }
    std::vector<int64_t> orderkeys;
    orderkeys.reserve(buffer.Deltas().size());
    for (const auto &delta : buffer.Deltas()) {
      if (overlay_orders.find(delta.orderkey) == overlay_orders.end())
        orderkeys.push_back(delta.orderkey);
    }
    if (!orderkeys.empty()) {
      if (!initial_scan ||
          !initial_scan->LookupOrders(orderkeys, looked_up_orders)) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
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
  if (!buffer.Deltas().empty()) {
    if (!orders_by_key) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    for (const auto &delta : buffer.Deltas()) {
      const auto order_it = orders_by_key->find(delta.orderkey);
      if (order_it == orders_by_key->end()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      add_month_from_days(order_it->second.orderdate_days);
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
            })) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    for (const auto month : rebuild_months) {
      TpchQ10State month_state;
      const auto month_it = state.month_revenue.find(month);
      if (month_it != state.month_revenue.end())
        month_state.month_revenue.emplace(month, month_it->second);
      const auto typed_state =
          std::make_shared<const TpchQ10State>(std::move(month_state));
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, MonthPartition(month), 0,
          MakeStateImage(TpchQ10ImageKind::Month, month, typed_state)));
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
    core::FinishRebuildPartition(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (!buffer.Deltas().empty()) {
    std::set<int32_t> touched_months;
    for (const auto &delta : buffer.Deltas()) {
      const auto order_it = orders_by_key->find(delta.orderkey);
      if (order_it == orders_by_key->end()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      const auto month = TpchQ10MonthKeyFromDays(order_it->second.orderdate_days);
      touched_months.insert(month);
    }

    std::vector<core::PartitionId> month_partitions;
    month_partitions.reserve(touched_months.size());
    for (const auto month : touched_months) {
      month_partitions.push_back(MonthPartition(month));
    }
    std::map<int32_t, core::Version> versions;
    const auto month_snapshot =
        services.StateStore().ReadPartitions(visibility, month_partitions);
    for (const auto &image : month_snapshot.partitions) {
      if (image.partition < core::TpchQ10Layout::kFirstMonth) {
        continue;
      }
      const auto month = static_cast<int32_t>(
          image.partition - core::TpchQ10Layout::kFirstMonth);
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Month);
      if (!typed || typed->month != month) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      const auto month_it = typed->state->month_revenue.find(month);
      if (month_it != typed->state->month_revenue.end())
        state.month_revenue[month] = month_it->second;
      versions[month] = image.version;
    }
    if (versions.size() != touched_months.size()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }

    for (const auto &delta : buffer.Deltas()) {
      const auto order_it = orders_by_key->find(delta.orderkey);
      if (order_it == orders_by_key->end()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      const auto month = TpchQ10MonthKeyFromDays(order_it->second.orderdate_days);
      state.month_revenue[month][order_it->second.custkey] +=
          static_cast<int64_t>(delta.sign) * delta.revenue_raw;
      if (state.month_revenue[month][order_it->second.custkey] == 0)
        state.month_revenue[month].erase(order_it->second.custkey);
    }
    outcome.action = core::MaintenanceAction::ApplyDelta;
    for (const auto month : touched_months) {
      const auto partition = MonthPartition(month);
      const auto version = versions.count(month) ? versions[month] : 0;
      TpchQ10State month_state;
      const auto month_it = state.month_revenue.find(month);
      if (month_it != state.month_revenue.end())
        month_state.month_revenue.emplace(month, month_it->second);
      const auto typed_state = std::make_shared<const TpchQ10State>(month_state);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ10Layout::kAccelerator, partition, version,
          MakeStateImage(TpchQ10ImageKind::Month, month, typed_state)));
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
  std::vector<const std::unordered_map<int64_t, int64_t> *> month_revenues;
  month_revenues.reserve(3);
  // Hold shared ownership for the duration of finalize.
  std::vector<std::shared_ptr<const TpchQ10State>> month_holders;
  month_holders.reserve(3);
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
      const auto month_it = typed->state->month_revenue.find(month);
      if (month_it != typed->state->month_revenue.end())
        month_revenues.push_back(&month_it->second);
    }
  }
  if (!side) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  TpchQ10FinalizeAndEmit(side->customer_ranks, side->nations, month_revenues,
                         *side->customers, start, sink, kTpchQ10ResultLimit);
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
  auto customers = std::make_shared<TpchQ10CustomerMap>();
  initial_scan->ScanCustomers([&](const TpchQ10CustomerInput &input) {
    state.SetCustomerRank(input.key, input.acctbal_raw, input.nationkey);
    (*customers)[input.key] = {input.name,  input.acctbal_raw, input.nationkey,
                               input.address, input.phone,     input.comment};
  });
  state.customers = std::move(customers);
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

bool TpchQ10Module::LoadState(
    core::AuxEngineServices &services, TpchQ10State &state, int32_t start_days,
    std::map<int32_t, core::Version> *versions) const {
  // Query execution never needs the order lookup table; that partition is
  // consumed only by write-side maintenance. Also load only the three month
  // partitions addressed by this invocation. The cache is maintained state,
  // not final query output, and its window is part of its key.
  std::vector<core::PartitionId> partitions = {
      core::TpchQ10Layout::kCustomerDimensions};
  const auto end_days = TpchQ10AddMonths(start_days, 3);
  for (int32_t cursor = start_days; cursor < end_days;) {
    const auto month = TpchQ10MonthKeyFromDays(cursor);
    partitions.push_back(MonthPartition(month));
    const auto next = TpchQ10AddMonths(TpchQ10MonthStartDays(month), 1);
    if (next <= cursor) {
      return false;
    }
    cursor = next;
  }
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  bool side = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ10Layout::kCustomerDimensions) {
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Side);
      if (typed) {
        state.customer_ranks = typed->state->customer_ranks;
        state.customers = typed->state->customers;
        state.nations = typed->state->nations;
        side = true;
      }
    }
    else if (image.partition >= core::TpchQ10Layout::kFirstMonth) {
      const auto month = static_cast<int32_t>(image.partition -
                                              core::TpchQ10Layout::kFirstMonth);
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Month);
      if (!typed || typed->month != month)
        return false;
      const auto month_it = typed->state->month_revenue.find(month);
      if (month_it != typed->state->month_revenue.end())
        state.month_revenue[month] = month_it->second;
      if (versions)
        (*versions)[month] = image.version;
    }
  }
  return side;
}

void TpchQ10Module::EmitResult(const TpchQ10State &state, int32_t start_days,
                               core::AuxRowSink &sink) const {
  const auto end_days = TpchQ10AddMonths(start_days, 3);
  std::vector<const std::unordered_map<int64_t, int64_t> *> month_revenues;
  for (int32_t cursor = start_days; cursor < end_days;) {
    const auto month = TpchQ10MonthKeyFromDays(cursor);
    const auto it = state.month_revenue.find(month);
    if (it != state.month_revenue.end())
      month_revenues.push_back(&it->second);
    const auto next = TpchQ10AddMonths(TpchQ10MonthStartDays(month), 1);
    if (next <= cursor)
      break;
    cursor = next;
  }
  TpchQ10FinalizeAndEmit(state.customer_ranks, state.nations, month_revenues,
                         *state.customers, start_days, sink, kTpchQ10ResultLimit);
}

std::vector<core::CalibrationWrite> TpchQ10Module::CalibrationWrites() const {
  return TpchQ10CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
