#include "tpch/q3/postgres/aux/module.hpp"
#include "tpch/q3/calibration_writes.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <cstring>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr std::size_t kTopK = 10;
// Orders per build slice: sorting a few days, merging them and encoding the
// record costs ~0.5 us, so 8192 pay for a task.
constexpr std::size_t kOrderGrain = 8192;

// ── Cell extraction helpers ─────────────────────────────────────────────────
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
  return false;
}

std::shared_ptr<const TpchQ3StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ3State> &state) {
  auto image = std::make_shared<TpchQ3StateImage>();
  image->state = state;
  return image;
}

const TpchQ3StateImage *AsQ3Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ3StateImage>();
  return payload && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// ── Bound bins ──────────────────────────────────────────────────────────────
// Bound values below 64 are their own bin; above, a bin is one value of the
// top six bits (~3% wide), numbered in increasing order of the bound.
int32_t BinOfBound(int64_t bound) {
  if (bound < 64)
    return bound < 0 ? 0 : static_cast<int32_t>(bound);
  const unsigned exponent = 63u - static_cast<unsigned>(__builtin_clzll(
                                      static_cast<unsigned long long>(bound)));
  const uint64_t top = static_cast<uint64_t>(bound) >> (exponent - 5);
  return static_cast<int32_t>(64 + (exponent - 6) * 32 + (top - 32));
}

// ── Order record encoding ───────────────────────────────────────────────────
// [i32 orderdate][i32 shippriority][u32 n] n x ([i32 shipdate][i64 revenue])
constexpr std::size_t kRecordHeader = 12;
constexpr std::size_t kBucketBytes = 12;

std::vector<uint8_t> EncodeRecord(const TpchQ3State::OrderRecord &record) {
  std::vector<uint8_t> bytes(kRecordHeader +
                             record.buckets.size() * kBucketBytes);
  const uint32_t count = static_cast<uint32_t>(record.buckets.size());
  std::memcpy(&bytes[0], &record.orderdate_days, 4);
  std::memcpy(&bytes[4], &record.shippriority, 4);
  std::memcpy(&bytes[8], &count, 4);
  std::size_t at = kRecordHeader;
  for (const auto &bucket : record.buckets) {
    std::memcpy(&bytes[at], &bucket.shipdate, 4);
    std::memcpy(&bytes[at + 4], &bucket.revenue_raw, 8);
    at += kBucketBytes;
  }
  return bytes;
}

// The query for one order given as header fields and buckets: nothing when it
// was ordered on or after the cutoff or has no lineitem shipped after it;
// otherwise the SUM of those lineitems' revenue.
struct OrderView {
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
  uint32_t count = 0;
  const uint8_t *buckets = nullptr;

  static OrderView Of(const core::ShardedCowRecordStore<
                      TpchQ3State::kBinShardBits>::View &view) {
    OrderView out;
    std::memcpy(&out.orderdate_days, view.data, 4);
    std::memcpy(&out.shippriority, view.data + 4, 4);
    std::memcpy(&out.count, view.data + 8, 4);
    out.buckets = view.data + kRecordHeader;
    return out;
  }
  bool RevenueAfter(int32_t cutoff_days, int64_t &revenue) const {
    if (orderdate_days >= cutoff_days)
      return false;
    bool any = false;
    revenue = 0;
    for (uint32_t i = 0; i < count; ++i) {
      int32_t shipdate = 0;
      int64_t bucket_revenue = 0;
      std::memcpy(&shipdate, buckets + i * kBucketBytes, 4);
      std::memcpy(&bucket_revenue, buckets + i * kBucketBytes + 4, 8);
      if (shipdate > cutoff_days) {
        any = true;
        revenue += bucket_revenue;
      }
    }
    return any;
  }
};

// ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority.
bool BetterRow(const TpchQ3OutputRow &lhs, const TpchQ3OutputRow &rhs) {
  if (lhs.revenue_raw != rhs.revenue_raw)
    return lhs.revenue_raw > rhs.revenue_raw;
  if (lhs.orderdate_days != rhs.orderdate_days)
    return lhs.orderdate_days < rhs.orderdate_days;
  if (lhs.orderkey != rhs.orderkey)
    return lhs.orderkey < rhs.orderkey;
  return lhs.shippriority < rhs.shippriority;
}

