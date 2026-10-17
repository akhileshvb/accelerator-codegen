#include "tpch/q3/mysql/aux/module.hpp"
#include "tpch/q3/calibration_writes.hpp"

#include "prims/mysql_collation.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {
constexpr std::size_t kTopK = 10;
// Elements per slice below which sorting stays on one thread.
constexpr std::size_t kSortGrain = 16384;
// Order results per slice below which their records are encoded serially.
constexpr std::size_t kPrepareGrain = 1024;
// Segment bins per slice of a read wave (each bin is scanned whole).
constexpr std::size_t kBinGrain = 1;

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
  // Offers the rows of `other`, which is left empty.
  void Merge(TopK &other) {
    for (const auto &row : other.Sorted())
      Offer(row);
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
                      line.extended_price_raw * (100 - line.discount_raw));
  }
  void Flush() {
    if (!open)
      return;
    std::sort(days.begin(), days.end());
    for (const auto &[day, revenue] : days) {
      auto &buckets = current.record.buckets;
      if (!buckets.empty() && buckets.back().shipdate == day)
        buckets.back().revenue_raw += revenue;
      else
        buckets.push_back({day, revenue});
    }
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

// Records are encoded in slices; replacing them in the shared bin and locator
// shards stays serial, in result order.
void ApplyOrderResults(const core::AuxEngineServices &services, TpchQ3State &state,
                       const std::vector<TpchQ3OrderResult> &results) {
  const auto has_record = [](const TpchQ3OrderResult &result) {
    return result.present && !result.record.buckets.empty();
  };
  std::vector<TpchQ3State::PreparedOrder> prepared(results.size());
  core::ParallelEach(services, results.size(), kPrepareGrain, [&](std::size_t i) {
    if (has_record(results[i]))
      prepared[i] = TpchQ3State::Prepare(results[i].record);
  });
  for (std::size_t i = 0; i < results.size(); ++i) {
    state.RemoveOrder(results[i].orderkey);
    if (has_record(results[i]))
      // c_mktsegment = ? is case-insensitive (MySQL default collation): folded ids.
      state.Install(state.SegmentId(mysql_collation::FoldAscii(results[i].mktsegment)),
                    results[i].orderkey, prepared[i]);
  }
}

// Customers of one scan partition: key and market segment, the segments
// case-folded (c_mktsegment = ? ignores case) and numbered by the partition's
// own dictionary.
struct CustomerPartial {
  std::vector<std::string> segments;
  std::vector<std::pair<int64_t, uint32_t>> customers; // custkey, segment index

  uint32_t IndexOf(const std::string &folded) {
    for (uint32_t i = 0; i < segments.size(); ++i)
      if (segments[i] == folded)
        return i;
    segments.push_back(folded);
    return static_cast<uint32_t>(segments.size() - 1);
  }
  void Merge(CustomerPartial &&from) {
    std::vector<uint32_t> index;
    for (const auto &segment : from.segments)
      index.push_back(IndexOf(segment));
    for (const auto &[custkey, segment] : from.customers)
      customers.emplace_back(custkey, index[segment]);
  }
};

struct JoinedOrder {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
  uint32_t segment = 0; // index in the customers' dictionary
};

struct Line {
  int64_t orderkey = 0;
  int32_t shipdate_days = 0;
  int64_t revenue_raw = 0;
};

// The lineitems of a build scan as one chunk per partition, so that merging
// moves vectors instead of lines.
using LineChunks = std::vector<std::vector<Line>>;

struct PreparedRecord {
  int64_t orderkey = 0;
  uint32_t segment = 0;
  TpchQ3State::PreparedOrder order;
};

// The segment of a customer, or null when there is no such customer.
const uint32_t *SegmentOf(const std::vector<std::pair<int64_t, uint32_t>> &customers,
                          int64_t custkey) {
  const auto found = std::lower_bound(
      customers.begin(), customers.end(), custkey,
      [](const std::pair<int64_t, uint32_t> &customer, int64_t key) {
        return customer.first < key;
      });
  return found != customers.end() && found->first == custkey ? &found->second : nullptr;
}

// The record of `order` from its lineitems lines[begin, end), sorted by shipdate.
PreparedRecord PrepareRecord(const JoinedOrder &order, const std::vector<Line> &lines,
                             std::size_t begin, std::size_t end) {
  TpchQ3State::OrderRecord record;
  record.orderkey = order.orderkey;
  record.orderdate_days = order.orderdate_days;
  record.shippriority = order.shippriority;
  for (std::size_t i = begin; i < end; ++i) {
    auto &buckets = record.buckets;
    if (!buckets.empty() && buckets.back().shipdate == lines[i].shipdate_days)
      buckets.back().revenue_raw += lines[i].revenue_raw;
    else
      buckets.push_back({lines[i].shipdate_days, lines[i].revenue_raw});
  }
  return {order.orderkey, order.segment, TpchQ3State::Prepare(record)};
}

// The records of the orders (ascending orderkey) that `lines` belong to. The
// lines are sorted here; the order cursor only moves forward.
std::vector<PreparedRecord> PrepareRecords(const std::vector<JoinedOrder> &orders,
                                           std::vector<Line> &lines) {
  std::sort(lines.begin(), lines.end(), [](const Line &l, const Line &r) {
    return std::tie(l.orderkey, l.shipdate_days) < std::tie(r.orderkey, r.shipdate_days);
  });
  std::vector<PreparedRecord> records;
  if (lines.empty())
    return records;
  auto order = std::lower_bound(orders.begin(), orders.end(), lines.front().orderkey,
                                [](const JoinedOrder &o, int64_t key) { return o.orderkey < key; });
  for (std::size_t begin = 0, end = 0; begin < lines.size(); begin = end) {
    const int64_t key = lines[begin].orderkey;
    while (end < lines.size() && lines[end].orderkey == key)
      ++end;
    while (order != orders.end() && order->orderkey < key)
      ++order;
    if (order != orders.end() && order->orderkey == key)
      records.push_back(PrepareRecord(*order, lines, begin, end));
  }
  return records;
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

TpchQ3State::PreparedOrder TpchQ3State::Prepare(const OrderRecord &record) {
  int64_t bound = 0; // no answer can take more than the positive day sums
  for (const auto &bucket : record.buckets)
    if (bucket.revenue_raw > 0)
      bound += bucket.revenue_raw;
  return {BinOfBound(bound), EncodeRecord(record)};
}

uint32_t TpchQ3State::SegmentId(const std::string &folded_mktsegment) {
  return segment_ids
      .emplace(folded_mktsegment, static_cast<uint32_t>(segment_ids.size()))
      .first->second;
}

void TpchQ3State::Install(uint32_t segment, int64_t orderkey,
                          const PreparedOrder &order) {
  bins[{segment, order.bin}].Put(orderkey, order.bytes.data(),
                                 order.bytes.size());
  locators.Upsert(orderkey, Locator{segment, order.bin});
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
  const auto typed_state = std::make_shared<const TpchQ3State>(BuildState(services));
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
      if (!core::IntCell(row, 0, key)) {
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
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.OrderResults().empty())
    return outcome;

  const auto decision =
      services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
          buffer.OrderResults().size(), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
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
  ApplyOrderResults(services, *new_state, buffer.OrderResults());

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

  const auto segment_it = state->segment_ids.find(mysql_collation::FoldAscii(segment));
  if (segment_it == state->segment_ids.end()) {
    EmitResult({}, sink); // no customer has this market segment
    return;
  }
  const uint32_t segment_id = segment_it->second;

  // The segment's bins from the largest bound down.
  struct Candidate {
    int32_t bound_bin;
    const TpchQ3State::Bin *bin;
  };
  std::vector<Candidate> candidates;
  auto it = state->bins.upper_bound({segment_id, std::numeric_limits<int32_t>::max()});
  while (it != state->bins.begin()) {
    --it;
    if (it->first.first != segment_id)
      break;
    candidates.push_back({it->first.second, &it->second});
  }

  // Waves of one bin per worker, each scanned into its own top k. The best k
  // rows do not depend on the order bins are visited in, and a bin the wave
  // skips by the k-th revenue it started with is one the serial walk skips too.
  TopK top;
  const std::size_t wave = std::max<std::size_t>(1, services.WorkerCount());
  for (std::size_t first = 0; first < candidates.size(); first += wave) {
    if (top.Full() && top.KthRevenue() > TpchQ3BinUpperBound(candidates[first].bound_bin))
      break;
    const std::size_t count = std::min(wave, candidates.size() - first);
    const bool full = top.Full();
    const int64_t kth = full ? top.KthRevenue() : 0;
    std::vector<TopK> found(count);
    core::ParallelEach(services, count, kBinGrain, [&](std::size_t i) {
      if (full && kth > TpchQ3BinUpperBound(candidates[first + i].bound_bin))
        return;
      candidates[first + i].bin->ForEach(
          [&](int64_t orderkey, const TpchQ3State::Bin::View &view) {
            const OrderView order = OrderView::Of(view);
            int64_t revenue = 0;
            if (order.RevenueAfter(cutoff, revenue))
              found[i].Offer({orderkey, revenue, order.orderdate_days,
                              order.shippriority});
          });
    });
    for (auto &partial : found)
      top.Merge(partial);
  }
  EmitResult(top.Sorted(), sink);
}

// Customers, orders and lineitems are scanned in partitions (customer ->
// orders -> lineitems, each probing the one before); each lineitem chunk then
// becomes its orders' records on its own. Installing the records in the shared
// bin and locator shards stays serial, in orderkey order.
TpchQ3State TpchQ3Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");
  const std::size_t partitions = core::BuildPartitions(services);

  auto customers = core::PartitionedBuild<CustomerPartial>(
      services, [] { return CustomerPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanCustomers(
            services, parts, [&](std::size_t partition, const TpchQ3CustomerInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ3CustomerInput>{input});
            });
      },
      [](CustomerPartial &into, const TpchQ3CustomerInput &input) {
        into.customers.emplace_back(
            input.custkey, into.IndexOf(mysql_collation::FoldAscii(input.mktsegment)));
      },
      [](CustomerPartial &into, CustomerPartial &&from) { into.Merge(std::move(from)); },
      partitions);
  core::ParallelStableSort(services, customers.customers,
                     [](const auto &l, const auto &r) { return l.first < r.first; },
                     kSortGrain);

  auto orders = core::PartitionedBuild<std::vector<JoinedOrder>>(
      services, [] { return std::vector<JoinedOrder>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(
            services, parts, [&](std::size_t partition, const TpchQ3OrderInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ3OrderInput>{input});
            });
      },
      [&](std::vector<JoinedOrder> &into, const TpchQ3OrderInput &input) {
        if (const uint32_t *segment = SegmentOf(customers.customers, input.custkey))
          into.push_back({input.orderkey, input.orderdate_days, input.shippriority, *segment});
      },
      [](std::vector<JoinedOrder> &into, std::vector<JoinedOrder> &&from) {
        into.insert(into.end(), from.begin(), from.end());
      },
      partitions);
  core::ParallelStableSort(services, orders,
                     [](const JoinedOrder &l, const JoinedOrder &r) { return l.orderkey < r.orderkey; },
                     kSortGrain);

  auto chunks = core::PartitionedBuild<LineChunks>(
      services, [] { return LineChunks(1); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(
            services, parts, [&](std::size_t partition, const TpchQ3LineitemInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ3LineitemInput>{input});
            });
      },
      [](LineChunks &into, const TpchQ3LineitemInput &input) {
        into.back().push_back({input.orderkey, input.shipdate_days,
                               input.extended_price_raw * (100 - input.discount_raw)});
      },
      [](LineChunks &into, LineChunks &&from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()),
                    std::make_move_iterator(from.end()));
      },
      partitions);

  // The chunks cover ascending orderkey ranges: chunk order is orderkey order.
  std::vector<std::vector<PreparedRecord>> records(chunks.size());
  core::ParallelEach(services, chunks.size(), 1, [&](std::size_t chunk) {
    records[chunk] = PrepareRecords(orders, chunks[chunk]);
    std::vector<Line>().swap(chunks[chunk]);
  });

  TpchQ3State state;
  std::vector<uint32_t> segment_ids;
  for (const auto &segment : customers.segments)
    segment_ids.push_back(state.SegmentId(segment));
  for (auto &chunk : records) {
    for (const auto &record : chunk)
      state.Install(segment_ids[record.segment], record.orderkey, record.order);
    std::vector<PreparedRecord>().swap(chunk);
  }
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
