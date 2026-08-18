#include "tpch/q3/aux/module.hpp"

#include <algorithm>
#include <queue>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr std::size_t kTopK = 10;

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

} // namespace

void FinalizeTpchQ3OrderSummary(std::vector<TpchQ3ShipdateBucket> &buckets,
                                const std::map<int32_t, int64_t> &day_revenue) {
  buckets.clear();
  for (const auto &entry : day_revenue) {
    if (entry.second != 0)
      buckets.push_back({PackTpchQ3Date(entry.first), 0});
  }
  if (buckets.empty()) {
    buckets.shrink_to_fit();
    return;
  }
  std::sort(buckets.begin(), buckets.end(),
            [](const TpchQ3ShipdateBucket &lhs, const TpchQ3ShipdateBucket &rhs) {
              return lhs.shipdate < rhs.shipdate;
            });
  int64_t running = 0;
  for (int index = static_cast<int>(buckets.size()) - 1; index >= 0; --index) {
    running += day_revenue.at(buckets[static_cast<std::size_t>(index)].shipdate);
    buckets[static_cast<std::size_t>(index)].suffix_revenue = running;
  }
  buckets.shrink_to_fit();
}

int64_t TpchQ3RevenueAboveCutoff(const TpchQ3OrderSummary &order,
                                 const std::vector<TpchQ3ShipdateBucket> &pool,
                                 int32_t cutoff_days) {
  if (order.orderdate_days >= cutoff_days || order.bucket_count == 0)
    return 0;
  const auto begin = pool.begin() + static_cast<std::ptrdiff_t>(order.bucket_begin);
  const auto end =
      begin + static_cast<std::ptrdiff_t>(order.bucket_count);
  if (static_cast<int32_t>((end - 1)->shipdate) <= cutoff_days)
    return 0;
  const auto it = std::upper_bound(
      begin, end, cutoff_days,
      [](int32_t cutoff, const TpchQ3ShipdateBucket &bucket) {
        return cutoff < static_cast<int32_t>(bucket.shipdate);
      });
  if (it == end)
    return 0;
  return it->suffix_revenue;
}

std::vector<TpchQ3OutputRow>
ComposeTpchQ3TopK(const TpchQ3SegmentIntervalIndex &index,
                  int32_t cutoff_days) {
  const auto begin = std::lower_bound(
      index.orders.begin(), index.orders.end(), cutoff_days,
      [&index](const TpchQ3OrderSummary &order, int32_t cutoff) {
        return index.MaxShipdate(order) <= cutoff;
      });
  std::priority_queue<TpchQ3OutputRow, std::vector<TpchQ3OutputRow>, TopKLess>
      heap;
  for (auto it = begin; it != index.orders.end(); ++it) {
    if (it->orderdate_days >= cutoff_days)
      continue;
    const auto revenue_raw =
        TpchQ3RevenueAboveCutoff(*it, index.bucket_pool, cutoff_days);
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

} // namespace accelerator_codegen::aux_state::query
