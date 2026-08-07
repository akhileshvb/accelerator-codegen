#include "accelerator_codegen/aux_state/query/tpch/tpch_q3_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kCustomerMagic = 0x43335151;  // Q3C
constexpr uint32_t kOrderRouteMagic = 0x52335151; // Q3R
constexpr uint32_t kIntervalMagic = 0x49335151;   // Q3I
constexpr int32_t kEpoch = 719468;
constexpr std::size_t kTopK = 10;

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
  void U32Count(uint32_t value) { U32(value); }
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

std::shared_ptr<const TpchQ3StateImage>
MakeStateImage(TpchQ3ImageKind kind, core::PartitionId segment,
               const std::shared_ptr<const TpchQ3State> &state) {
  auto image = std::make_shared<TpchQ3StateImage>();
  image->kind = kind;
  image->segment = segment;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ3State>
MakeSegmentIntervalState(const TpchQ3SegmentIntervalIndex &index) {
  auto state = std::make_shared<TpchQ3State>();
  state->segment_intervals.emplace(index.segment, index);
  return state;
}

const TpchQ3StateImage *AsQ3Image(const core::StateImage &image,
                                  TpchQ3ImageKind kind) {
  const auto *payload = image.As<TpchQ3StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ3LineitemInput &out) {
  int64_t orderkey = 0;
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  if (!IntCell(row, 0, orderkey) ||
      !RawDecimalCell(row, 1, extended_raw, extended_scale) ||
      !RawDecimalCell(row, 2, discount_raw, discount_scale) ||
      !DateCell(row, 3, out.shipdate_days))
    return false;
  out.orderkey = orderkey;
  out.extended_price_raw = extended_raw;
  out.extended_price_scale = extended_scale;
  out.discount_raw = discount_raw;
  out.discount_scale = discount_scale;
  return true;
}

bool BetterRow(const TpchQ3OutputRow &lhs, const TpchQ3OutputRow &rhs) {
  if (lhs.revenue_raw != rhs.revenue_raw)
    return lhs.revenue_raw > rhs.revenue_raw;
  if (lhs.orderdate_days != rhs.orderdate_days)
    return lhs.orderdate_days < rhs.orderdate_days;
  if (lhs.orderkey != rhs.orderkey)
    return lhs.orderkey < rhs.orderkey;
  return lhs.shippriority < rhs.shippriority;
}

struct TopKLess {
  bool operator()(const TpchQ3OutputRow &lhs, const TpchQ3OutputRow &rhs) const {
    return BetterRow(lhs, rhs);
  }
};

std::map<int32_t, int64_t>
DayRevenueFromSummary(const TpchQ3OrderSummary &order) {
  std::map<int32_t, int64_t> day_revenue;
  for (std::size_t i = 0; i < order.sorted_shipdates.size(); ++i) {
    const int64_t revenue =
        order.suffix_revenue[i] - order.suffix_revenue[i + 1];
    if (revenue != 0)
      day_revenue.emplace(order.sorted_shipdates[i], revenue);
  }
  return day_revenue;
}

void ApplyLineitemDelta(TpchQ3OrderSummary &summary,
                        const TpchQ3LineitemDelta &delta) {
  auto day_revenue = DayRevenueFromSummary(summary);
  day_revenue[delta.shipdate_day] +=
      static_cast<int64_t>(delta.sign) * delta.revenue_raw;
  if (day_revenue[delta.shipdate_day] == 0)
    day_revenue.erase(delta.shipdate_day);
  FinalizeTpchQ3OrderSummary(summary, day_revenue);
}

struct OrderBuilder {
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
  core::PartitionId segment = 0;
  std::map<int32_t, int64_t> shipdate_revenue;
};

} // namespace

int32_t TpchQ3DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return DaysFromCivil(year, month, day);
}

int64_t TpchQ3RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
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

core::PartitionId TpchQ3PartitionFromMktsegment(const std::string &segment) {
  if (segment == "BUILDING")
    return core::TpchQ3Layout::kSegmentBuilding;
  if (segment == "FURNITURE")
    return core::TpchQ3Layout::kSegmentFurniture;
  if (segment == "HOUSEHOLD")
    return core::TpchQ3Layout::kSegmentHousehold;
  if (segment == "MACHINERY")
    return core::TpchQ3Layout::kSegmentMachinery;
  if (segment == "AUTOMOBILE")
    return core::TpchQ3Layout::kSegmentAutomobile;
  return 0;
}

