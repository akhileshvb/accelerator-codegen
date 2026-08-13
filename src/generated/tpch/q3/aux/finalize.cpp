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

void FinalizeTpchQ3OrderSummary(TpchQ3OrderSummary &summary,
                                const std::map<int32_t, int64_t> &day_revenue) {
  summary.buckets.clear();
  for (const auto &entry : day_revenue) {
    if (entry.second != 0)
      summary.buckets.push_back({entry.first, 0});
  }
  if (summary.buckets.empty()) {
    summary.max_shipdate = 0;
    summary.buckets.shrink_to_fit();
    return;
  }
  std::sort(summary.buckets.begin(), summary.buckets.end(),
            [](const TpchQ3ShipdateBucket &lhs, const TpchQ3ShipdateBucket &rhs) {
              return lhs.shipdate < rhs.shipdate;
            });
  summary.max_shipdate = summary.buckets.back().shipdate;
  int64_t running = 0;
  for (int index = static_cast<int>(summary.buckets.size()) - 1; index >= 0;
       --index) {
    running += day_revenue.at(summary.buckets[static_cast<std::size_t>(index)].shipdate);
    summary.buckets[static_cast<std::size_t>(index)].suffix_revenue = running;
  }
  summary.buckets.shrink_to_fit();
}

int64_t TpchQ3RevenueAboveCutoff(const TpchQ3OrderSummary &order,
                                 int32_t cutoff_days) {
  if (order.orderdate_days >= cutoff_days || order.max_shipdate <= cutoff_days)
    return 0;
  const auto it = std::upper_bound(
      order.buckets.begin(), order.buckets.end(), cutoff_days,
      [](int32_t cutoff, const TpchQ3ShipdateBucket &bucket) {
        return cutoff < bucket.shipdate;
      });
  if (it == order.buckets.end())
    return 0;
  return it->suffix_revenue;
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

} // namespace accelerator_codegen::aux_state::query