// LIMIT 10: keeps the best rows seen so far; the worst of them on top.
class TopK {
public:
  void Offer(const TpchQ3OutputRow &row) {
    if (heap.size() < kTopK) {
      heap.push(row);
    } else if (BetterRow(row, heap.top())) {
      heap.pop();
      heap.push(row);
    }
  }
  bool Full() const { return heap.size() >= kTopK; }
  int64_t KthRevenue() const { return heap.top().revenue_raw; }
  std::vector<TpchQ3OutputRow> Sorted() {
    std::vector<TpchQ3OutputRow> rows;
    while (!heap.empty()) {
      rows.push_back(heap.top());
      heap.pop();
    }
    std::sort(rows.begin(), rows.end(), BetterRow);
    return rows;
  }

private:
  struct Worse {
    bool operator()(const TpchQ3OutputRow &lhs, const TpchQ3OutputRow &rhs) const {
      return BetterRow(lhs, rhs);
    }
  };
  std::priority_queue<TpchQ3OutputRow, std::vector<TpchQ3OutputRow>, Worse> heap;
};

// The (shipdate, revenue) pairs of one order as ascending per-day buckets.
std::vector<TpchQ3State::Bucket>
DayBuckets(std::vector<std::pair<int32_t, int64_t>> &days) {
  std::sort(days.begin(), days.end());
  std::vector<TpchQ3State::Bucket> buckets;
  for (const auto &[day, revenue] : days) {
    if (!buckets.empty() && buckets.back().shipdate == day)
      buckets.back().revenue_raw += revenue;
    else
      buckets.push_back({day, revenue});
  }
  return buckets;
}

int64_t RevenueRaw(int64_t extended_price_raw, int64_t discount_raw) {
  return extended_price_raw * (100 - discount_raw);
}

// Collects the consecutive rows of one order into its new state; reports it
// when the next order starts (or at Flush).
class OrderLineGrouper {
public:
  using Emit = std::function<void(TpchQ3OrderResult &&)>;
  explicit OrderLineGrouper(Emit emit) : emit(std::move(emit)) {}

  void Add(const TpchQ3OrderLineInput &line) {
    if (open && line.orderkey != current.orderkey)
      Flush();
    if (!open) {
      current = TpchQ3OrderResult{};
      current.orderkey = line.orderkey;
      current.present = true;
      current.mktsegment = line.mktsegment;
      current.record.orderkey = line.orderkey;
      current.record.orderdate_days = line.orderdate_days;
      current.record.shippriority = line.shippriority;
      open = true;
    }
    days.emplace_back(line.shipdate_days,
                      RevenueRaw(line.extended_price_raw, line.discount_raw));
  }
  void Flush() {
    if (!open)
      return;
    current.record.buckets = DayBuckets(days);
    days.clear();
    open = false;
    emit(std::move(current));
  }

private:
  Emit emit;
  TpchQ3OrderResult current;
  bool open = false;
  std::vector<std::pair<int32_t, int64_t>> days;
};

// ── Query parameters ────────────────────────────────────────────────────────
// false: a NULL parameter, for which the WHERE clause keeps nothing.
bool ReadParameters(const core::QueryArguments &arguments, std::string &segment,
                    int32_t &cutoff_days) {
  if (arguments.values.size() < 2 || arguments.values[0].is_null ||
      arguments.values[1].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload))
    return false;
  segment = std::get<std::string>(arguments.values[0].payload);
  const auto &date = arguments.values[1].payload;
  if (const auto *d = std::get_if<core::AuxDate>(&date)) {
    cutoff_days = d->days_since_epoch;
    return true;
  }
  if (const auto *i = std::get_if<int64_t>(&date)) {
    if (*i < std::numeric_limits<int32_t>::min() ||
        *i > std::numeric_limits<int32_t>::max())
      return false;
    cutoff_days = static_cast<int32_t>(*i);
    return true;
  }
  return false;
}

} // namespace

int64_t TpchQ3BinUpperBound(int32_t bin) {
  if (bin < 64)
    return bin;
  const unsigned t = static_cast<unsigned>(bin - 64);
  const unsigned exponent = t / 32 + 6;
  const uint64_t top = 33 + t % 32;
  const unsigned __int128 bound =
      (static_cast<unsigned __int128>(top) << (exponent - 5)) - 1;
  return bound > static_cast<unsigned __int128>(std::numeric_limits<int64_t>::max())
             ? std::numeric_limits<int64_t>::max()
             : static_cast<int64_t>(bound);
}