std::string TpchQ3MktsegmentFromPartition(core::PartitionId partition) {
  switch (partition) {
  case core::TpchQ3Layout::kSegmentBuilding:
    return "BUILDING";
  case core::TpchQ3Layout::kSegmentFurniture:
    return "FURNITURE";
  case core::TpchQ3Layout::kSegmentHousehold:
    return "HOUSEHOLD";
  case core::TpchQ3Layout::kSegmentMachinery:
    return "MACHINERY";
  case core::TpchQ3Layout::kSegmentAutomobile:
    return "AUTOMOBILE";
  default:
    return {};
  }
}

void FinalizeTpchQ3OrderSummary(TpchQ3OrderSummary &summary,
                                const std::map<int32_t, int64_t> &day_revenue) {
  summary.sorted_shipdates.clear();
  summary.suffix_revenue.clear();
  for (const auto &entry : day_revenue) {
    if (entry.second != 0)
      summary.sorted_shipdates.push_back(entry.first);
  }
  if (summary.sorted_shipdates.empty()) {
    summary.max_shipdate = 0;
    return;
  }
  std::sort(summary.sorted_shipdates.begin(), summary.sorted_shipdates.end());
  summary.max_shipdate = summary.sorted_shipdates.back();
  std::vector<int64_t> revenues;
  revenues.reserve(summary.sorted_shipdates.size());
  for (const auto day : summary.sorted_shipdates)
    revenues.push_back(day_revenue.at(day));
  summary.suffix_revenue.resize(revenues.size() + 1);
  int64_t running = 0;
  for (int index = static_cast<int>(revenues.size()) - 1; index >= 0; --index) {
    running += revenues[static_cast<std::size_t>(index)];
    summary.suffix_revenue[static_cast<std::size_t>(index)] = running;
  }
  summary.suffix_revenue.back() = 0;
}

int64_t TpchQ3RevenueAboveCutoff(const TpchQ3OrderSummary &order,
                                 int32_t cutoff_days) {
  if (order.orderdate_days >= cutoff_days || order.max_shipdate <= cutoff_days)
    return 0;
  const auto it = std::upper_bound(order.sorted_shipdates.begin(),
                                   order.sorted_shipdates.end(), cutoff_days);
  const auto index =
      static_cast<std::size_t>(it - order.sorted_shipdates.begin());
  return order.suffix_revenue[index];
}

void SortTpchQ3SegmentIntervalIndex(TpchQ3SegmentIntervalIndex &index) {
  std::sort(index.orders.begin(), index.orders.end(),
            [](const TpchQ3OrderSummary &lhs, const TpchQ3OrderSummary &rhs) {
              if (lhs.max_shipdate != rhs.max_shipdate)
                return lhs.max_shipdate < rhs.max_shipdate;
              return lhs.orderkey < rhs.orderkey;
            });
  index.order_positions.clear();
  for (std::size_t i = 0; i < index.orders.size(); ++i)
    index.order_positions.emplace(index.orders[i].orderkey, i);
}

std::vector<TpchQ3OutputRow>
ComposeTpchQ3TopK(const TpchQ3SegmentIntervalIndex &index,
                   int32_t cutoff_days) {
  const auto begin = std::lower_bound(
      index.orders.begin(), index.orders.end(), cutoff_days,
      [](const TpchQ3OrderSummary &order, int32_t cutoff) {
        return order.max_shipdate <= cutoff;
      });
  std::priority_queue<TpchQ3OutputRow, std::vector<TpchQ3OutputRow>, TopKLess>
      heap;
  for (auto it = begin; it != index.orders.end(); ++it) {
    if (it->orderdate_days >= cutoff_days)
      continue;
    const auto revenue_raw = TpchQ3RevenueAboveCutoff(*it, cutoff_days);
    if (revenue_raw == 0)
      continue;
    TpchQ3OutputRow row;
    row.orderkey = it->orderkey;
    row.revenue_raw = revenue_raw;
    row.orderdate_days = it->orderdate_days;
    row.shippriority = it->shippriority;
    if (heap.size() < kTopK) {
      heap.push(row);
      continue;
    }
    if (BetterRow(row, heap.top())) {
      heap.pop();
      heap.push(row);
    }
  }
  std::vector<TpchQ3OutputRow> ordered;
  ordered.reserve(heap.size());
  while (!heap.empty()) {
    ordered.push_back(heap.top());
    heap.pop();
  }
  std::sort(ordered.begin(), ordered.end(), BetterRow);
  return ordered;
}

