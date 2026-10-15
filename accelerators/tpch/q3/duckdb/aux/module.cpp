#include "tpch/q3/duckdb/aux/module.hpp"
#include "tpch/q3/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
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
                         const std::shared_ptr<const TpchQ3Segment> &state) {
  auto image = std::make_shared<TpchQ3StateImage>();
  image->kind = TpchQ3ImageKind::SegmentIntervalIndex;
  image->segment = segment;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ3StateImage>
MakeOrderSegmentSideImage(const std::shared_ptr<const TpchQ3Side> &side) {
  auto image = std::make_shared<TpchQ3StateImage>();
  image->kind = TpchQ3ImageKind::OrderSegmentSide;
  image->side = side;
  return image;
}

const TpchQ3StateImage *AsQ3Image(const core::StateImage &image,
                                  TpchQ3ImageKind kind) {
  const auto *payload = image.As<TpchQ3StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  if (kind == TpchQ3ImageKind::OrderSegmentSide && !payload->side)
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

inline bool EntryBefore(const TpchQ3Entry &a, int64_t bound, uint32_t orderkey) {
  return a.bound > bound || (a.bound == bound && a.orderkey < orderkey);
}

inline uint16_t BinOf(uint16_t orderdate) {
  return static_cast<uint16_t>(orderdate >> kTpchQ3BinShift);
}

// Per-shipdate revenue of one stored entry (inverse of the suffix encoding).
std::map<int32_t, int64_t> DayRevenueOf(const TpchQ3Chunk &chunk,
                                        const TpchQ3Entry &entry) {
  std::map<int32_t, int64_t> day_revenue;
  const auto begin = static_cast<std::size_t>(entry.bucket_begin);
  for (std::size_t i = 0; i < entry.bucket_count; ++i) {
    const auto &bucket = chunk.pool[begin + i];
    const int64_t next_suffix =
        (i + 1 < entry.bucket_count) ? chunk.pool[begin + i + 1].suffix_revenue
                                     : 0;
    const int64_t revenue = bucket.suffix_revenue - next_suffix;
    if (revenue != 0)
      day_revenue.emplace(static_cast<int32_t>(bucket.shipdate), revenue);
  }
  return day_revenue;
}

// Upper bound of the order's revenue for any cutoff: sum of positive days.
int64_t BoundOf(const std::map<int32_t, int64_t> &day_revenue) {
  int64_t bound = 0;
  for (const auto &entry : day_revenue)
    if (entry.second > 0)
      bound += entry.second;
  return bound;
}

// Rebuilds a chunk from entries (already in key order) whose buckets live in
// `src_pool`; the new chunk gets a compact private pool.
std::shared_ptr<TpchQ3Chunk>
MakeChunk(const TpchQ3Entry *entries, std::size_t count,
          const TpchQ3ShipdateBucket *src_pool) {
  auto chunk = std::make_shared<TpchQ3Chunk>();
  chunk->entries.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    TpchQ3Entry e = entries[i];
    const auto new_begin = static_cast<uint32_t>(chunk->pool.size());
    chunk->pool.insert(chunk->pool.end(), src_pool + e.bucket_begin,
                       src_pool + e.bucket_begin + e.bucket_count);
    e.bucket_begin = new_begin;
    chunk->max_maxship = std::max(chunk->max_maxship, e.maxship);
    chunk->entries.push_back(e);
  }
  return chunk;
}

// Builds an immutable bin from key-sorted entries (buckets in src_pool).
std::shared_ptr<const TpchQ3BinList>
MakeBinList(const std::vector<TpchQ3Entry> &sorted,
            const std::vector<TpchQ3ShipdateBucket> &src_pool) {
  auto bin = std::make_shared<TpchQ3BinList>();
  for (std::size_t i = 0; i < sorted.size(); i += kTpchQ3ChunkTarget) {
    const auto n = std::min(kTpchQ3ChunkTarget, sorted.size() - i);
    auto chunk = MakeChunk(sorted.data() + i, n, src_pool.data());
    bin->max_maxship = std::max(bin->max_maxship, chunk->max_maxship);
    bin->chunks.push_back(std::move(chunk));
  }
  return bin;
}

// Copy-on-write writer over a published segment: only the touched chunks (and
// the chunk-pointer arrays of the touched bins) are copied.  Bins are
// independent, so after PrepareBin() (single-threaded) distinct bins may be
// updated concurrently through TakeIn()/InsertIn().
class Q3SegmentTxn {
public:
  struct Bin {
    std::vector<std::shared_ptr<const TpchQ3Chunk>> chunks;
    std::vector<char> owned;
    // Separators: the last (bound, orderkey) of every chunk, contiguous so
    // the chunk search does not chase chunk pointers.
    std::vector<std::pair<int64_t, uint32_t>> lasts;
    bool touched = false;
    // Chunks are copied from the published list on first use, inside the
    // task that owns the bin (so the O(chunks) copy runs in parallel).
    const TpchQ3BinList *base_list = nullptr;
    bool materialized = false;
  };

  Q3SegmentTxn(core::PartitionId segment,
               std::shared_ptr<const TpchQ3Segment> base)
      : segment_(segment), base_(std::move(base)) {}

  // The txn's working copy of a bin (nullptr: absent and !create).  Not
  // thread-safe; the returned pointer stays valid for the txn's lifetime.
  Bin *PrepareBin(uint16_t bin_id, bool create) {
    return FindBin(bin_id, create);
  }

  // Removes the entry; returns its per-shipdate revenue. False if absent.
  static bool TakeIn(Bin &bin, uint16_t orderdate, int64_t bound,
                     uint32_t orderkey, std::map<int32_t, int64_t> &day_revenue) {
    (void)orderdate;
    Materialize(bin);
    const std::size_t ci = ChunkFor(bin, bound, orderkey);
    if (ci >= bin.chunks.size())
      return false;
    const auto &chunk = *bin.chunks[ci];
    auto it = std::lower_bound(
        chunk.entries.begin(), chunk.entries.end(), 0,
        [&](const TpchQ3Entry &e, int) {
          return EntryBefore(e, bound, orderkey);
        });
    if (it == chunk.entries.end() || it->orderkey != orderkey ||
        it->bound != bound)
      return false;
    day_revenue = DayRevenueOf(chunk, *it);
    const auto pos = it - chunk.entries.begin();
    auto &mutable_chunk = Mutable(bin, ci);
    mutable_chunk.entries.erase(mutable_chunk.entries.begin() + pos);
    if (mutable_chunk.entries.empty()) {
      bin.chunks.erase(bin.chunks.begin() + static_cast<std::ptrdiff_t>(ci));
      bin.owned.erase(bin.owned.begin() + static_cast<std::ptrdiff_t>(ci));
      bin.lasts.erase(bin.lasts.begin() + static_cast<std::ptrdiff_t>(ci));
    } else if (static_cast<std::size_t>(pos) == mutable_chunk.entries.size()) {
      const auto &last = mutable_chunk.entries.back();
      bin.lasts[ci] = {last.bound, last.orderkey};
    }
    bin.touched = true;
    return true;
  }

  static void InsertIn(Bin &bin, const TpchQ3Entry &entry_in,
                       const std::vector<TpchQ3ShipdateBucket> &buckets) {
    Materialize(bin);
    std::size_t ci = ChunkFor(bin, entry_in.bound, entry_in.orderkey);
    if (bin.chunks.empty()) {
      bin.chunks.push_back(std::make_shared<TpchQ3Chunk>());
      bin.owned.push_back(1);
      bin.lasts.emplace_back(0, 0);
      ci = 0;
    } else if (ci >= bin.chunks.size()) {
      ci = bin.chunks.size() - 1;
    }
    auto &chunk = Mutable(bin, ci);
    TpchQ3Entry entry = entry_in;
    entry.bucket_begin = static_cast<uint32_t>(chunk.pool.size());
    entry.bucket_count = static_cast<uint16_t>(buckets.size());
    entry.maxship = buckets.empty() ? 0 : buckets.back().shipdate;
    chunk.pool.insert(chunk.pool.end(), buckets.begin(), buckets.end());
    auto it = std::lower_bound(
        chunk.entries.begin(), chunk.entries.end(), 0,
        [&](const TpchQ3Entry &e, int) {
          return EntryBefore(e, entry.bound, entry.orderkey);
        });
    chunk.entries.insert(it, entry);
    if (chunk.entries.size() > kTpchQ3ChunkMax) {
      const std::size_t half = chunk.entries.size() / 2;
      auto lo = MakeChunk(chunk.entries.data(), half, chunk.pool.data());
      auto hi = MakeChunk(chunk.entries.data() + half,
                          chunk.entries.size() - half, chunk.pool.data());
      bin.chunks[ci] = std::move(lo);
      bin.chunks.insert(bin.chunks.begin() + static_cast<std::ptrdiff_t>(ci) + 1,
                        std::move(hi));
      bin.owned[ci] = 1;
      bin.owned.insert(bin.owned.begin() + static_cast<std::ptrdiff_t>(ci) + 1, 1);
      bin.lasts.insert(bin.lasts.begin() + static_cast<std::ptrdiff_t>(ci), {0, 0});
      SetLast(bin, ci);
      SetLast(bin, ci + 1);
    } else {
      SetLast(bin, ci);
    }
    bin.touched = true;
  }

  // Single-threaded convenience wrappers.
  bool Take(uint16_t orderdate, int64_t bound, uint32_t orderkey,
            std::map<int32_t, int64_t> &day_revenue) {
    Bin *bin = FindBin(BinOf(orderdate), false);
    return bin && TakeIn(*bin, orderdate, bound, orderkey, day_revenue);
  }
  void Insert(const TpchQ3Entry &entry,
              const std::vector<TpchQ3ShipdateBucket> &buckets) {
    InsertIn(*FindBin(BinOf(entry.orderdate), true), entry, buckets);
  }

  bool Dirty() const {
    for (const auto &kv : bins_)
      if (kv.second.touched)
        return true;
    return false;
  }

  // `parallel_for(n, fn)` runs fn(0..n) (possibly on several threads).
  template <class ParallelFor>
  std::shared_ptr<const TpchQ3Segment> Finish(ParallelFor &&parallel_for) {
    auto out = std::make_shared<TpchQ3Segment>();
    out->segment = segment_;
    if (base_)
      out->bins = base_->bins;
    std::vector<std::pair<uint16_t, Bin *>> touched;
    for (auto &[bin_id, bin] : bins_)
      if (bin.touched)
        touched.emplace_back(bin_id, &bin);
    std::vector<std::shared_ptr<TpchQ3BinList>> lists(touched.size());
    parallel_for(touched.size(), [&](std::size_t t) {
      Bin &bin = *touched[t].second;
      if (bin.chunks.empty())
        return;
      auto list = std::make_shared<TpchQ3BinList>();
      list->chunks.reserve(bin.chunks.size());
      for (std::size_t i = 0; i < bin.chunks.size(); ++i) {
        if (bin.owned[i])
          Seal(const_cast<TpchQ3Chunk &>(*bin.chunks[i]));
        list->max_maxship = std::max(list->max_maxship, bin.chunks[i]->max_maxship);
        list->chunks.push_back(bin.chunks[i]);
      }
      lists[t] = std::move(list);
    });
    for (std::size_t t = 0; t < touched.size(); ++t) {
      if (!lists[t])
        out->bins.erase(touched[t].first);
      else
        out->bins[touched[t].first] = std::move(lists[t]);
    }
    return out;
  }

private:
  Bin *FindBin(uint16_t bin_id, bool create) {
    auto it = bins_.find(bin_id);
    if (it != bins_.end())
      return &it->second;
    Bin bin;
    if (base_) {
      auto base_it = base_->bins.find(bin_id);
      if (base_it != base_->bins.end() && base_it->second &&
          !base_it->second->chunks.empty())
        bin.base_list = base_it->second.get();
    }
    if (!bin.base_list && !create)
      return nullptr;
    return &bins_.emplace(bin_id, std::move(bin)).first->second;
  }

  static void Materialize(Bin &bin) {
    if (bin.materialized)
      return;
    bin.materialized = true;
    if (!bin.base_list)
      return;
    bin.chunks = bin.base_list->chunks;
    bin.owned.assign(bin.chunks.size(), 0);
    bin.lasts.resize(bin.chunks.size());
    for (std::size_t i = 0; i < bin.chunks.size(); ++i) {
      const auto &entries = bin.chunks[i]->entries;
      bin.lasts[i] = entries.empty()
                         ? std::pair<int64_t, uint32_t>{0, 0}
                         : std::pair<int64_t, uint32_t>{entries.back().bound,
                                                        entries.back().orderkey};
    }
  }

  static void SetLast(Bin &bin, std::size_t ci) {
    const auto &entries = bin.chunks[ci]->entries;
    if (!entries.empty())
      bin.lasts[ci] = {entries.back().bound, entries.back().orderkey};
  }

  // First chunk whose last entry does not sort before (bound, orderkey).
  static std::size_t ChunkFor(const Bin &bin, int64_t bound, uint32_t orderkey) {
    std::size_t lo = 0, hi = bin.lasts.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      const auto &last = bin.lasts[mid];
      if (last.first > bound || (last.first == bound && last.second < orderkey))
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  // Chunks this txn created are private (made via make_shared<TpchQ3Chunk>),
  // so mutating them through the const handle is well-defined.
  static TpchQ3Chunk &Mutable(Bin &bin, std::size_t ci) {
    if (!bin.owned[ci]) {
      bin.chunks[ci] = std::make_shared<TpchQ3Chunk>(*bin.chunks[ci]);
      bin.owned[ci] = 1;
    }
    return const_cast<TpchQ3Chunk &>(*bin.chunks[ci]);
  }

  static void Seal(TpchQ3Chunk &chunk) {
    uint16_t max_ship = 0;
    std::size_t live = 0;
    for (const auto &e : chunk.entries) {
      max_ship = std::max(max_ship, e.maxship);
      live += e.bucket_count;
    }
    chunk.max_maxship = max_ship;
    if (chunk.pool.size() > live * 2 + 32) {
      std::vector<TpchQ3ShipdateBucket> pool;
      pool.reserve(live);
      for (auto &e : chunk.entries) {
        const auto new_begin = static_cast<uint32_t>(pool.size());
        pool.insert(pool.end(), chunk.pool.begin() + e.bucket_begin,
                    chunk.pool.begin() + e.bucket_begin + e.bucket_count);
        e.bucket_begin = new_begin;
      }
      chunk.pool = std::move(pool);
    }
  }

  core::PartitionId segment_;
  std::shared_ptr<const TpchQ3Segment> base_;
  std::map<uint16_t, Bin> bins_;
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

std::vector<uint8_t> EncodeTpchQ3OrderSegmentSide(const TpchQ3Side &side) {
  uint32_t order_count = 0, cust_count = 0;
  side.orders.ForEach([&](const TpchQ3OrderRec &) { ++order_count; });
  side.custs.ForEach([&](const TpchQ3CustRec &) { ++cust_count; });
  ByteWriter writer;
  core::WriteImageHeader(writer, kOrderSegmentMagic, kVersion, order_count);
  side.orders.ForEach([&](const TpchQ3OrderRec &rec) {
    writer.U32(rec.key);
    writer.U32(rec.orderdate);
    writer.U8(rec.seg_flags);
    writer.U8(rec.shippriority);
    writer.I64(rec.bound);
  });
  writer.U32(cust_count);
  side.custs.ForEach([&](const TpchQ3CustRec &rec) {
    writer.U32(rec.key);
    writer.U32(rec.orphan_orders);
    writer.U8(rec.segment);
  });
  return std::move(writer.bytes);
}

std::vector<uint8_t>
EncodeTpchQ3SegmentIntervalIndex(const TpchQ3Segment &segment) {
  uint32_t count = 0;
  for (const auto &bin : segment.bins)
    for (const auto &chunk : bin.second->chunks)
      count += static_cast<uint32_t>(chunk->entries.size());
  ByteWriter writer;
  core::WriteImageHeader(writer, kIntervalMagic, kVersion, count);
  for (const auto &bin : segment.bins) {
    for (const auto &chunk : bin.second->chunks) {
      for (const auto &order : chunk->entries) {
        writer.I64(order.orderkey);
        writer.I32(order.orderdate);
        writer.I32(order.shippriority);
        writer.I32(order.maxship);
        writer.U32(order.bucket_count);
        for (uint32_t b = 0; b < order.bucket_count; ++b) {
          const auto &bucket = chunk->pool[order.bucket_begin + b];
          writer.I32(bucket.shipdate);
          writer.I64(bucket.suffix_revenue);
        }
      }
    }
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ3OrderSegmentSide(const std::vector<uint8_t> &bytes,
                                  TpchQ3Side &side) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kOrderSegmentMagic, kVersion, count))
    return false;
  TpchQ3ShardIndex<TpchQ3OrderRec>::Builder orders;
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ3OrderRec rec;
    uint32_t orderdate = 0;
    if (!reader.U32(rec.key) || !reader.U32(orderdate) ||
        !reader.U8(rec.seg_flags) || !reader.U8(rec.shippriority) ||
        !reader.I64(rec.bound))
      return false;
    rec.orderdate = static_cast<uint16_t>(orderdate);
    orders.Add(rec);
  }
  uint32_t cust_count = 0;
  if (!reader.U32(cust_count))
    return false;
  TpchQ3ShardIndex<TpchQ3CustRec>::Builder custs;
  for (uint32_t i = 0; i < cust_count; ++i) {
    TpchQ3CustRec rec;
    if (!reader.U32(rec.key) || !reader.U32(rec.orphan_orders) ||
        !reader.U8(rec.segment))
      return false;
    custs.Add(rec);
  }
  orders.Sort();
  custs.Sort();
  side.orders = orders.Finish();
  side.custs = custs.Finish();
  return reader.offset == bytes.size();
}

bool DecodeTpchQ3SegmentIntervalIndex(const std::vector<uint8_t> &bytes,
                                      core::PartitionId segment_id,
                                      TpchQ3Segment &segment) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kIntervalMagic, kVersion, count))
    return false;
  segment.segment = segment_id;
  segment.bins.clear();
  std::map<uint16_t, std::pair<std::vector<TpchQ3Entry>,
                               std::vector<TpchQ3ShipdateBucket>>>
      staging;
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ3Entry order;
    uint32_t bucket_count = 0;
    int64_t orderkey = 0;
    int32_t orderdate_days = 0, shippriority = 0, max_shipdate = 0;
    if (!reader.I64(orderkey) || !reader.I32(orderdate_days) ||
        !reader.I32(shippriority) || !reader.I32(max_shipdate) ||
        !reader.U32(bucket_count))
      return false;
    order.orderkey = PackTpchQ3Orderkey(orderkey);
    order.orderdate = PackTpchQ3Date(orderdate_days);
    order.shippriority = PackTpchQ3Shippriority(shippriority);
    order.maxship = PackTpchQ3Date(max_shipdate);
    auto &stage = staging[BinOf(order.orderdate)];
    order.bucket_begin = static_cast<uint32_t>(stage.second.size());
    order.bucket_count = static_cast<uint16_t>(bucket_count);
    int64_t bound = 0, prev_suffix = 0;
    std::vector<TpchQ3ShipdateBucket> buckets;
    for (uint32_t b = 0; b < bucket_count; ++b) {
      int32_t shipdate = 0;
      TpchQ3ShipdateBucket bucket;
      if (!reader.I32(shipdate) || !reader.I64(bucket.suffix_revenue))
        return false;
      bucket.shipdate = PackTpchQ3Date(shipdate);
      buckets.push_back(bucket);
    }
    // day revenue = suffix[i] - suffix[i+1]
    for (std::size_t b = 0; b < buckets.size(); ++b) {
      const int64_t next =
          b + 1 < buckets.size() ? buckets[b + 1].suffix_revenue : 0;
      const int64_t day = buckets[b].suffix_revenue - next;
      if (day > 0)
        bound += day;
    }
    (void)prev_suffix;
    order.bound = bound;
    stage.second.insert(stage.second.end(), buckets.begin(), buckets.end());
    stage.first.push_back(order);
  }
  for (auto &[bin_id, stage] : staging) {
    std::sort(stage.first.begin(), stage.first.end(),
              [](const TpchQ3Entry &a, const TpchQ3Entry &b) {
                return EntryBefore(a, b.bound, b.orderkey);
              });
    segment.bins[bin_id] = MakeBinList(stage.first, stage.second);
  }
  return reader.offset == bytes.size();
}