// ── State ───────────────────────────────────────────────────────────────────
TpchQ3State::PreparedOrder TpchQ3State::Prepare(const OrderRecord &record) {
  int64_t bound = 0; // no answer can take more than the positive day sums
  for (const auto &bucket : record.buckets)
    if (bucket.revenue_raw > 0)
      bound += bucket.revenue_raw;
  return {BinOfBound(bound), EncodeRecord(record)};
}

void TpchQ3State::Install(const std::string &mktsegment, int64_t orderkey,
                          const PreparedOrder &order) {
  const uint32_t segment =
      segment_ids.emplace(mktsegment, static_cast<uint32_t>(segment_ids.size()))
          .first->second;
  bins[{segment, order.bin}].Put(orderkey, order.bytes.data(),
                                 order.bytes.size());
  locators.Upsert(orderkey, Locator{segment, order.bin});
}

void TpchQ3State::SetOrder(const std::string &mktsegment,
                           const OrderRecord &record) {
  RemoveOrder(record.orderkey);
  if (!record.buckets.empty())
    Install(mktsegment, record.orderkey, Prepare(record));
}

void TpchQ3State::RemoveOrder(int64_t orderkey) {
  const Locator *found = locators.Find(orderkey);
  if (!found)
    return;
  const Locator locator = *found;
  const auto it = bins.find({locator.segment, locator.bin});
  if (it != bins.end()) {
    it->second.Erase(orderkey);
    if (it->second.empty())
      bins.erase(it);
  }
  locators.Erase(orderkey);
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ3TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ3TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  order_results.clear();
}
bool TpchQ3TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
void TpchQ3TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  order_results.clear();
}
void TpchQ3TxnBuffer::AddOrderResult(TpchQ3OrderResult result) {
  pending = true;
  order_results.push_back(std::move(result));
}
const std::vector<TpchQ3OrderResult> &TpchQ3TxnBuffer::OrderResults() const {
  return order_results;
}

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ3Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ3Module::Dependencies() const {
  return {{config.customer_table, {0, 1}, {0, 6}},
          {config.orders_table, {0, 1, 2, 3}, {0, 4, 1, 7}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 5, 6, 10}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ3Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ3TxnBuffer>();
}

void TpchQ3Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  const auto typed_state =
      std::make_shared<const TpchQ3State>(BuildState(services));
  builder.Write(core::StateImage::Make(TpchQ3Layout::kAccelerator,
                                       TpchQ3Layout::kState, 0,
                                       MakeStateImage(typed_state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ3Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.customer_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

// Every write is applied incrementally: the orders it touches (their own
// orderkeys, or those of a changed customer) are re-read from the database as
// the statement left them, one keyed query, and each order's record replaced.
void TpchQ3Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ3TxnBuffer &>(base_buffer);
  const bool paired = mutation.kind == core::MutationKind::Update;
  if (paired && mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }
  const bool has_old = mutation.kind != core::MutationKind::Insert;
  const bool has_new = mutation.kind != core::MutationKind::Delete;
  const std::size_t count =
      has_old ? mutation.old_rows.size() : mutation.new_rows.size();

  std::vector<int64_t> orderkeys;
  std::vector<int64_t> custkeys;
  const bool is_customer = mutation.table == config.customer_table;
  if (!is_customer && mutation.table != config.lineitem_table &&
      mutation.table != config.orders_table)
    return;
  for (std::size_t i = 0; i < count; ++i) {
    for (int side = 0; side < 2; ++side) {
      const bool old_side = side == 0;
      if (old_side ? !has_old : !has_new)
        continue;
      const core::AuxRow &row =
          old_side ? mutation.old_rows[i] : mutation.new_rows[i];
      int64_t key = 0;
      if (!IntCell(row, 0, key)) {
        buffer.MarkRebuild();
        return;
      }
      (is_customer ? custkeys : orderkeys).push_back(key);
    }
  }
  if (!custkeys.empty()) {
    std::sort(custkeys.begin(), custkeys.end());
    custkeys.erase(std::unique(custkeys.begin(), custkeys.end()), custkeys.end());
    if (!initial_scan ||
        !initial_scan->LookupOrdersOfCustomers(custkeys, orderkeys)) {
      buffer.MarkRebuild();
      return;
    }
  }
  std::sort(orderkeys.begin(), orderkeys.end());
  orderkeys.erase(std::unique(orderkeys.begin(), orderkeys.end()),
                  orderkeys.end());
  if (orderkeys.empty())
    return;

  std::unordered_map<int64_t, TpchQ3OrderResult> results;
  OrderLineGrouper grouper([&](TpchQ3OrderResult &&result) {
    const int64_t key = result.orderkey;
    results[key] = std::move(result);
  });
  if (!initial_scan ||
      !initial_scan->LookupOrderLines(
          orderkeys,
          [&](const TpchQ3OrderLineInput &line) { grouper.Add(line); })) {
    buffer.MarkRebuild();
    return;
  }
  grouper.Flush();
  for (const int64_t orderkey : orderkeys) {
    auto it = results.find(orderkey);
    if (it != results.end()) {
      buffer.AddOrderResult(std::move(it->second));
    } else {
      TpchQ3OrderResult gone;
      gone.orderkey = orderkey;
      buffer.AddOrderResult(std::move(gone));
    }
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
  if (buffer.OrderResults().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.OrderResults().size()), 1, true, false,
       true, false, false, core::MaintenanceAction::NoOp,
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

  std::shared_ptr<const TpchQ3State> current;
  core::Version version = 0;
  if (!LoadState(services, current, &version)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  // Copy-on-write copy: only the shards a record write touches are cloned.
  auto new_state = std::make_shared<TpchQ3State>(*current);
  for (const auto &result : buffer.OrderResults()) {
    if (result.present)
      new_state->SetOrder(result.mktsegment, result.record);
    else
      new_state->RemoveOrder(result.orderkey);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ3State>(std::move(new_state));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ3Layout::kAccelerator, TpchQ3Layout::kState, version,
      MakeStateImage(typed_state)));
  return outcome;
}

bool TpchQ3Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

// Top-k search: bins of the segment from the largest bound down; an order's
// revenue after the cutoff never exceeds its bound, so once the k-th best
// revenue is above the bound of the next bin nothing in it (or below) can
// enter the answer. Orders with fewer than k qualifying orders scan the
// segment fully (there is no k-th best).
void TpchQ3Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  std::string segment;
  int32_t cutoff = 0;
  if (!ReadParameters(arguments, segment, cutoff)) {
    EmitResult({}, sink);
    return;
  }
  std::shared_ptr<const TpchQ3State> state;
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid() ||
      !LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto segment_it = state->segment_ids.find(segment);
  if (segment_it == state->segment_ids.end()) {
    EmitResult({}, sink); // no customer has this market segment
    return;
  }
  const uint32_t segment_id = segment_it->second;

  TopK top;
  auto it = state->bins.upper_bound({segment_id, std::numeric_limits<int32_t>::max()});
  while (it != state->bins.begin()) {
    --it;
    if (it->first.first != segment_id)
      break;
    if (top.Full() && top.KthRevenue() > TpchQ3BinUpperBound(it->first.second))
      break;
    it->second.ForEach(
        [&](int64_t orderkey, const TpchQ3State::Bin::View &view) {
          const OrderView order = OrderView::Of(view);
          int64_t revenue = 0;
          if (order.RevenueAfter(cutoff, revenue))
            top.Offer({orderkey, revenue, order.orderdate_days,
                       order.shippriority});
        });
  }
  EmitResult(top.Sorted(), sink);
}

// No usable state: one streaming pass over the joined rows, O(1) memory.
void TpchQ3Module::ExecuteFallback(core::AuxEngineServices &,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");
  std::string segment;
  int32_t cutoff = 0;
  if (!ReadParameters(arguments, segment, cutoff)) {
    EmitResult({}, sink);
    return;
  }
  TopK top;
  OrderLineGrouper grouper([&](TpchQ3OrderResult &&result) {
    if (result.mktsegment != segment)
      return;
    const auto bytes = EncodeRecord(result.record);
    core::ShardedCowRecordStore<TpchQ3State::kBinShardBits>::View view{
        bytes.data(), bytes.size()};
    const OrderView order = OrderView::Of(view);
    int64_t revenue = 0;
    if (order.RevenueAfter(cutoff, revenue))
      top.Offer({result.orderkey, revenue, order.orderdate_days,
                 order.shippriority});
  });
  initial_scan->ScanOrderLines(
      [&](const TpchQ3OrderLineInput &line) { grouper.Add(line); });
  grouper.Flush();
  EmitResult(top.Sorted(), sink);
}

namespace {

// customer ⨝ orders ⨝ lineitem, gathered from three table scans: a lineitem
// joins through its order's position, which is its rank by orderkey.
class OrderJoin {
public:
  struct Order {
    int64_t orderkey = 0;
    int32_t orderdate_days = 0;
    int32_t shippriority = 0;
    uint32_t segment = 0;
    uint32_t lines = 0;
  };
  struct Line {
    uint32_t order = 0;
    int32_t shipdate_days = 0;
    int64_t revenue_raw = 0;
  };

  explicit OrderJoin(TpchQ3InitialScan &scan) {
    std::map<std::string, uint32_t> segment_ids;
    core::FlatHashMap<uint32_t> segment_of_customer;
    scan.ScanCustomers([&](const TpchQ3CustomerInput &customer) {
      const auto [it, added] = segment_ids.emplace(
          customer.mktsegment, static_cast<uint32_t>(segments.size()));
      if (added)
        segments.push_back(customer.mktsegment);
      segment_of_customer.Upsert(customer.custkey, it->second);
    });
    scan.ScanOrders([&](const TpchQ3OrderInput &order) {
      if (const uint32_t *segment = segment_of_customer.Find(order.custkey))
        orders.push_back({order.orderkey, order.orderdate_days,
                          order.shippriority, *segment, 0});
    });
    std::sort(orders.begin(), orders.end(), [](const Order &l, const Order &r) {
      return l.orderkey < r.orderkey;
    });
    core::FlatHashMap<uint32_t> rank_of_order(orders.size());
    for (std::size_t rank = 0; rank < orders.size(); ++rank)
      rank_of_order.Upsert(orders[rank].orderkey, static_cast<uint32_t>(rank));
    std::vector<Line> arrived;
    scan.ScanLineitems([&](const TpchQ3LineitemInput &line) {
      if (const uint32_t *rank = rank_of_order.Find(line.orderkey)) {
        ++orders[*rank].lines;
        arrived.push_back({*rank, line.shipdate_days,
                           RevenueRaw(line.extended_price_raw, line.discount_raw)});
      }
    });
    // Counting sort of the lineitems by order rank.
    std::vector<std::size_t> end(orders.size());
    std::size_t total = 0;
    for (std::size_t rank = 0; rank < orders.size(); ++rank)
      end[rank] = total += orders[rank].lines;
    lines.resize(arrived.size());
    for (const Line &line : arrived)
      lines[--end[line.order]] = line;
    first_line = std::move(end); // each `end` has been walked back to its start
  }

  // The record of the order of rank `rank` (no buckets without lineitems).
  TpchQ3State::OrderRecord Record(std::size_t rank) const {
    const Order &order = orders[rank];
    std::vector<std::pair<int32_t, int64_t>> days;
    for (std::size_t i = 0; i < order.lines; ++i)
      days.emplace_back(lines[first_line[rank] + i].shipdate_days,
                        lines[first_line[rank] + i].revenue_raw);
    TpchQ3State::OrderRecord record;
    record.orderkey = order.orderkey;
    record.orderdate_days = order.orderdate_days;
    record.shippriority = order.shippriority;
    record.buckets = DayBuckets(days);
    return record;
  }

  std::vector<std::string> segments;
  std::vector<Order> orders; // ascending orderkey
  std::vector<Line> lines;   // grouped by order, from first_line[rank]
  std::vector<std::size_t> first_line;
};

} // namespace

// Records are built in slices; installing them (shared bin and locator
// shards) stays serial, in orderkey order.
TpchQ3State TpchQ3Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");
  const OrderJoin join(*initial_scan);

  struct Prepared {
    int64_t orderkey = 0;
    uint32_t segment = 0;
    TpchQ3State::PreparedOrder order;
  };
  std::vector<Prepared> prepared;
  core::ParallelReduce(
      services, join.orders.size(), kOrderGrain, prepared,
      [&](std::vector<Prepared> &out, std::size_t begin, std::size_t end) {
        for (std::size_t rank = begin; rank < end; ++rank) {
          if (join.orders[rank].lines == 0)
            continue;
          out.push_back({join.orders[rank].orderkey, join.orders[rank].segment,
                         TpchQ3State::Prepare(join.Record(rank))});
        }
      },
      [](std::vector<Prepared> &into, std::vector<Prepared> &&from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()),
                    std::make_move_iterator(from.end()));
      });

  TpchQ3State state;
  for (const Prepared &p : prepared)
    state.Install(join.segments[p.segment], p.orderkey, p.order);
  return state;
}

bool TpchQ3Module::LoadState(core::AuxEngineServices &services,
                             std::shared_ptr<const TpchQ3State> &state,
                             core::Version *version) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ3Layout::kState});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ3Layout::kState)
      continue;
    const auto *typed = AsQ3Image(image);
    if (!typed)
      return false;
    state = typed->state;
    if (version)
      *version = image.version;
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