std::vector<uint8_t> EncodeTpchQ3CustomerSegments(const TpchQ3State &state) {
  Writer writer;
  std::vector<int64_t> keys;
  for (const auto &entry : state.customer_segments)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  Begin(writer, kCustomerMagic, static_cast<uint32_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.U32Count(static_cast<uint32_t>(state.customer_segments.at(key)));
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ3OrderRoutes(const TpchQ3State &state) {
  Writer writer;
  std::vector<int64_t> keys;
  for (const auto &entry : state.order_routes)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  Begin(writer, kOrderRouteMagic, static_cast<uint32_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.U32Count(static_cast<uint32_t>(state.order_routes.at(key)));
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ3SegmentIntervalIndex(
    const TpchQ3SegmentIntervalIndex &index) {
  Writer writer;
  Begin(writer, kIntervalMagic, static_cast<uint32_t>(index.orders.size()));
  for (const auto &order : index.orders) {
    writer.I64(order.orderkey);
    writer.I32(order.orderdate_days);
    writer.I32(order.shippriority);
    writer.I32(order.max_shipdate);
    writer.U32Count(static_cast<uint32_t>(order.sorted_shipdates.size()));
    for (std::size_t i = 0; i < order.sorted_shipdates.size(); ++i) {
      writer.I32(order.sorted_shipdates[i]);
      const int64_t revenue = order.suffix_revenue[i] - order.suffix_revenue[i + 1];
      writer.I64(revenue);
    }
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ3CustomerSegments(const std::vector<uint8_t> &bytes,
                                  TpchQ3State &state) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kCustomerMagic, count))
    return false;
  state.customer_segments.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint32_t segment = 0;
    if (!reader.I64(key) || !reader.U32(segment))
      return false;
    state.customer_segments.emplace(key, static_cast<core::PartitionId>(segment));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ3OrderRoutes(const std::vector<uint8_t> &bytes,
                             TpchQ3State &state) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kOrderRouteMagic, count))
    return false;
  state.order_routes.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint32_t segment = 0;
    if (!reader.I64(key) || !reader.U32(segment))
      return false;
    state.order_routes.emplace(key, static_cast<core::PartitionId>(segment));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ3SegmentIntervalIndex(const std::vector<uint8_t> &bytes,
                                        core::PartitionId segment,
                                        TpchQ3SegmentIntervalIndex &index) {
  Reader reader{bytes};
  uint32_t count = 0;
  if (!Begin(reader, kIntervalMagic, count))
    return false;
  index.segment = segment;
  index.orders.clear();
  index.order_positions.clear();
  index.orders.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ3OrderSummary order;
    uint32_t bucket_count = 0;
    if (!reader.I64(order.orderkey) || !reader.I32(order.orderdate_days) ||
        !reader.I32(order.shippriority) || !reader.I32(order.max_shipdate) ||
        !reader.U32(bucket_count))
      return false;
    std::map<int32_t, int64_t> day_revenue;
    for (uint32_t b = 0; b < bucket_count; ++b) {
      int32_t day = 0;
      int64_t revenue = 0;
      if (!reader.I32(day) || !reader.I64(revenue))
        return false;
      if (revenue != 0)
        day_revenue.emplace(day, revenue);
    }
    FinalizeTpchQ3OrderSummary(order, day_revenue);
    index.orders.push_back(std::move(order));
  }
  SortTpchQ3SegmentIntervalIndex(index);
  return reader.offset == bytes.size();
}

bool TpchQ3TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ3TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ3TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ3LineitemDelta> &TpchQ3TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ3TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ3TxnBuffer::AddDelta(TpchQ3LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ3Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ3Module::Dependencies() const {
  return {{config.customer_table, {0, 1}},
          {config.orders_table, {0, 1, 2, 3}},
          {config.lineitem_table, {0, 1, 2, 3}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ3Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ3TxnBuffer>();
}

void TpchQ3Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ3State>(state);
  builder.Write(core::StateImage::Make(
      core::TpchQ3Layout::kAccelerator,
      core::TpchQ3Layout::kCustomerSegments, 0,
      MakeStateImage(TpchQ3ImageKind::CustomerSegments, 0, typed_state)));
  builder.Write(core::StateImage::Make(
      core::TpchQ3Layout::kAccelerator, core::TpchQ3Layout::kOrderRoutes, 0,
      MakeStateImage(TpchQ3ImageKind::OrderRoutes, 0, typed_state)));
  for (const auto &entry : state.segment_intervals) {
    builder.Write(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, entry.first, 0,
        MakeStateImage(TpchQ3ImageKind::SegmentIntervalIndex, entry.first,
                       typed_state)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ3Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.customer_table ||
      mutation.table == config.orders_table)
    return All();
  return {};
}

void TpchQ3Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ3TxnBuffer &>(base_buffer);
  if (mutation.table != config.lineitem_table)
    return;
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ3LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    TpchQ3LineitemDelta delta;
    delta.orderkey = input.orderkey;
    delta.shipdate_day = input.shipdate_days;
    delta.revenue_raw =
        TpchQ3RevenueRaw(input.extended_price_raw, input.extended_price_scale,
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
TpchQ3Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ3TxnBuffer &>(base_buffer);
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

  const auto visibility = services.CurrentVisibility();
  const std::unordered_map<int64_t, core::PartitionId> *order_routes = nullptr;
  {
    const auto snapshot = services.StateStore().ReadPartitions(
        visibility, {core::TpchQ3Layout::kOrderRoutes});
    for (const auto &image : snapshot.partitions) {
      if (image.partition != core::TpchQ3Layout::kOrderRoutes)
        continue;
      const auto *typed = AsQ3Image(image, TpchQ3ImageKind::OrderRoutes);
      if (!typed)
        return outcome;
      order_routes = &typed->state->order_routes;
      break;
    }
    if (!order_routes || order_routes->empty()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }

  std::map<core::PartitionId, std::vector<TpchQ3LineitemDelta>> grouped;
  for (auto delta : buffer.Deltas()) {
    const auto route_it = order_routes->find(delta.orderkey);
    if (route_it == order_routes->end()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    delta.segment = route_it->second;
    grouped[delta.segment].push_back(delta);
  }

  for (const auto &group : grouped) {
    const auto segment = group.first;
    const auto snapshot =
        services.StateStore().ReadPartitions(visibility, {segment});
    TpchQ3SegmentIntervalIndex index;
    core::Version version = 0;
    bool found = false;
    for (const auto &image : snapshot.partitions) {
      if (image.partition != segment)
        continue;
      const auto *typed =
          AsQ3Image(image, TpchQ3ImageKind::SegmentIntervalIndex);
      if (!typed || typed->segment != segment) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      const auto it = typed->state->segment_intervals.find(segment);
      if (it != typed->state->segment_intervals.end())
        index = it->second;
      version = image.version;
      found = true;
      break;
    }
    if (!found) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }

    bool needs_resort = false;
    for (const auto &delta : group.second) {
      const auto position_it = index.order_positions.find(delta.orderkey);
      if (position_it == index.order_positions.end()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      auto &summary = index.orders[position_it->second];
      const auto old_max = summary.max_shipdate;
      ApplyLineitemDelta(summary, delta);
      if (summary.sorted_shipdates.empty()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      if (summary.max_shipdate != old_max)
        needs_resort = true;
    }
    if (needs_resort)
      SortTpchQ3SegmentIntervalIndex(index);

    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, segment, version,
        MakeStateImage(TpchQ3ImageKind::SegmentIntervalIndex, segment,
                       MakeSegmentIntervalState(index))));
  }
  return outcome;
}

bool TpchQ3Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ3Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  std::string segment;
  int32_t cutoff = 0;
  if (arguments.values.size() < 2)
    return ExecuteFallback(services, arguments, sink);
  if (!arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    segment = std::get<std::string>(arguments.values[0].payload);
  if (!arguments.values[1].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[1].payload))
      cutoff =
          std::get<core::AuxDate>(arguments.values[1].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[1].payload))
      cutoff = static_cast<int32_t>(std::get<int64_t>(arguments.values[1].payload));
  }
  const auto partition = TpchQ3PartitionFromMktsegment(segment);
  if (partition == 0) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const TpchQ3SegmentIntervalIndex *interval_index = nullptr;
  if (!LoadSegmentIntervalIndex(services, partition, interval_index) ||
      interval_index == nullptr) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(ComposeTpchQ3TopK(*interval_index, cutoff), sink);
}

void TpchQ3Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  std::string segment;
  int32_t cutoff = 0;
  if (arguments.values.size() >= 1 && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    segment = std::get<std::string>(arguments.values[0].payload);
  if (arguments.values.size() >= 2 && !arguments.values[1].is_null &&
      std::holds_alternative<core::AuxDate>(arguments.values[1].payload))
    cutoff =
        std::get<core::AuxDate>(arguments.values[1].payload).days_since_epoch;
  const auto state = BuildState(services);
  const auto partition = TpchQ3PartitionFromMktsegment(segment);
  const auto it = state.segment_intervals.find(partition);
  EmitResult(ComposeTpchQ3TopK(
                 it != state.segment_intervals.end()
                     ? it->second
                     : TpchQ3SegmentIntervalIndex{partition, {}, {}},
                 cutoff),
             sink);
}

TpchQ3State TpchQ3Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ3State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");
  initial_scan->ScanCustomers([&](const TpchQ3CustomerInput &input) {
    const auto partition = TpchQ3PartitionFromMktsegment(input.mktsegment);
    if (partition != 0)
      state.customer_segments[input.key] = partition;
  });

  std::unordered_map<int64_t, OrderBuilder> builders;
  initial_scan->ScanOrders([&](const TpchQ3OrderInput &input) {
    const auto segment_it = state.customer_segments.find(input.custkey);
    if (segment_it == state.customer_segments.end())
      return;
    state.order_routes[input.key] = segment_it->second;
    auto &builder = builders[input.key];
    builder.orderdate_days = input.orderdate_days;
    builder.shippriority = input.shippriority;
    builder.segment = segment_it->second;
  });
  initial_scan->ScanLineitems([&](const TpchQ3LineitemInput &input) {
    const auto route_it = state.order_routes.find(input.orderkey);
    if (route_it == state.order_routes.end())
      return;
    auto &builder = builders[input.orderkey];
    if (builder.segment == 0) {
      builder.segment = route_it->second;
      const auto order_it = builders.find(input.orderkey);
      if (order_it == builders.end())
        return;
    }
    const auto revenue =
        TpchQ3RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                         input.discount_raw, input.discount_scale);
    builder.shipdate_revenue[input.shipdate_days] += revenue;
  });

  for (auto &entry : builders) {
    if (entry.second.shipdate_revenue.empty())
      continue;
    TpchQ3OrderSummary summary;
    summary.orderkey = entry.first;
    summary.orderdate_days = entry.second.orderdate_days;
    summary.shippriority = entry.second.shippriority;
    FinalizeTpchQ3OrderSummary(summary, entry.second.shipdate_revenue);
    auto &index = state.segment_intervals[entry.second.segment];
    index.segment = entry.second.segment;
    index.orders.push_back(std::move(summary));
  }
  for (auto &entry : state.segment_intervals)
    SortTpchQ3SegmentIntervalIndex(entry.second);
  return state;
}

