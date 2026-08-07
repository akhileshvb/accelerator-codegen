#include "accelerator_codegen/aux_state/query/tpch/tpch_q10_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kSideMagic = 0x53303151;   // Q10S
constexpr uint32_t kOrdersMagic = 0x4f303151; // Q10O
constexpr uint32_t kMonthMagic = 0x4d303151;  // Q10M
constexpr int32_t kEpoch = 719468;

struct Writer {
  std::vector<uint8_t> bytes;
  void U32(uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
      bytes.push_back(static_cast<uint8_t>(value >> shift));
  }
  void U64(uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
      bytes.push_back(static_cast<uint8_t>(value >> shift));
  }
  void I64(int64_t value) { U64(static_cast<uint64_t>(value)); }
  void I32(int32_t value) { U32(static_cast<uint32_t>(value)); }
  void Double(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    U64(bits);
  }
  void String(const std::string &value) {
    U32(static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
};

struct Reader {
  const std::vector<uint8_t> &bytes;
  std::size_t offset = 0;
  bool CanRead(std::size_t count) const {
    return offset <= bytes.size() && count <= bytes.size() - offset;
  }
  bool U32(uint32_t &value) {
    if (!CanRead(4))
      return false;
    value = static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return true;
  }
  bool U64(uint64_t &value) {
    if (!CanRead(8))
      return false;
    value = 0;
    for (int shift = 0; shift < 64; shift += 8)
      value |= static_cast<uint64_t>(bytes[offset++]) << shift;
    return true;
  }
  bool I64(int64_t &value) {
    uint64_t bits = 0;
    if (!U64(bits))
      return false;
    value = static_cast<int64_t>(bits);
    return true;
  }
  bool I32(int32_t &value) {
    uint32_t bits = 0;
    if (!U32(bits))
      return false;
    value = static_cast<int32_t>(bits);
    return true;
  }
  bool Double(double &value) {
    uint64_t bits = 0;
    if (!U64(bits))
      return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }
  bool String(std::string &value) {
    uint32_t length = 0;
    if (!U32(length) || !CanRead(length))
      return false;
    value.assign(reinterpret_cast<const char *>(bytes.data() + offset), length);
    offset += length;
    return true;
  }
};

void Begin(Writer &writer, uint32_t magic, uint32_t count) {
  writer.U32(magic);
  writer.U32(kVersion);
  writer.U32(count);
}
bool Begin(Reader &reader, uint32_t magic, uint32_t &count) {
  uint32_t actual = 0, version = 0;
  return reader.U32(actual) && reader.U32(version) && reader.U32(count) &&
         actual == magic && version == kVersion;
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

uint32_t MonthPartition(int32_t month) {
  return static_cast<uint32_t>(core::TpchQ10Layout::kFirstMonth +
                               static_cast<uint64_t>(month));
}

} // namespace

std::vector<uint8_t> EncodeTpchQ10Side(const TpchQ10State &state) {
  Writer writer;
  Begin(writer, kSideMagic, static_cast<uint32_t>(state.customers.size()));
  std::vector<int64_t> keys;
  for (const auto &entry : state.customers)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  for (const auto key : keys) {
    const auto &customer = state.customers.at(key);
    writer.I64(key);
    writer.String(customer.name);
    writer.I64(customer.acctbal_raw);
    writer.I32(customer.nationkey);
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
  Writer writer;
  Begin(writer, kOrdersMagic, static_cast<uint32_t>(state.orders.size()));
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
  Writer writer;
  Begin(writer, kMonthMagic, static_cast<uint32_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.I64(it->second.at(key));
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ10Side(const std::vector<uint8_t> &bytes, TpchQ10State &state) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kSideMagic, count))
    return false;
  state.customers.clear();
  state.nations.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    TpchQ10Customer customer;
    if (!reader.I64(key) || !reader.String(customer.name) ||
        !reader.I64(customer.acctbal_raw) || !reader.I32(customer.nationkey) ||
        !reader.String(customer.address) || !reader.String(customer.phone) ||
        !reader.String(customer.comment))
      return false;
    state.customers.emplace(key, std::move(customer));
  }
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
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kOrdersMagic, count))
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
  Reader header{bytes};
  uint32_t count = 0;
  if (!Begin(header, kOrdersMagic, count)) {
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
    Reader reader{bytes};
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
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kMonthMagic, count))
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
}
bool TpchQ10TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ10LineitemDelta> &TpchQ10TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ10TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ10TxnBuffer::AddDelta(TpchQ10LineitemDelta delta) {
  pending = true;
  if (delta.revenue_raw != 0)
    deltas.push_back(delta);
}

