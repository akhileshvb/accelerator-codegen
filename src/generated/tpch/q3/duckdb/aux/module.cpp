#include "tpch/q3/duckdb/aux/module.hpp"
#include "tpch/q3/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_set>
#include <stdexcept>
#include <utility>
#include "tpch/q3/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kOrderSegmentMagic = 0x53335151; // Q3S
constexpr uint32_t kIntervalMagic = 0x49335151;     // Q3I
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

std::shared_ptr<const TpchQ3StateImage>
MakeSegmentIntervalImage(core::PartitionId segment,
                         const std::shared_ptr<const TpchQ3State> &state) {
  auto image = std::make_shared<TpchQ3StateImage>();
  image->kind = TpchQ3ImageKind::SegmentIntervalIndex;
  image->segment = segment;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ3StateImage> MakeOrderSegmentSideImage(
    const std::shared_ptr<const TpchQ3OrderSegmentSide> &side) {
  auto image = std::make_shared<TpchQ3StateImage>();
  image->kind = TpchQ3ImageKind::OrderSegmentSide;
  image->order_segment_side = side;
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
  if (!payload || payload->kind != kind)
    return nullptr;
  if (kind == TpchQ3ImageKind::OrderSegmentSide &&
      !payload->order_segment_side)
    return nullptr;
  if (kind == TpchQ3ImageKind::SegmentIntervalIndex && !payload->state)
    return nullptr;
  return payload;
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

// Matches ScanOrders: o_orderkey, o_orderdate, o_custkey, o_shippriority
bool ReadOrder(const core::AuxRow &row, int64_t &orderkey,
               int32_t &orderdate_days, int64_t &custkey,
               int32_t &shippriority) {
  int64_t shippriority_raw = 0;
  if (!IntCell(row, 0, orderkey) || !DateCell(row, 1, orderdate_days) ||
      !IntCell(row, 2, custkey) || !IntCell(row, 3, shippriority_raw))
    return false;
  if (shippriority_raw < std::numeric_limits<int32_t>::min() ||
      shippriority_raw > std::numeric_limits<int32_t>::max())
    return false;
  shippriority = static_cast<int32_t>(shippriority_raw);
  return true;
}

std::map<int32_t, int64_t>
DayRevenueFromSummary(const TpchQ3SegmentIntervalIndex &index,
                      const TpchQ3OrderSummary &order) {
  std::map<int32_t, int64_t> day_revenue;
  const auto begin = static_cast<std::size_t>(order.bucket_begin);
  for (std::size_t i = 0; i < order.bucket_count; ++i) {
    const auto &bucket = index.bucket_pool[begin + i];
    const int64_t next_suffix = (i + 1 < order.bucket_count)
                                    ? index.bucket_pool[begin + i + 1].suffix_revenue
                                    : 0;
    const int64_t revenue = bucket.suffix_revenue - next_suffix;
    if (revenue != 0)
      day_revenue.emplace(static_cast<int32_t>(bucket.shipdate), revenue);
  }
  return day_revenue;
}

void ApplyLineitemDelta(TpchQ3SegmentIntervalIndex &index,
                        TpchQ3OrderSummary &summary,
                        const TpchQ3LineitemDelta &delta) {
  auto day_revenue = DayRevenueFromSummary(index, summary);
  day_revenue[delta.shipdate_day] +=
      static_cast<int64_t>(delta.sign) * delta.revenue_raw;
  if (day_revenue[delta.shipdate_day] == 0)
    day_revenue.erase(delta.shipdate_day);
  std::vector<TpchQ3ShipdateBucket> buckets;
  FinalizeTpchQ3OrderSummary(buckets, day_revenue);
  index.AssignBuckets(summary, std::move(buckets));
}

void EnsureOrderSegmentSlot(std::vector<uint8_t> &order_segment,
                            int64_t orderkey) {
  if (orderkey < 0)
    return;
  const auto index = static_cast<std::size_t>(orderkey);
  if (index >= order_segment.size())
    order_segment.resize(index + 1, 0);
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

void SortTpchQ3SegmentIntervalIndex(TpchQ3SegmentIntervalIndex &index) {
  std::sort(index.orders.begin(), index.orders.end(),
            [&index](const TpchQ3OrderSummary &lhs, const TpchQ3OrderSummary &rhs) {
              const auto lhs_max = index.MaxShipdate(lhs);
              const auto rhs_max = index.MaxShipdate(rhs);
              if (lhs_max != rhs_max)
                return lhs_max < rhs_max;
              return lhs.orderkey < rhs.orderkey;
            });
  index.ClearPositions();
  for (std::size_t i = 0; i < index.orders.size(); ++i)
    index.SetPosition(static_cast<int64_t>(index.orders[i].orderkey), i);
}

void RemoveOrderFromIndex(TpchQ3SegmentIntervalIndex &index, int64_t orderkey) {
  std::size_t pos = 0;
  if (!index.FindPosition(orderkey, pos))
    return;
  index.orders.erase(index.orders.begin() + static_cast<std::ptrdiff_t>(pos));
  index.ErasePosition(orderkey);
  for (std::size_t i = pos; i < index.orders.size(); ++i)
    index.SetPosition(static_cast<int64_t>(index.orders[i].orderkey), i);
}

std::vector<uint8_t>
EncodeTpchQ3OrderSegmentSide(const TpchQ3OrderSegmentSide &side) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kOrderSegmentMagic, kVersion,
                         static_cast<uint32_t>(side.order_segment.size()));
  for (const auto segment : side.order_segment)
    writer.U8(segment);
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ3SegmentIntervalIndex(
    const TpchQ3SegmentIntervalIndex &index) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kIntervalMagic, kVersion,
                         static_cast<uint32_t>(index.orders.size()));
  for (const auto &order : index.orders) {
    writer.I64(order.orderkey);
    writer.I32(order.orderdate_days);
    writer.I32(order.shippriority);
    writer.I32(index.MaxShipdate(order));
    writer.U32(order.bucket_count);
    const auto begin = static_cast<std::size_t>(order.bucket_begin);
    for (uint32_t b = 0; b < order.bucket_count; ++b) {
      const auto &bucket = index.bucket_pool[begin + b];
      writer.I32(bucket.shipdate);
      writer.I64(bucket.suffix_revenue);
    }
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ3OrderSegmentSide(const std::vector<uint8_t> &bytes,
                                  TpchQ3OrderSegmentSide &side) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kOrderSegmentMagic, kVersion, count))
    return false;
  side.order_segment.clear();
  side.order_segment.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t segment = 0;
    if (!reader.U8(segment))
      return false;
    side.order_segment.push_back(segment);
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ3SegmentIntervalIndex(const std::vector<uint8_t> &bytes,
                                      core::PartitionId segment,
                                      TpchQ3SegmentIntervalIndex &index) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kIntervalMagic, kVersion, count))
    return false;
  index.segment = segment;
  index.orders.clear();
  index.bucket_pool.clear();
  index.ClearPositions();
  index.orders.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ3OrderSummary order;
    uint32_t bucket_count = 0;
    int64_t orderkey = 0;
    int32_t orderdate_days = 0;
    int32_t shippriority = 0;
    int32_t max_shipdate = 0;
    if (!reader.I64(orderkey) || !reader.I32(orderdate_days) ||
        !reader.I32(shippriority) || !reader.I32(max_shipdate) ||
        !reader.U32(bucket_count))
      return false;
    (void)max_shipdate;
    order.orderkey = PackTpchQ3Orderkey(orderkey);
    order.orderdate_days = PackTpchQ3Date(orderdate_days);
    order.shippriority = PackTpchQ3Shippriority(shippriority);
    std::vector<TpchQ3ShipdateBucket> buckets;
    buckets.reserve(bucket_count);
    for (uint32_t b = 0; b < bucket_count; ++b) {
      int32_t shipdate = 0;
      TpchQ3ShipdateBucket bucket;
      if (!reader.I32(shipdate) || !reader.I64(bucket.suffix_revenue))
        return false;
      bucket.shipdate = PackTpchQ3Date(shipdate);
      buckets.push_back(bucket);
    }
    index.AssignBuckets(order, std::move(buckets));
    index.orders.push_back(order);
  }
  SortTpchQ3SegmentIntervalIndex(index);
  return reader.offset == bytes.size();
}