bool TpchQ3Module::LoadSegmentIntervalIndex(
    core::AuxEngineServices &services, core::PartitionId segment,
    const TpchQ3SegmentIntervalIndex *&interval_index) const {
  interval_index = nullptr;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {segment});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != segment)
      continue;
    const auto *typed =
        AsQ3Image(image, TpchQ3ImageKind::SegmentIntervalIndex);
    if (!typed || typed->segment != segment)
      return false;
    const auto it = typed->state->segment_intervals.find(segment);
    if (it == typed->state->segment_intervals.end())
      return false;
    interval_index = &it->second;
    return true;
  }
  return false;
}

void TpchQ3Module::EmitResult(const std::vector<TpchQ3OutputRow> &rows,
                              core::AuxRowSink &sink) const {
  sink.Begin({{{"l_orderkey", core::AuxType::Int64, false},
               {"revenue", core::AuxType::ScaledDecimal, false},
               {"o_orderdate", core::AuxType::Date, false},
               {"o_shippriority", core::AuxType::Int64, false}}});
  for (const auto &row : rows) {
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::Int64, row.orderkey),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{row.revenue_raw, 4}),
          core::AuxValue::Of(core::AuxType::Date,
                             core::AuxDate{row.orderdate_days}),
          core::AuxValue::Of(core::AuxType::Int64, row.shippriority)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