core::AcceleratorId TpchQ10Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ10Module::Dependencies() const {
  return {{config.customer_table, {0, 1, 2, 3, 4, 5, 6}},
          {config.nation_table, {0, 1}},
          {config.orders_table, {0, 1, 2}},
          {config.lineitem_table, {0, 1, 2, 3}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ10Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ10TxnBuffer>();
}
void TpchQ10Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ10State>(state);
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator,
      core::TpchQ10Layout::kCustomerDimensions, 0,
      MakeStateImage(TpchQ10ImageKind::Side, 0, typed_state)));
  builder.Write(core::StateImage::Make(
      core::TpchQ10Layout::kAccelerator,
      core::TpchQ10Layout::kOrderMetadata, 0,
      MakeStateImage(TpchQ10ImageKind::Orders, 0, typed_state)));
  for (const auto &entry : state.month_revenue) {
    builder.Write(core::StateImage::Make(
        core::TpchQ10Layout::kAccelerator, MonthPartition(entry.first), 0,
        MakeStateImage(TpchQ10ImageKind::Month, entry.first, typed_state)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ10Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.customer_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.orders_table)
    return All();
  return {};
}

void TpchQ10Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ10TxnBuffer &>(base_buffer);
  if (mutation.table != config.lineitem_table) {
    buffer.MarkRebuild();
    return;
  }
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

  TpchQ10State state;
  const auto visibility = services.CurrentVisibility();
  const auto order_snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ10Layout::kOrderMetadata});
  const TpchQ10State *order_metadata = nullptr;
  for (const auto &image : order_snapshot.partitions) {
    if (image.partition == core::TpchQ10Layout::kOrderMetadata) {
      const auto *typed = AsQ10Image(image, TpchQ10ImageKind::Orders);
      order_metadata = typed ? typed->state.get() : nullptr;
      break;
    }
  }
  if (!order_metadata) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  std::set<int32_t> touched_months;
  for (const auto &delta : buffer.Deltas()) {
    const auto order_it = order_metadata->orders.find(delta.orderkey);
    if (order_it == order_metadata->orders.end()) {
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
    const auto order_it = order_metadata->orders.find(delta.orderkey);
    if (order_it == order_metadata->orders.end()) {
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
  TpchQ10State state;
  if (!LoadState(services, state, start)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(state, start, sink);
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
  initial_scan->ScanCustomers([&](const TpchQ10CustomerInput &input) {
    state.customers[input.key] = {input.name, input.acctbal_raw,
                                  input.nationkey, input.address, input.phone,
                                  input.comment};
  });
  initial_scan->ScanNations([&](const TpchQ10NationInput &input) {
    state.nations[input.key] = input.name;
  });
  initial_scan->ScanOrders([&](const TpchQ10OrderInput &input) {
    state.orders[input.key] = {input.custkey, input.orderdate_days};
  });
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
  std::unordered_map<int64_t, int64_t> totals;
  for (int32_t cursor = start_days; cursor < end_days;) {
    const auto month = TpchQ10MonthKeyFromDays(cursor);
    const auto it = state.month_revenue.find(month);
    if (it != state.month_revenue.end())
      for (const auto &entry : it->second)
        totals[entry.first] += entry.second;
    const auto next = TpchQ10AddMonths(TpchQ10MonthStartDays(month), 1);
    if (next <= cursor)
      break;
    cursor = next;
  }
  std::vector<int64_t> customers;
  for (const auto &entry : totals)
    if (entry.second != 0 && state.customers.count(entry.first))
      customers.push_back(entry.first);
  std::sort(customers.begin(), customers.end(), [&](int64_t lhs, int64_t rhs) {
    if (totals[lhs] != totals[rhs])
      return totals[lhs] > totals[rhs];
    return lhs < rhs;
  });
  sink.Begin({{{"c_custkey", core::AuxType::Int64, false},
               {"c_name", core::AuxType::String, false},
               {"revenue", core::AuxType::ScaledDecimal, false},
               {"c_acctbal", core::AuxType::ScaledDecimal, false},
               {"n_name", core::AuxType::String, false},
               {"c_address", core::AuxType::String, false},
               {"c_phone", core::AuxType::String, false},
               {"c_comment", core::AuxType::String, false}}});
  for (const auto custkey : customers) {
    const auto &customer = state.customers.at(custkey);
    const auto nation = state.nations.find(customer.nationkey);
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, custkey),
          core::AuxValue::Of(core::AuxType::String, customer.name),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{totals.at(custkey), 4}),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{customer.acctbal_raw, 2}),
          core::AuxValue::Of(core::AuxType::String,
                             nation == state.nations.end() ? std::string()
                                                           : nation->second),
          core::AuxValue::Of(core::AuxType::String, customer.address),
          core::AuxValue::Of(core::AuxType::String, customer.phone),
          core::AuxValue::Of(core::AuxType::String, customer.comment)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