bool TpchQ3TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ3TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
}
bool TpchQ3TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ3LineitemDelta> &TpchQ3TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ3OrderDelta> &TpchQ3TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ3TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
}
void TpchQ3TxnBuffer::AddDelta(TpchQ3LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ3TxnBuffer::AddOrderDelta(TpchQ3OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}

core::AcceleratorId TpchQ3Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ3Module::Dependencies() const {
  return {{config.customer_table, {0, 1}, {6, 0}},
          {config.orders_table, {0, 1, 2, 3}, {0, 4, 1, 7}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 5, 6, 10}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ3Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ3TxnBuffer>();
}

void TpchQ3Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  BuildScratch scratch;
  const auto state = BuildState(services, scratch);
  auto side = std::make_shared<TpchQ3OrderSegmentSide>();
  side->order_segment = std::move(scratch.order_segment);
  for (const auto &[custkey, segment] : scratch.customer_segments) {
    if (custkey < 0)
      continue;
    const auto index = static_cast<std::size_t>(custkey);
    if (index >= side->customer_segment.size())
      side->customer_segment.resize(index + 1, 0);
    side->customer_segment[index] = static_cast<uint8_t>(segment);
  }
  builder.Write(core::StateImage::Make(
      core::TpchQ3Layout::kAccelerator, core::TpchQ3Layout::kOrderSegmentSide, 0,
      MakeOrderSegmentSideImage(side)));
  for (const auto &entry : state.segment_intervals) {
    builder.Write(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, entry.first, 0,
        MakeSegmentIntervalImage(entry.first,
                                 MakeSegmentIntervalState(entry.second))));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ3Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.customer_table)
    return All();
  return {};
}