bool TpchQ3TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ3TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
  customer_deltas.clear();
}
bool TpchQ3TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ3LineitemDelta> &TpchQ3TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ3OrderDelta> &TpchQ3TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
const std::vector<TpchQ3CustomerDelta> &TpchQ3TxnBuffer::CustomerDeltas() const {
  return customer_deltas;
}
void TpchQ3TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
  customer_deltas.clear();
}
void TpchQ3TxnBuffer::MarkNoop() { pending = true; }
void TpchQ3TxnBuffer::AddDelta(TpchQ3LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ3TxnBuffer::AddOrderDelta(TpchQ3OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}
void TpchQ3TxnBuffer::AddCustomerDelta(TpchQ3CustomerDelta delta) {
  pending = true;
  customer_deltas.push_back(delta);
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
  auto built = BuildState(services);
  auto side = std::make_shared<TpchQ3Side>(std::move(built.side));
  builder.Write(core::StateImage::Make(
      core::TpchQ3Layout::kAccelerator, core::TpchQ3Layout::kOrderSegmentSide, 0,
      MakeOrderSegmentSideImage(side)));
  for (core::PartitionId segment = core::TpchQ3Layout::kFirstSegment;
       segment <= core::TpchQ3Layout::kLastSegment; ++segment) {
    auto it = built.segments.find(segment);
    std::shared_ptr<const TpchQ3Segment> state;
    if (it != built.segments.end()) {
      state = it->second;
    } else {
      auto empty = std::make_shared<TpchQ3Segment>();
      empty->segment = segment;
      state = empty;
    }
    builder.Write(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, segment, 0,
        MakeSegmentIntervalImage(segment, state)));
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
  // Customer writes are applied precisely (or mark a rebuild) in
  // AccumulateMutation; they need no blanket invalidation.
  return {};
}

void TpchQ3Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ3TxnBuffer &>(base_buffer);
  if (mutation.table == config.customer_table) {
    // Projected row: (c_mktsegment, c_custkey).
    auto read = [&](const core::AuxRow &row, int64_t &custkey,
                    core::PartitionId &segment) -> bool {
      std::string mkt;
      if (!StringCell(row, 0, mkt) || !IntCell(row, 1, custkey) || custkey < 0 ||
          custkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
        return false;
      segment = TpchQ3PartitionFromMktsegment(mkt);
      return true;
    };
    using CKind = TpchQ3CustomerDelta::Kind;
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        int64_t key = 0;
        core::PartitionId segment = 0;
        if (!read(row, key, segment)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddCustomerDelta({CKind::Insert, key, segment});
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        int64_t key = 0;
        core::PartitionId segment = 0;
        if (!read(row, key, segment)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddCustomerDelta({CKind::Delete, key, 0});
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, nk = 0;
        core::PartitionId os = 0, ns = 0;
        if (!read(mutation.old_rows[i], ok, os) ||
            !read(mutation.new_rows[i], nk, ns)) {
          buffer.MarkRebuild();
          return;
        }
        if (ok == nk && os == ns) {
          buffer.MarkNoop();
        } else if (ok == nk) {
          buffer.AddCustomerDelta({CKind::Move, nk, ns});
        } else {
          buffer.AddCustomerDelta({CKind::Delete, ok, 0});
          buffer.AddCustomerDelta({CKind::Insert, nk, ns});
        }
      }
      break;
    }
    return;
  }
  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0, custkey = 0;
      int32_t orderdate = 0, shippriority = 0;
      if (!ReadOrder(row, orderkey, orderdate, custkey, shippriority) ||
          orderkey < 0 ||
          orderkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
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
    if (!ReadLineitem(row, input) || input.orderkey < 0 ||
        input.orderkey >
            static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
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
  auto rebuild_all = [&]() {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.updates.clear();
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  };
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

  const auto total_deltas = buffer.Deltas().size() +
                            buffer.OrderDeltas().size() +
                            buffer.CustomerDeltas().size();
  // RF-sized batches are always applied incrementally; the cost model is
  // only consulted for bulk changes.
  if (total_deltas > 256) {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(total_deltas), 1, true, false, true, false,
         false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      outcome.action = decision.action == core::MaintenanceAction::RebuildPartition
                           ? core::MaintenanceAction::RebuildAll
                           : decision.action;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }

  // One consistent snapshot of the side partition and all segment partitions.
  std::vector<core::PartitionId> partitions{core::TpchQ3Layout::kOrderSegmentSide};
  for (core::PartitionId s = core::TpchQ3Layout::kFirstSegment;
       s <= core::TpchQ3Layout::kLastSegment; ++s)
    partitions.push_back(s);
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  std::shared_ptr<const TpchQ3Side> side;
  core::Version side_version = 0;
  std::map<core::PartitionId, std::shared_ptr<const TpchQ3Segment>> seg_base;
  std::map<core::PartitionId, core::Version> seg_version;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ3Layout::kOrderSegmentSide) {
      const auto *typed = AsQ3Image(image, TpchQ3ImageKind::OrderSegmentSide);
      if (!typed)
        return rebuild_all();
      side = typed->side;
      side_version = image.version;
    } else if (core::TpchQ3Layout::IsSegmentPartition(image.partition)) {
      const auto *typed = AsQ3Image(image, TpchQ3ImageKind::SegmentIntervalIndex);
      if (!typed)
        return rebuild_all();
      seg_base[image.partition] = typed->state;
      seg_version[image.partition] = image.version;
    }
  }
  if (!side)
    return rebuild_all();


  TpchQ3ShardIndex<TpchQ3OrderRec>::Txn otx(side->orders);
  TpchQ3ShardIndex<TpchQ3CustRec>::Txn ctx(side->custs);
  std::map<core::PartitionId, Q3SegmentTxn> seg_txns;
  auto segtx = [&](core::PartitionId segment) -> Q3SegmentTxn & {
    auto it = seg_txns.find(segment);
    if (it == seg_txns.end()) {
      auto base = seg_base.find(segment);
      it = seg_txns
               .emplace(segment,
                        Q3SegmentTxn(segment, base != seg_base.end()
                                                  ? base->second
                                                  : nullptr))
               .first;
    }
    return it->second;
  };

  // Orders referencing a key that is not a tracked customer are untracked;
  // the key's record counts them so a later customer insert / segment change
  // knows whether a lookup is needed.
  auto adjust_orphans = [&](int64_t custkey, int32_t delta) {
    if (custkey < 0 ||
        custkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
      return;
    const auto key = static_cast<uint32_t>(custkey);
    const auto *rec = ctx.Find(key);
    if (rec) {
      if (rec->segment != 0) {
        return;
      }
      TpchQ3CustRec next = *rec;
      const int64_t count = static_cast<int64_t>(next.orphan_orders) + delta;
      next.orphan_orders = count > 0 ? static_cast<uint32_t>(count) : 0;
      ctx.Upsert(next);
    } else if (delta > 0) {
      TpchQ3CustRec next;
      next.key = key;
      next.orphan_orders = static_cast<uint32_t>(delta);
      ctx.Upsert(next);
    }
  };
  auto tracked_customer = [&](int64_t custkey) -> const TpchQ3CustRec * {
    if (custkey < 0 ||
        custkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
      return nullptr;
    const auto *rec = ctx.Find(static_cast<uint32_t>(custkey));
    return rec && rec->segment != 0 ? rec : nullptr;
  };

  // Inserts the order's index entry (if it has lineitems) into its segment.
  auto place_order = [&](TpchQ3OrderRec &rec,
                         const std::map<int32_t, int64_t> &day) {
    rec.seg_flags = static_cast<uint8_t>(rec.seg_flags & 0x7F);
    rec.bound = 0;
    if (day.empty())
      return;
    std::vector<TpchQ3ShipdateBucket> buckets;
    FinalizeTpchQ3OrderSummary(buckets, day);
    if (buckets.empty())
      return;
    TpchQ3Entry entry;
    entry.bound = BoundOf(day);
    entry.orderkey = rec.key;
    entry.orderdate = rec.orderdate;
    entry.shippriority = rec.shippriority;
    rec.bound = entry.bound;
    rec.seg_flags = static_cast<uint8_t>(rec.seg_flags | 0x80);
    segtx(rec.segment()).Insert(entry, buckets);
  };

  // ---- Order inserts / deletes / lineitem changes, per key -----------------
  // A key both deleted and inserted is an UPDATE: the order and its lineitems
  // stay.  Three steps: (A) a sequential pass over the sorted keys decides
  // each key's fate against the side index; (B) the segment-index work is done
  // bin-parallel (takes, then entry computation, then inserts: bins are
  // independent copy-on-write units); (C) the side index is updated.
  using OrderPtr = std::pair<uint32_t, const TpchQ3OrderDelta *>;
  std::vector<OrderPtr> ins_v, del_v;
  for (const auto &delta : buffer.OrderDeltas()) {
    const auto key = static_cast<uint32_t>(delta.orderkey);
    (delta.sign > 0 ? ins_v : del_v).emplace_back(key, &delta);
  }
  auto by_key = [](const OrderPtr &a, const OrderPtr &b) { return a.first < b.first; };
  std::stable_sort(ins_v.begin(), ins_v.end(), by_key);
  std::stable_sort(del_v.begin(), del_v.end(), by_key);
  for (std::size_t i = 1; i < ins_v.size(); ++i)
    if (ins_v[i].first == ins_v[i - 1].first)
      return rebuild_all(); // duplicate inserted key
  // Net per-(key, day) revenue of the lineitem deltas, sorted by (key, day).
  // Mutations usually arrive grouped by order: sort only when they do not.
  struct Line {
    uint32_t key;
    int32_t day;
    int64_t revenue;
  };
  std::vector<Line> lines;
  lines.reserve(buffer.Deltas().size());
  for (const auto &delta : buffer.Deltas())
    lines.push_back({static_cast<uint32_t>(delta.orderkey), delta.shipdate_day,
                     static_cast<int64_t>(delta.sign) * delta.revenue_raw});

  const auto line_less = [](const Line &a, const Line &b) {
    return a.key != b.key ? a.key < b.key : a.day < b.day;
  };
  if (!std::is_sorted(lines.begin(), lines.end(), line_less)) {
    if (!std::is_sorted(lines.begin(), lines.end(),
                        [](const Line &a, const Line &b) { return a.key < b.key; }))
      std::stable_sort(lines.begin(), lines.end(),
                       [](const Line &a, const Line &b) { return a.key < b.key; });
    for (std::size_t i = 0; i < lines.size();) {
      std::size_t j = i + 1;
      while (j < lines.size() && lines[j].key == lines[i].key)
        ++j;
      std::sort(lines.begin() + static_cast<std::ptrdiff_t>(i),
                lines.begin() + static_cast<std::ptrdiff_t>(j),
                [](const Line &a, const Line &b) { return a.day < b.day; });
      i = j;
    }
  }
  {
    std::size_t out = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (out > 0 && lines[out - 1].key == lines[i].key &&
          lines[out - 1].day == lines[i].day)
        lines[out - 1].revenue += lines[i].revenue;
      else
        lines[out++] = lines[i];
    }
    lines.resize(out);
  }

  const std::size_t workers = std::max<std::size_t>(
      1, std::min<std::size_t>(services.WorkerCount(), 8));
  // Runs fn(0..n) on up to `workers` threads.
  auto parallel_for = [&](std::size_t n, auto &&fn) {
    const std::size_t threads = std::min(workers, n);
    if (threads <= 1) {
      for (std::size_t i = 0; i < n; ++i)
        fn(i);
      return;
    }
    std::atomic<std::size_t> next{0};
    auto body = [&]() {
      for (;;) {
        const std::size_t i = next.fetch_add(1);
        if (i >= n)
          return;
        fn(i);
      }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (std::size_t t = 1; t < threads; ++t)
      pool.emplace_back(body);
    body();
    for (auto &th : pool)
      th.join();
  };

  struct Work {
    uint32_t key = 0;
    bool had = false;
    bool take = false;   // remove the old index entry
    bool exists = false; // the order is (still) tracked afterwards
    bool place = false;  // an index entry was computed
    TpchQ3OrderRec old_rec;
    TpchQ3OrderRec rec;
    uint32_t line_begin = 0, line_end = 0;
    std::map<int32_t, int64_t> day;
    TpchQ3Entry entry;
    std::vector<TpchQ3ShipdateBucket> buckets;
    Q3SegmentTxn::Bin *take_bin = nullptr;
    Q3SegmentTxn::Bin *put_bin = nullptr;
  };
  std::vector<Work> works;
  works.reserve(ins_v.size() + del_v.size() + 16);
  // Side-index lookups for every key, in parallel (the index is not written
  // until step C, so published leaves are stable).
  std::vector<uint32_t> all_keys;
  all_keys.reserve(ins_v.size() + del_v.size() + lines.size());
  for (const auto &e : ins_v)
    all_keys.push_back(e.first);
  for (const auto &e : del_v)
    all_keys.push_back(e.first);
  for (const auto &e : lines)
    all_keys.push_back(e.key);
  std::sort(all_keys.begin(), all_keys.end());
  all_keys.erase(std::unique(all_keys.begin(), all_keys.end()), all_keys.end());
  std::vector<const TpchQ3OrderRec *> found_recs(all_keys.size());
  parallel_for((all_keys.size() + 1023) / 1024, [&](std::size_t r) {
    const std::size_t end = std::min(all_keys.size(), (r + 1) * 1024);
    for (std::size_t i = r * 1024; i < end; ++i)
      found_recs[i] = otx.Find(all_keys[i]);
  });

  {
    std::size_t ki = 0;
    std::size_t ii = 0, di = 0, li = 0;
    while (ii < ins_v.size() || di < del_v.size() || li < lines.size()) {
      uint32_t key = std::numeric_limits<uint32_t>::max();
      if (ii < ins_v.size())
        key = std::min(key, ins_v[ii].first);
      if (di < del_v.size())
        key = std::min(key, del_v[di].first);
      if (li < lines.size())
        key = std::min(key, lines[li].key);
      const TpchQ3OrderDelta *ins = nullptr, *del = nullptr;
      if (ii < ins_v.size() && ins_v[ii].first == key)
        ins = ins_v[ii++].second;
      if (di < del_v.size() && del_v[di].first == key) {
        del = del_v[di].second; // first delete of a key wins
        while (di < del_v.size() && del_v[di].first == key)
          ++di;
      }
      uint32_t lbeg = static_cast<uint32_t>(li);
      while (li < lines.size() && lines[li].key == key)
        ++li;
      const uint32_t lend = static_cast<uint32_t>(li);

      while (all_keys[ki] != key)
        ++ki;
      const auto *found = found_recs[ki];
      const bool had = found != nullptr;
      const bool is_ins = ins != nullptr, is_del = del != nullptr;
      if (!had && !is_ins) {
        // lineitem change / delete of an order outside every tracked segment
        if (is_del)
          adjust_orphans(del->custkey, -1);
        continue;
      }
      Work w;
      w.key = key;
      w.had = had;
      w.line_begin = lbeg;
      w.line_end = lend;
      if (had)
        w.old_rec = *found;
      w.rec = w.old_rec;
      w.take = had && w.old_rec.indexed();
      if (had && is_del && !is_ins) {
        w.exists = false;
      } else if (had && is_ins && !is_del) {
        return rebuild_all(); // duplicate key
      } else if (is_ins) {
        // new order, or UPDATE of an existing one (possibly to another
        // customer and hence segment: the entry is re-placed there)
        const auto &d = *ins;
        if (d.custkey < 0 ||
            d.custkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
          return rebuild_all();
        if (!had && is_del)
          adjust_orphans(del->custkey, -1); // old row was untracked
        const auto *cust = tracked_customer(d.custkey);
        if (!cust) {
          w.exists = false; // customer outside every tracked segment
          adjust_orphans(d.custkey, 1);
        } else {
          w.rec.key = key;
          w.rec.orderdate = PackTpchQ3Date(d.orderdate_days);
          w.rec.shippriority = PackTpchQ3Shippriority(d.shippriority);
          w.rec.seg_flags = cust->segment;
          w.rec.bound = 0;
          w.exists = true;
        }
      } else {
        w.exists = true; // lineitem-only change of an existing order
      }
      if (w.take) {
        w.take_bin = segtx(w.old_rec.segment())
                         .PrepareBin(BinOf(w.old_rec.orderdate), false);
        if (!w.take_bin)
          return rebuild_all(); // index and side disagree
      }
      works.push_back(std::move(w));
    }
  }


  std::atomic<bool> inconsistent{false};

  // (B1) Takes: one task per touched bin.
  {
    std::vector<std::pair<Q3SegmentTxn::Bin *, uint32_t>> order;
    for (uint32_t i = 0; i < works.size(); ++i)
      if (works[i].take)
        order.emplace_back(works[i].take_bin, i);
    std::sort(order.begin(), order.end());
    std::vector<std::pair<std::size_t, std::size_t>> groups;
    for (std::size_t i = 0; i < order.size();) {
      std::size_t j = i + 1;
      while (j < order.size() && order[j].first == order[i].first)
        ++j;
      groups.emplace_back(i, j);
      i = j;
    }
    parallel_for(groups.size(), [&](std::size_t g) {
      for (std::size_t i = groups[g].first; i < groups[g].second; ++i) {
        auto &w = works[order[i].second];
        if (!Q3SegmentTxn::TakeIn(*w.take_bin, w.old_rec.orderdate,
                                  w.old_rec.bound, w.key, w.day))
          inconsistent = true; // index and side disagree
      }
    });
  }
  if (inconsistent)
    return rebuild_all();


  // (B2) New entries; resolve the target bins (single-threaded).
  constexpr std::size_t kRange = 256;
  parallel_for((works.size() + kRange - 1) / kRange, [&](std::size_t r) {
    const std::size_t end = std::min(works.size(), (r + 1) * kRange);
    for (std::size_t i = r * kRange; i < end; ++i) {
      auto &w = works[i];
      if (!w.exists)
        continue;
      for (uint32_t l = w.line_begin; l < w.line_end; ++l) {
        auto &v = w.day[lines[l].day];
        v += lines[l].revenue;
        if (v == 0)
          w.day.erase(lines[l].day);
      }
      w.rec.seg_flags = static_cast<uint8_t>(w.rec.seg_flags & 0x7F);
      w.rec.bound = 0;
      if (w.day.empty())
        continue;
      FinalizeTpchQ3OrderSummary(w.buckets, w.day);
      if (w.buckets.empty())
        continue;
      w.entry.bound = BoundOf(w.day);
      w.entry.orderkey = w.rec.key;
      w.entry.orderdate = w.rec.orderdate;
      w.entry.shippriority = w.rec.shippriority;
      w.rec.bound = w.entry.bound;
      w.rec.seg_flags = static_cast<uint8_t>(w.rec.seg_flags | 0x80);
      w.place = true;
    }
  });
  for (auto &w : works)
    if (w.place)
      w.put_bin = segtx(w.rec.segment()).PrepareBin(BinOf(w.rec.orderdate), true);


  // (B3) Inserts: one task per touched bin.
  {
    std::vector<std::pair<Q3SegmentTxn::Bin *, uint32_t>> order;
    for (uint32_t i = 0; i < works.size(); ++i)
      if (works[i].place)
        order.emplace_back(works[i].put_bin, i);
    std::sort(order.begin(), order.end());
    std::vector<std::pair<std::size_t, std::size_t>> groups;
    for (std::size_t i = 0; i < order.size();) {
      std::size_t j = i + 1;
      while (j < order.size() && order[j].first == order[i].first)
        ++j;
      groups.emplace_back(i, j);
      i = j;
    }
    parallel_for(groups.size(), [&](std::size_t g) {
      for (std::size_t i = groups[g].first; i < groups[g].second; ++i) {
        auto &w = works[order[i].second];
        Q3SegmentTxn::InsertIn(*w.put_bin, w.entry, w.buckets);
      }
    });
  }

  // (C) Side index.
  for (auto &w : works) {
    if (w.exists)
      otx.Upsert(w.rec);
    else if (w.had)
      otx.Erase(w.key);
  }

  // Customer changes last, against the post-write tables: orders of the
  // customer are fetched with one point query and moved between segment
  // indexes (or dropped / materialized) copy-on-write.
  auto move_customer = [&](uint32_t custkey, core::PartitionId new_seg,
                           bool is_insert) -> bool {
    const auto *cur = ctx.Find(custkey);
    const core::PartitionId cur_seg = cur ? cur->segment : 0;
    const uint32_t orphans = cur ? cur->orphan_orders : 0;
    if (is_insert && cur_seg != 0)
      return false; // duplicate key
    if (cur_seg == new_seg)
      return true;
    TpchQ3CustRec next;
    next.key = custkey;
    next.segment = static_cast<uint8_t>(new_seg);
    if (cur_seg != 0) {
      std::vector<TpchQ3OrderInput> orders;
      if (!initial_scan ||
          !initial_scan->ScanOrdersByCustomer(
              custkey,
              [&](const TpchQ3OrderInput &o) { orders.push_back(o); }))
        return false;
      for (const auto &o : orders) {
        if (o.key < 0 ||
            o.key > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
          return false;
        const auto okey = static_cast<uint32_t>(o.key);
        const auto *rec = otx.Find(okey);
        if (!rec)
          continue;
        TpchQ3OrderRec r = *rec;
        if (r.segment() != cur_seg)
          return false;
        std::map<int32_t, int64_t> day;
        if (r.indexed() &&
            !segtx(cur_seg).Take(r.orderdate, r.bound, okey, day))
          return false;
        if (new_seg == 0) {
          otx.Erase(okey);
          continue;
        }
        r.seg_flags = static_cast<uint8_t>(new_seg);
        place_order(r, day);
        otx.Upsert(r);
      }
      next.orphan_orders =
          new_seg == 0 ? static_cast<uint32_t>(orders.size()) : 0;
    } else if (new_seg != 0 && orphans > 0) {
      // The key had untracked orders: materialize them (post-write state).
      std::vector<TpchQ3OrderInput> orders;
      if (!initial_scan ||
          !initial_scan->ScanOrdersByCustomer(
              custkey,
              [&](const TpchQ3OrderInput &o) { orders.push_back(o); }))
        return false;
      std::vector<int64_t> order_keys;
      for (const auto &o : orders)
        order_keys.push_back(o.key);
      std::map<int64_t, std::map<int32_t, int64_t>> days;
      if (!initial_scan->ScanLineitemsByOrders(
              order_keys, [&](const TpchQ3LineitemInput &li) {
                days[li.orderkey][li.shipdate_days] += TpchQ3RevenueRaw(
                    li.extended_price_raw, li.extended_price_scale,
                    li.discount_raw, li.discount_scale);
              }))
        return false;
      for (const auto &o : orders) {
        if (o.key < 0 ||
            o.key > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
          return false;
        const auto okey = static_cast<uint32_t>(o.key);
        if (otx.Find(okey))
          return false; // should be untracked
        TpchQ3OrderRec r;
        r.key = okey;
        r.orderdate = PackTpchQ3Date(o.orderdate_days);
        r.shippriority = PackTpchQ3Shippriority(o.shippriority);
        r.seg_flags = static_cast<uint8_t>(new_seg);
        std::map<int32_t, int64_t> day;
        for (const auto &e : days[o.key])
          if (e.second != 0)
            day[e.first] = e.second;
        place_order(r, day);
        otx.Upsert(r);
      }
      next.orphan_orders = 0;
    } else {
      next.orphan_orders = new_seg == 0 ? orphans : 0;
    }
    ctx.Upsert(next);
    return true;
  };
  using CKind = TpchQ3CustomerDelta::Kind;
  for (const auto &c : buffer.CustomerDeltas()) {
    const auto key = static_cast<uint32_t>(c.custkey);
    if (!move_customer(key, c.segment, c.kind == CKind::Insert))
      return rebuild_all();
  }

  const bool side_dirty = otx.Dirty() || ctx.Dirty();
  bool seg_dirty = false;
  for (const auto &entry : seg_txns)
    seg_dirty = seg_dirty || entry.second.Dirty();
  if (!side_dirty && !seg_dirty)
    return outcome; // nothing in any tracked segment changed


  outcome.action = core::MaintenanceAction::ApplyDelta;

  for (auto &[segment, txn] : seg_txns) {
    if (!txn.Dirty())
      continue;
    const auto version = seg_version.count(segment) ? seg_version[segment] : 0;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, segment, version,
        MakeSegmentIntervalImage(segment, txn.Finish(parallel_for))));
  }

  if (side_dirty) {
    auto new_side = std::make_shared<TpchQ3Side>();
    new_side->orders = otx.Finish(parallel_for);
    new_side->custs = ctx.Finish(parallel_for);
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ3Layout::kAccelerator, core::TpchQ3Layout::kOrderSegmentSide,
        side_version,
        MakeOrderSegmentSideImage(std::shared_ptr<const TpchQ3Side>(
            std::move(new_side)))));
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
  std::shared_ptr<const TpchQ3Segment> state;
  if (!LoadSegment(services, partition, state) || !state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(ComposeTpchQ3TopK(*state, cutoff), sink);
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
  const auto built = BuildState(services);
  const auto partition = TpchQ3PartitionFromMktsegment(segment);
  const auto it = built.segments.find(partition);
  if (it == built.segments.end()) {
    EmitResult({}, sink);
    return;
  }
  EmitResult(ComposeTpchQ3TopK(*it->second, cutoff), sink);
}

namespace {

// Initial-build temporaries: per-order shipdate revenue lists chained through
// a chunked node pool (no per-order std::map, no pool reallocation peaks).
struct DayNode {
  int64_t revenue = 0;
  uint32_t next = 0; // index + 1; 0 = end
  uint16_t date = 0;
};

class DayNodePool {
public:
  uint32_t Add(const DayNode &node) {
    const std::size_t index = size_++;
    if (index / kBlock >= blocks_.size())
      blocks_.push_back(std::make_unique<DayNode[]>(kBlock));
    blocks_[index / kBlock][index % kBlock] = node;
    return static_cast<uint32_t>(index + 1);
  }
  DayNode &At(uint32_t handle) {
    const std::size_t index = handle - 1;
    return blocks_[index / kBlock][index % kBlock];
  }

private:
  static constexpr std::size_t kBlock = 1u << 20;
  std::vector<std::unique_ptr<DayNode[]>> blocks_;
  std::size_t size_ = 0;
};

} // namespace

TpchQ3Module::BuiltState
TpchQ3Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  BuiltState built;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");

  TpchQ3ShardIndex<TpchQ3CustRec>::Builder custs;
  initial_scan->ScanCustomers([&](const TpchQ3CustomerInput &input) {
    const auto partition = TpchQ3PartitionFromMktsegment(input.mktsegment);
    if (partition == 0 || input.key < 0 ||
        input.key > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
      return;
    TpchQ3CustRec rec;
    rec.key = static_cast<uint32_t>(input.key);
    rec.segment = static_cast<uint8_t>(partition);
    custs.Add(rec);
  });
  custs.Sort();

  // `bound` temporarily holds the head of the order's day-node chain.
  std::unordered_map<uint32_t, uint32_t> orphan_custs;
  TpchQ3ShardIndex<TpchQ3OrderRec>::Builder orders;
  initial_scan->ScanOrders([&](const TpchQ3OrderInput &input) {
    if (input.custkey < 0 ||
        input.custkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
      return;
    const auto *cust = custs.Find(static_cast<uint32_t>(input.custkey));
    if (!cust) {
      ++orphan_custs[static_cast<uint32_t>(input.custkey)];
      return;
    }
    TpchQ3OrderRec rec;
    rec.key = PackTpchQ3Orderkey(input.key);
    rec.orderdate = PackTpchQ3Date(input.orderdate_days);
    rec.shippriority = PackTpchQ3Shippriority(input.shippriority);
    rec.seg_flags = cust->segment;
    rec.bound = 0;
    orders.Add(rec);
  });
  orders.Sort();
  if (!orphan_custs.empty()) {
    for (const auto &entry : orphan_custs) {
      TpchQ3CustRec rec;
      rec.key = entry.first;
      rec.orphan_orders = entry.second;
      custs.Add(rec);
    }
    custs.Sort();
  }

  DayNodePool nodes;
  initial_scan->ScanLineitems([&](const TpchQ3LineitemInput &input) {
    if (input.orderkey < 0 ||
        input.orderkey > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
      return;
    auto *order = orders.Find(static_cast<uint32_t>(input.orderkey));
    if (!order)
      return;
    const auto revenue =
        TpchQ3RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                         input.discount_raw, input.discount_scale);
    const uint16_t date = PackTpchQ3Date(input.shipdate_days);
    for (auto h = static_cast<uint32_t>(order->bound); h != 0;) {
      auto &node = nodes.At(h);
      if (node.date == date) {
        node.revenue += revenue;
        return;
      }
      h = node.next;
    }
    DayNode node;
    node.revenue = revenue;
    node.date = date;
    node.next = static_cast<uint32_t>(order->bound);
    order->bound = nodes.Add(node);
  });

  // Group orders with >= 1 shipdate into (segment, orderdate-bin) streams.
  struct Staging {
    std::vector<TpchQ3Entry> entries;
    std::vector<TpchQ3ShipdateBucket> pool;
  };
  std::map<std::pair<core::PartitionId, uint16_t>, Staging> staging;
  std::vector<std::pair<int32_t, int64_t>> days;
  std::map<int32_t, int64_t> day_map;
  std::vector<TpchQ3ShipdateBucket> buckets;
  orders.ForEach([&](TpchQ3OrderRec &order) {
    auto head = static_cast<uint32_t>(order.bound);
    order.bound = 0;
    if (head == 0)
      return;
    day_map.clear();
    for (auto h = head; h != 0;) {
      const auto &node = nodes.At(h);
      if (node.revenue != 0)
        day_map[node.date] = node.revenue;
      h = node.next;
    }
    if (day_map.empty())
      return;
    FinalizeTpchQ3OrderSummary(buckets, day_map);
    if (buckets.empty())
      return;
    TpchQ3Entry entry;
    entry.bound = BoundOf(day_map);
    entry.orderkey = order.key;
    entry.orderdate = order.orderdate;
    entry.shippriority = order.shippriority;
    entry.maxship = buckets.back().shipdate;
    auto &stage = staging[{order.segment(), BinOf(order.orderdate)}];
    entry.bucket_begin = static_cast<uint32_t>(stage.pool.size());
    entry.bucket_count = static_cast<uint16_t>(buckets.size());
    stage.pool.insert(stage.pool.end(), buckets.begin(), buckets.end());
    stage.entries.push_back(entry);
    order.bound = entry.bound;
    order.seg_flags = static_cast<uint8_t>(order.seg_flags | 0x80);
  });

  std::map<core::PartitionId, std::shared_ptr<TpchQ3Segment>> segments;
  for (auto &[key, stage] : staging) {
    std::sort(stage.entries.begin(), stage.entries.end(),
              [](const TpchQ3Entry &a, const TpchQ3Entry &b) {
                return EntryBefore(a, b.bound, b.orderkey);
              });
    auto &segment = segments[key.first];
    if (!segment) {
      segment = std::make_shared<TpchQ3Segment>();
      segment->segment = key.first;
    }
    segment->bins[key.second] = MakeBinList(stage.entries, stage.pool);
    stage = Staging{}; // release the staging memory bin by bin
  }
  for (auto &[id, segment] : segments)
    built.segments[id] = segment;
  built.side.orders = orders.Finish();
  built.side.custs = custs.Finish();
  return built;
}

bool TpchQ3Module::LoadSegment(
    core::AuxEngineServices &services, core::PartitionId segment,
    std::shared_ptr<const TpchQ3Segment> &state) const {
  state.reset();
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {segment});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != segment)
      continue;
    const auto *typed =
        AsQ3Image(image, TpchQ3ImageKind::SegmentIntervalIndex);
    if (!typed || typed->segment != segment)
      return false;
    state = typed->state;
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
