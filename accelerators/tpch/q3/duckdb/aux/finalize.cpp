#include "tpch/q3/duckdb/aux/module.hpp"

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

// Best-first walk over the bins' (bound desc, orderkey asc) entry streams.
// Every order's revenue at any cutoff is <= its bound, so once the next
// bound is below the current 10th result nothing remaining can enter the
// top 10: the walk touches O(answer * bins) entries, not the whole segment.
struct Cursor {
  const TpchQ3BinList *bin = nullptr;
  std::size_t chunk = 0;
  std::size_t idx = 0;
  const TpchQ3Entry &Current() const { return bin->chunks[chunk]->entries[idx]; }
  const TpchQ3Chunk &CurrentChunk() const { return *bin->chunks[chunk]; }
  // Moves to the next entry; false when the bin is exhausted.
  bool Advance() {
    if (++idx < bin->chunks[chunk]->entries.size())
      return true;
    ++chunk;
    idx = 0;
    while (chunk < bin->chunks.size() && bin->chunks[chunk]->entries.empty())
      ++chunk;
    return chunk < bin->chunks.size();
  }
};

struct CursorWorse {
  // priority_queue keeps the "largest" at top: make it the best entry.
  bool operator()(const Cursor &a, const Cursor &b) const {
    const auto &x = a.Current();
    const auto &y = b.Current();
    if (x.bound != y.bound)
      return x.bound < y.bound;
    return x.orderkey > y.orderkey;
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

int64_t TpchQ3RevenueAboveCutoff(const TpchQ3Entry &order,
                                 const TpchQ3ShipdateBucket *buckets,
                                 int32_t cutoff_days) {
  if (static_cast<int32_t>(order.orderdate) >= cutoff_days ||
      order.bucket_count == 0)
    return 0;
  const auto *begin = buckets + order.bucket_begin;
  const auto *end = begin + order.bucket_count;
  if (static_cast<int32_t>((end - 1)->shipdate) <= cutoff_days)
    return 0;
  const auto *it = std::upper_bound(
      begin, end, cutoff_days,
      [](int32_t cutoff, const TpchQ3ShipdateBucket &bucket) {
        return cutoff < static_cast<int32_t>(bucket.shipdate);
      });
  if (it == end)
    return 0;
  return it->suffix_revenue;
}

std::vector<TpchQ3OutputRow> ComposeTpchQ3TopK(const TpchQ3Segment &segment,
                                               int32_t cutoff_days) {
  std::priority_queue<Cursor, std::vector<Cursor>, CursorWorse> cursors;
  for (const auto &[bin_id, bin] : segment.bins) {
    if (!bin || bin->chunks.empty())
      continue;
    const int32_t bin_start = static_cast<int32_t>(bin_id) << kTpchQ3BinShift;
    if (bin_start >= cutoff_days ||
        static_cast<int32_t>(bin->max_maxship) <= cutoff_days)
      continue;
    Cursor cursor;
    cursor.bin = bin.get();
    while (cursor.chunk < bin->chunks.size() &&
           bin->chunks[cursor.chunk]->entries.empty())
      ++cursor.chunk;
    if (cursor.chunk < bin->chunks.size())
      cursors.push(cursor);
  }

  std::priority_queue<TpchQ3OutputRow, std::vector<TpchQ3OutputRow>, TopKLess>
      heap;
  while (!cursors.empty()) {
    Cursor cursor = cursors.top();
    cursors.pop();
    const TpchQ3Entry &entry = cursor.Current();
    if (heap.size() >= kTopK && entry.bound < heap.top().revenue_raw)
      break; // every remaining entry has bound <= this one
    if (static_cast<int32_t>(entry.orderdate) < cutoff_days &&
        static_cast<int32_t>(entry.maxship) > cutoff_days) {
      const auto revenue_raw = TpchQ3RevenueAboveCutoff(
          entry, cursor.CurrentChunk().pool.data(), cutoff_days);
      if (revenue_raw != 0) {
        TpchQ3OutputRow row;
        row.orderkey = entry.orderkey;
        row.revenue_raw = revenue_raw;
        row.orderdate_days = entry.orderdate;
        row.shippriority = entry.shippriority;
        if (heap.size() < kTopK) {
          heap.push(row);
        } else if (BetterRow(row, heap.top())) {
          heap.pop();
          heap.push(row);
        }
      }
    }
    if (cursor.Advance())
      cursors.push(cursor);
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