void TpchQ3Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ3TxnBuffer &>(base_buffer);
  if (mutation.table == config.customer_table) {
    buffer.MarkRebuild();
    return;
  }
  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0, custkey = 0;
      int32_t orderdate = 0, shippriority = 0;
      if (!ReadOrder(row, orderkey, orderdate, custkey, shippriority)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta(
          {orderkey, orderdate, custkey, shippriority, sign});
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
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty())
    return outcome;

  const auto total_deltas =
      buffer.Deltas().size() + buffer.OrderDeltas().size();

  const auto visibility = services.CurrentVisibility();
  std::shared_ptr<TpchQ3OrderSegmentSide> order_side;
  core::Version side_version = 0;
  {
    const auto snapshot = services.StateStore().ReadPartitions(
        visibility, {core::TpchQ3Layout::kOrderSegmentSide});
    for (const auto &image : snapshot.partitions) {
      if (image.partition != core::TpchQ3Layout::kOrderSegmentSide)
        continue;
      const auto *typed = AsQ3Image(image, TpchQ3ImageKind::OrderSegmentSide);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      order_side =
          std::make_shared<TpchQ3OrderSegmentSide>(*typed->order_segment_side);
      side_version = image.version;
      break;
    }
    if (!order_side) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }

  struct OrderMeta {
    int32_t orderdate_days = 0;
    int32_t shippriority = 0;
  };
  std::unordered_map<int64_t, OrderMeta> order_meta;
  std::unordered_map<int64_t, TpchQ3InitialScan::OrderContext> context_cache;
  bool side_changed = false;

  auto resolve_context = [&](int64_t orderkey,
                             TpchQ3InitialScan::OrderContext &ctx) -> bool {
    const auto cached = context_cache.find(orderkey);
    if (cached != context_cache.end()) {
      ctx = cached->second;
      return ctx.segment != 0;
    }
    if (initial_scan && initial_scan->LookupOrderContext(orderkey, ctx) &&
        ctx.segment != 0) {
      context_cache.emplace(orderkey, ctx);
      return true;
    }
    return false;
  };

  auto resolve_from_order_delta =
      [&](const TpchQ3OrderDelta &delta,
          TpchQ3InitialScan::OrderContext &ctx) -> bool {
    if (resolve_context(delta.orderkey, ctx))
      return true;
    if (delta.custkey <= 0)
      return false;
    core::PartitionId segment = 0;
    if (static_cast<std::size_t>(delta.custkey) <
        order_side->customer_segment.size()) {
      segment = static_cast<core::PartitionId>(
          order_side->customer_segment[static_cast<std::size_t>(delta.custkey)]);
    }
    if (segment == 0 && initial_scan) {
      initial_scan->ScanCustomers([&](const TpchQ3CustomerInput &input) {
        if (input.key == delta.custkey)
          segment = TpchQ3PartitionFromMktsegment(input.mktsegment);
      });
    }
    if (segment == 0)
      return false;
    ctx = {segment, delta.orderdate_days, delta.shippriority, delta.custkey};
    context_cache.emplace(delta.orderkey, ctx);
    return true;
  };

  // An UPDATE of an order arrives as its old row (-1) and new row (+1): the
  // order and its lineitems stay; only its date / priority change in place
  // (a move to another segment rebuilds). Deleted orders go in RF2 below.
  std::unordered_set<int64_t> removed_orders, updated_orders;
  for (const auto &delta : buffer.OrderDeltas())
    if (delta.sign < 0)
      removed_orders.insert(delta.orderkey);
  for (const auto &delta : buffer.OrderDeltas())
    if (delta.sign > 0 && removed_orders.count(delta.orderkey))
      updated_orders.insert(delta.orderkey);
  std::unordered_map<int64_t, core::PartitionId> updated_old_segment;
  for (const auto orderkey : updated_orders) {
    core::PartitionId segment = 0;
    if (orderkey >= 0 &&
        static_cast<std::size_t>(orderkey) < order_side->order_segment.size())
      segment = static_cast<core::PartitionId>(
          order_side->order_segment[static_cast<std::size_t>(orderkey)]);
    updated_old_segment[orderkey] = segment;
  }

  // RF1: install order_segment (+ meta) before lineitem deltas.
  for (const auto &delta : buffer.OrderDeltas()) {
    if (delta.sign <= 0)
      continue;
    TpchQ3InitialScan::OrderContext ctx;
    if (!resolve_from_order_delta(delta, ctx)) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    if (updated_orders.count(delta.orderkey)) {
      const auto old_segment = updated_old_segment[delta.orderkey];
      if (old_segment != 0 && old_segment != ctx.segment) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
    }
    EnsureOrderSegmentSlot(order_side->order_segment, delta.orderkey);
    order_side->order_segment[static_cast<std::size_t>(delta.orderkey)] =
        static_cast<uint8_t>(ctx.segment);
    order_meta[delta.orderkey] = OrderMeta{delta.orderdate_days != 0
                                               ? delta.orderdate_days
                                               : ctx.orderdate_days,
                                           delta.shippriority != 0
                                               ? delta.shippriority
                                               : ctx.shippriority};
    side_changed = true;
  }

  std::map<core::PartitionId, std::vector<TpchQ3LineitemDelta>> grouped;
  for (auto delta : buffer.Deltas()) {
    if (delta.orderkey < 0)
      continue;
    core::PartitionId segment = 0;
    if (static_cast<std::size_t>(delta.orderkey) <
        order_side->order_segment.size())
      segment = static_cast<core::PartitionId>(
          order_side->order_segment[static_cast<std::size_t>(delta.orderkey)]);
    if (segment == 0) {
      TpchQ3InitialScan::OrderContext ctx;
      if (!resolve_context(delta.orderkey, ctx))
        continue;
      EnsureOrderSegmentSlot(order_side->order_segment, delta.orderkey);
      order_side->order_segment[static_cast<std::size_t>(delta.orderkey)] =
          static_cast<uint8_t>(ctx.segment);
      order_meta[delta.orderkey] =
          OrderMeta{ctx.orderdate_days, ctx.shippriority};
      segment = ctx.segment;
      side_changed = true;
    }
    delta.segment = segment;
    grouped[delta.segment].push_back(delta);
  }

  std::set<core::PartitionId> touched_segments;
  for (const auto &group : grouped)
    touched_segments.insert(group.first);

  // Collect segments that order deletes must clear from interval indexes.
  for (const auto &delta : buffer.OrderDeltas()) {
    if (delta.sign >= 0)
      continue;
    if (delta.orderkey < 0 ||
        static_cast<std::size_t>(delta.orderkey) >=
            order_side->order_segment.size())
      continue;
    const auto segment = static_cast<core::PartitionId>(
        order_side->order_segment[static_cast<std::size_t>(delta.orderkey)]);
    if (segment != 0)
      touched_segments.insert(segment);
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas),
       static_cast<uint64_t>(std::max<std::size_t>(1, touched_segments.size())),
       true, true, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    for (const auto segment : touched_segments) {
      if (!core::TpchQ3Layout::IsSegmentPartition(segment))
        continue;
      auto index = BuildSegment(services, segment);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ3Layout::kAccelerator, segment, 0,
          MakeSegmentIntervalImage(segment,
                                   MakeSegmentIntervalState(index))));
    }
    if (side_changed) {
      const auto typed_side =
          std::static_pointer_cast<const TpchQ3OrderSegmentSide>(
              std::move(order_side));
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ3Layout::kAccelerator,
          core::TpchQ3Layout::kOrderSegmentSide, 0,
          MakeOrderSegmentSideImage(typed_side)));
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

  std::map<core::PartitionId, TpchQ3SegmentIntervalIndex> indexes;
  std::map<core::PartitionId, core::Version> versions;
  for (const auto segment : touched_segments) {
    const auto snapshot =
        services.StateStore().ReadPartitions(visibility, {segment});
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
        indexes[segment] = it->second;
      else {
        indexes[segment] = TpchQ3SegmentIntervalIndex{segment, {}, {}};
      }
      versions[segment] = image.version;
      found = true;
      break;
    }
    if (!found) {
      // New segment partition can appear for RF1 into an empty segment image.
      indexes[segment] = TpchQ3SegmentIntervalIndex{segment, {}, {}};
      versions[segment] = 0;
    }
  }

  bool any_changed = false;
  std::size_t applied_lineitem_orders = 0;
  for (const auto orderkey : updated_orders) {
    const auto segment = updated_old_segment[orderkey];
    const auto meta_it = order_meta.find(orderkey);
    auto index_it = indexes.find(segment);
    if (segment == 0 || meta_it == order_meta.end() || index_it == indexes.end())
      continue;
    std::size_t position = 0;
    if (!index_it->second.FindPosition(orderkey, position))
      continue;
    auto &summary = index_it->second.orders[position];
    summary.orderdate_days = PackTpchQ3Date(meta_it->second.orderdate_days);
    summary.shippriority = PackTpchQ3Shippriority(meta_it->second.shippriority);
    any_changed = true;
    outcome.action = core::MaintenanceAction::ApplyDelta;
  }
  for (auto &group : grouped) {
    auto &index = indexes[group.first];
    bool needs_resort = false;
    bool changed = false;
    std::map<int64_t, std::vector<TpchQ3LineitemDelta>> by_order;
    for (const auto &delta : group.second)
      by_order[delta.orderkey].push_back(delta);

    for (const auto &order_deltas : by_order) {
      const auto orderkey = order_deltas.first;
      std::size_t position = 0;
      if (!index.FindPosition(orderkey, position)) {
        OrderMeta meta;
        const auto meta_it = order_meta.find(orderkey);
        if (meta_it != order_meta.end()) {
          meta = meta_it->second;
        } else {
          TpchQ3InitialScan::OrderContext ctx;
          if (!resolve_context(orderkey, ctx))
            continue;
          meta = OrderMeta{ctx.orderdate_days, ctx.shippriority};
        }
        TpchQ3OrderSummary summary;
        summary.orderkey = PackTpchQ3Orderkey(orderkey);
        summary.orderdate_days = PackTpchQ3Date(meta.orderdate_days);
        summary.shippriority = PackTpchQ3Shippriority(meta.shippriority);
        for (const auto &delta : order_deltas.second)
          ApplyLineitemDelta(index, summary, delta);
        if (summary.bucket_count == 0)
          continue;
        index.orders.push_back(summary);
        needs_resort = true;
        changed = true;
        ++applied_lineitem_orders;
        continue;
      }
      auto &summary = index.orders[position];
      const auto old_max = index.MaxShipdate(summary);
      for (const auto &delta : order_deltas.second)
        ApplyLineitemDelta(index, summary, delta);
      if (summary.bucket_count == 0) {
        RemoveOrderFromIndex(index, orderkey);
        needs_resort = true;
        changed = true;
        ++applied_lineitem_orders;
        continue;
      }
      if (index.MaxShipdate(summary) != old_max)
        needs_resort = true;
      changed = true;
      ++applied_lineitem_orders;
    }
    if (changed && needs_resort)
      SortTpchQ3SegmentIntervalIndex(index);
    if (changed) {
      any_changed = true;
      outcome.action = core::MaintenanceAction::ApplyDelta;
    }
  }

  // RF2: clear segment slots and remove from interval indexes after lineitems.
  for (const auto &delta : buffer.OrderDeltas()) {
    if (delta.sign >= 0)
      continue;
    if (delta.orderkey < 0 || updated_orders.count(delta.orderkey))
      continue;
    core::PartitionId segment = 0;
    if (static_cast<std::size_t>(delta.orderkey) <
        order_side->order_segment.size()) {
      segment = static_cast<core::PartitionId>(
          order_side->order_segment[static_cast<std::size_t>(delta.orderkey)]);
      order_side->order_segment[static_cast<std::size_t>(delta.orderkey)] = 0;
      side_changed = true;
    }
    if (segment == 0)
      continue;
    auto index_it = indexes.find(segment);
    if (index_it == indexes.end())
      continue;
    RemoveOrderFromIndex(index_it->second, delta.orderkey);
    any_changed = true;
    outcome.action = core::MaintenanceAction::ApplyDelta;
  }

  // A lineitem UPDATE that does not land on a published order used to still
  // report ApplyDelta and republish the unchanged top-k (SF=1 Q3 probe).
  if (!buffer.Deltas().empty() && applied_lineitem_orders == 0 &&
      !any_changed && !side_changed) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.updates.clear();
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto &[segment, index] : indexes) {
    if (touched_segments.find(segment) == touched_segments.end())
      continue;
    if (!any_changed && !side_changed)
      continue;
    const auto version = versions.count(segment) ? versions[segment] : 0;
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, segment, version,
        MakeSegmentIntervalImage(segment, MakeSegmentIntervalState(index))));
  }

  if (side_changed) {
    outcome.action = core::MaintenanceAction::ApplyDelta;
    const auto typed_side =
        std::static_pointer_cast<const TpchQ3OrderSegmentSide>(
            std::move(order_side));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, core::TpchQ3Layout::kOrderSegmentSide,
        side_version, MakeOrderSegmentSideImage(typed_side)));
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
  BuildScratch scratch;
  const auto state = BuildState(services, scratch);
  const auto partition = TpchQ3PartitionFromMktsegment(segment);
  const auto it = state.segment_intervals.find(partition);
  EmitResult(ComposeTpchQ3TopK(
                 it != state.segment_intervals.end()
                     ? it->second
                     : TpchQ3SegmentIntervalIndex{partition, {}, {}},
                 cutoff),
             sink);
}

TpchQ3State TpchQ3Module::BuildState(core::AuxEngineServices &services,
                                     BuildScratch &scratch) const {
  (void)services;
  TpchQ3State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");
  initial_scan->ScanCustomers([&](const TpchQ3CustomerInput &input) {
    const auto partition = TpchQ3PartitionFromMktsegment(input.mktsegment);
    if (partition != 0)
      scratch.customer_segments[input.key] = partition;
  });

  std::unordered_map<int64_t, OrderBuilder> builders;
  initial_scan->ScanOrders([&](const TpchQ3OrderInput &input) {
    const auto segment_it = scratch.customer_segments.find(input.custkey);
    if (segment_it == scratch.customer_segments.end())
      return;
    EnsureOrderSegmentSlot(scratch.order_segment, input.key);
    scratch.order_segment[static_cast<std::size_t>(input.key)] =
        static_cast<uint8_t>(segment_it->second);
    auto &builder = builders[input.key];
    builder.orderdate_days = input.orderdate_days;
    builder.shippriority = input.shippriority;
    builder.segment = segment_it->second;
  });
  initial_scan->ScanLineitems([&](const TpchQ3LineitemInput &input) {
    if (input.orderkey < 0 ||
        static_cast<std::size_t>(input.orderkey) >= scratch.order_segment.size())
      return;
    const auto segment =
        static_cast<core::PartitionId>(scratch.order_segment[static_cast<std::size_t>(
            input.orderkey)]);
    if (segment == 0)
      return;
    auto &builder = builders[input.orderkey];
    if (builder.segment == 0)
      builder.segment = segment;
    const auto revenue =
        TpchQ3RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                         input.discount_raw, input.discount_scale);
    builder.shipdate_revenue[input.shipdate_days] += revenue;
  });

  for (auto &entry : builders) {
    if (entry.second.shipdate_revenue.empty())
      continue;
    TpchQ3OrderSummary summary;
    summary.orderkey = PackTpchQ3Orderkey(entry.first);
    summary.orderdate_days = PackTpchQ3Date(entry.second.orderdate_days);
    summary.shippriority = PackTpchQ3Shippriority(entry.second.shippriority);
    std::vector<TpchQ3ShipdateBucket> buckets;
    FinalizeTpchQ3OrderSummary(buckets, entry.second.shipdate_revenue);
    auto &index = state.segment_intervals[entry.second.segment];
    index.segment = entry.second.segment;
    index.AssignBuckets(summary, std::move(buckets));
    if (summary.bucket_count == 0)
      continue;
    index.orders.push_back(summary);
  }
  for (auto &entry : state.segment_intervals)
    SortTpchQ3SegmentIntervalIndex(entry.second);
  scratch.order_segment.shrink_to_fit();
  return state;
}

TpchQ3SegmentIntervalIndex
TpchQ3Module::BuildSegment(core::AuxEngineServices &services,
                           core::PartitionId segment) const {
  (void)services;
  TpchQ3SegmentIntervalIndex index{segment, {}, {}};
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");
  const auto mktsegment = TpchQ3MktsegmentFromPartition(segment);
  if (mktsegment.empty())
    return index;

  std::unordered_map<int64_t, OrderBuilder> builders;
  auto consume_join = [&](const TpchQ3OrderInput &order,
                          const TpchQ3LineitemInput &line) {
    auto &builder = builders[order.key];
    builder.orderdate_days = order.orderdate_days;
    builder.shippriority = order.shippriority;
    builder.segment = segment;
    const auto revenue =
        TpchQ3RevenueRaw(line.extended_price_raw, line.extended_price_scale,
                         line.discount_raw, line.discount_scale);
    builder.shipdate_revenue[line.shipdate_days] += revenue;
  };

  if (!initial_scan->ScanSegmentJoin(mktsegment, consume_join)) {
    BuildScratch scratch;
    const auto state = BuildState(services, scratch);
    const auto it = state.segment_intervals.find(segment);
    if (it != state.segment_intervals.end())
      return it->second;
    return index;
  }

  for (auto &entry : builders) {
    if (entry.second.shipdate_revenue.empty())
      continue;
    TpchQ3OrderSummary summary;
    summary.orderkey = PackTpchQ3Orderkey(entry.first);
    summary.orderdate_days = PackTpchQ3Date(entry.second.orderdate_days);
    summary.shippriority = PackTpchQ3Shippriority(entry.second.shippriority);
    std::vector<TpchQ3ShipdateBucket> buckets;
    FinalizeTpchQ3OrderSummary(buckets, entry.second.shipdate_revenue);
    index.AssignBuckets(summary, std::move(buckets));
    if (summary.bucket_count == 0)
      continue;
    index.orders.push_back(summary);
  }
  SortTpchQ3SegmentIntervalIndex(index);
  return index;
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

std::vector<core::CalibrationWrite> TpchQ3Module::CalibrationWrites() const {
  return TpchQ3CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
