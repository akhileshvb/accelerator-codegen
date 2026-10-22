#include "tpch/q3/duckdb/aux/module.hpp"
#include "tpch/q3/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include "tpch/q3/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

// Rows per ApplyGrouped group (the substrate test grain can lower it).
constexpr std::size_t kRowsPerGroup = 2048;

// Shards per slice when sorting or scanning the built order index (untimed).
constexpr std::size_t kSortShardGrain = 16;
// Order keys per slice of the side-index lookups (untimed).
constexpr std::size_t kLookupGrain = 1024;
// Orders per slice of the entry computation (untimed).
constexpr std::size_t kWorkGrain = 256;
// Segment bins per slice (untimed): each bin is an independent copy-on-write unit.
constexpr std::size_t kBinGrain = 1;
// Line-item buckets per slice of the day-list chaining (untimed): each bucket is heavy.
constexpr std::size_t kBucketGrain = 1;
// Bins or shards per slice when publishing a commit's images (untimed).
constexpr std::size_t kFinishGrain = 1;
// Elements per sorted slice when sorting a commit's deltas (untimed).
constexpr std::size_t kDeltaSortGrain = 1 << 16;
// Deltas above which the cost model chooses between delta-apply and rebuild.
constexpr std::size_t kBulkDeltaCount = 256;
// Scale of the revenue sums: price (2) * (1 - discount) (2).
constexpr int32_t kRevenueScale = 4;

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

bool ReadLineitem(const core::AuxRow &row, TpchQ3LineitemInput &out) {
  int64_t orderkey = 0;
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  if (!core::IntCell(row, 0, orderkey) ||
      !core::DecimalCell(row, 1, extended_raw, extended_scale) ||
      !core::DecimalCell(row, 2, discount_raw, discount_scale) ||
      !core::DateCell(row, 3, out.shipdate_days))
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
  if (!core::IntCell(row, 0, orderkey) || !core::DateCell(row, 1, orderdate_days) ||
      !core::IntCell(row, 2, custkey) || !core::IntCell(row, 3, shippriority_raw))
    return false;
  if (shippriority_raw < std::numeric_limits<int32_t>::min() ||
      shippriority_raw > std::numeric_limits<int32_t>::max())
    return false;
  shippriority = static_cast<int32_t>(shippriority_raw);
  return true;
}

inline bool EntryBefore(const TpchQ3Entry &a, int64_t bound, uint64_t orderkey) {
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
    std::vector<std::pair<int64_t, uint64_t>> lasts;
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
  static bool TakeIn(Bin &bin, int64_t bound,
                     uint64_t orderkey, std::map<int32_t, int64_t> &day_revenue) {
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
  bool Take(uint16_t orderdate, int64_t bound, uint64_t orderkey,
            std::map<int32_t, int64_t> &day_revenue) {
    Bin *bin = FindBin(BinOf(orderdate), false);
    return bin && TakeIn(*bin, bound, orderkey, day_revenue);
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

  // `for_each_index(n, fn)` runs fn(0..n).
  template <class ForEachIndex>
  std::shared_ptr<const TpchQ3Segment> Finish(ForEachIndex &&for_each_index) {
    auto out = std::make_shared<TpchQ3Segment>();
    out->segment = segment_;
    if (base_)
      out->bins = base_->bins;
    std::vector<std::pair<uint16_t, Bin *>> touched;
    for (auto &[bin_id, bin] : bins_)
      if (bin.touched)
        touched.emplace_back(bin_id, &bin);
    std::vector<std::shared_ptr<TpchQ3BinList>> lists(touched.size());
    for_each_index(touched.size(), [&](std::size_t t) {
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
                         ? std::pair<int64_t, uint64_t>{0, 0}
                         : std::pair<int64_t, uint64_t>{entries.back().bound,
                                                        entries.back().orderkey};
    }
  }

  static void SetLast(Bin &bin, std::size_t ci) {
    const auto &entries = bin.chunks[ci]->entries;
    if (!entries.empty())
      bin.lasts[ci] = {entries.back().bound, entries.back().orderkey};
  }

  // First chunk whose last entry does not sort before (bound, orderkey).
  static std::size_t ChunkFor(const Bin &bin, int64_t bound, uint64_t orderkey) {
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

// One order's change in a commit: whether it had an index entry, whether it is
// still tracked afterwards, and the entry to put back.
struct OrderWork {
  uint64_t key = 0;
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

// Net revenue of one order on one ship day.
struct NetLine {
  uint64_t key;
  int32_t day;
  int64_t revenue;
};

using OrderDeltaRef = std::pair<uint64_t, const TpchQ3OrderDelta *>;

// The published side index and segment indexes of one snapshot.
struct PublishedIndexes {
  std::shared_ptr<const TpchQ3Side> side;
  core::Version side_version = 0;
  std::map<core::PartitionId, std::shared_ptr<const TpchQ3Segment>> seg_base;
  std::map<core::PartitionId, core::Version> seg_version;
};

// False when an image is missing or foreign (the caller rebuilds).
bool LoadPublishedIndexes(core::AuxEngineServices &services, PublishedIndexes &out) {
  std::vector<core::PartitionId> partitions{core::TpchQ3Layout::kOrderSegmentSide};
  for (core::PartitionId s = core::TpchQ3Layout::kFirstSegment;
       s <= core::TpchQ3Layout::kLastSegment; ++s)
    partitions.push_back(s);
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ3Layout::kOrderSegmentSide) {
      const auto *typed = AsQ3Image(image, TpchQ3ImageKind::OrderSegmentSide);
      if (!typed)
        return false;
      out.side = typed->side;
      out.side_version = image.version;
    } else if (core::TpchQ3Layout::IsSegmentPartition(image.partition)) {
      const auto *typed = AsQ3Image(image, TpchQ3ImageKind::SegmentIntervalIndex);
      if (!typed)
        return false;
      out.seg_base[image.partition] = typed->state;
      out.seg_version[image.partition] = image.version;
    }
  }
  return out.side != nullptr;
}

// Applies a commit's order, lineitem and customer deltas to copy-on-write
// transactions over the published indexes. Stage() returns false when the
// published state cannot take them (the caller rebuilds).
class DeltaStager {
public:
  DeltaStager(core::AuxEngineServices &services, const TpchQ3TxnBuffer &buffer,
              TpchQ3InitialScan *initial_scan, PublishedIndexes published)
      : services_(services), buffer_(buffer), initial_scan_(initial_scan),
        published_(std::move(published)), otx_(published_.side->orders),
        ctx_(published_.side->custs) {}

  // A key both deleted and inserted is an UPDATE: the order and its lineitems
  // stay. Order and lineitem deltas run in steps: (A) a sequential pass over
  // the sorted keys decides each key's fate against the side index; (B) the
  // segment-index work is done bin-parallel (takes, then entry computation,
  // then inserts: bins are independent copy-on-write units); (C) the side
  // index is updated. Customer changes come last.
  bool Stage(core::StagedMaintenanceOutcome &outcome) {
    if (!SplitOrderDeltas())
      return false;
    NetLineitemDeltas();
    LookUpSideRecords();
    if (!PlanWorks() || !TakeEntries())
      return false;
    BuildEntries();
    PutEntries();
    ApplyOrderRecords();
    using CKind = TpchQ3CustomerDelta::Kind;
    for (const auto &c : buffer_.CustomerDeltas()) {
      if (!MoveCustomer(static_cast<uint64_t>(c.custkey), c.segment,
                        c.kind == CKind::Insert))
        return false;
    }
    Publish(outcome);
    return true;
  }

private:
  Q3SegmentTxn &SegmentTxn(core::PartitionId segment) {
    auto it = seg_txns_.find(segment);
    if (it == seg_txns_.end()) {
      auto base = published_.seg_base.find(segment);
      it = seg_txns_
               .emplace(segment, Q3SegmentTxn(segment, base != published_.seg_base.end()
                                                           ? base->second
                                                           : nullptr))
               .first;
    }
    return it->second;
  }

  // Orders referencing a key that is not a tracked customer are untracked;
  // the key's record counts them so a later customer insert / segment change
  // knows whether a lookup is needed.
  void AdjustOrphans(int64_t custkey, int32_t delta) {
    if (custkey < 0)
      return;
    const auto key = static_cast<uint64_t>(custkey);
    const auto *rec = ctx_.Find(key);
    if (rec) {
      if (rec->segment != 0) {
        return;
      }
      TpchQ3CustRec next = *rec;
      const int64_t count = static_cast<int64_t>(next.orphan_orders) + delta;
      next.orphan_orders = count > 0 ? static_cast<uint32_t>(count) : 0;
      ctx_.Upsert(next);
    } else if (delta > 0) {
      TpchQ3CustRec next;
      next.key = key;
      next.orphan_orders = static_cast<uint32_t>(delta);
      ctx_.Upsert(next);
    }
  }

  const TpchQ3CustRec *TrackedCustomer(int64_t custkey) {
    if (custkey < 0)
      return nullptr;
    const auto *rec = ctx_.Find(static_cast<uint64_t>(custkey));
    return rec && rec->segment != 0 ? rec : nullptr;
  }

  // Inserts the order's index entry (if it has lineitems) into its segment.
  void PlaceOrder(TpchQ3OrderRec &rec, const std::map<int32_t, int64_t> &day) {
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
    SegmentTxn(rec.segment()).Insert(entry, buckets);
  }

  // Inserted and deleted orders by key; false on a duplicate inserted key.
  bool SplitOrderDeltas() {
    for (const auto &delta : buffer_.OrderDeltas()) {
      const auto key = static_cast<uint64_t>(delta.orderkey);
      (delta.sign > 0 ? inserts_ : deletes_).emplace_back(key, &delta);
    }
    const auto by_key = [](const OrderDeltaRef &a, const OrderDeltaRef &b) {
      return a.first < b.first;
    };
    core::ParallelStableSort(services_, inserts_, by_key, kDeltaSortGrain);
    core::ParallelStableSort(services_, deletes_, by_key, kDeltaSortGrain);
    for (std::size_t i = 1; i < inserts_.size(); ++i)
      if (inserts_[i].first == inserts_[i - 1].first)
        return false;
    return true;
  }

  // Net per-(key, day) revenue of the lineitem deltas, sorted by (key, day).
  // Mutations usually arrive grouped by order: sort only when they do not.
  void NetLineitemDeltas() {
    lines_.reserve(buffer_.Deltas().size());
    for (const auto &delta : buffer_.Deltas())
      lines_.push_back({static_cast<uint64_t>(delta.orderkey), delta.shipdate_day,
                        static_cast<int64_t>(delta.sign) * delta.revenue_raw});
    const auto line_less = [](const NetLine &a, const NetLine &b) {
      return a.key != b.key ? a.key < b.key : a.day < b.day;
    };
    if (!std::is_sorted(lines_.begin(), lines_.end(), line_less)) {
      const auto key_less = [](const NetLine &a, const NetLine &b) { return a.key < b.key; };
      if (!std::is_sorted(lines_.begin(), lines_.end(), key_less))
        core::ParallelStableSort(services_, lines_, key_less, kDeltaSortGrain);
      const std::vector<std::size_t> starts =
          core::RunStarts(lines_, [](const NetLine &line) { return line.key; });
      for (std::size_t r = 0; r + 1 < starts.size(); ++r)
        std::sort(lines_.begin() + static_cast<std::ptrdiff_t>(starts[r]),
                  lines_.begin() + static_cast<std::ptrdiff_t>(starts[r + 1]),
                  [](const NetLine &a, const NetLine &b) { return a.day < b.day; });
    }
    std::size_t out = 0;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
      if (out > 0 && lines_[out - 1].key == lines_[i].key &&
          lines_[out - 1].day == lines_[i].day)
        lines_[out - 1].revenue += lines_[i].revenue;
      else
        lines_[out++] = lines_[i];
    }
    lines_.resize(out);
  }

  // Side-index lookups for every key, in parallel (the index is not written
  // until step C, so published leaves are stable).
  void LookUpSideRecords() {
    // The three lists are sorted by key: merge them instead of sorting the union.
    const auto sorted_keys = [](const auto &list, auto key_of) {
      std::vector<uint64_t> keys;
      keys.reserve(list.size());
      for (const auto &e : list)
        if (keys.empty() || keys.back() != key_of(e))
          keys.push_back(key_of(e));
      return keys;
    };
    const auto merge_keys = [](const std::vector<uint64_t> &a, const std::vector<uint64_t> &b) {
      std::vector<uint64_t> out;
      out.reserve(a.size() + b.size());
      std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
      return out;
    };
    all_keys_ = merge_keys(
        merge_keys(sorted_keys(inserts_, [](const OrderDeltaRef &e) { return e.first; }),
                   sorted_keys(deletes_, [](const OrderDeltaRef &e) { return e.first; })),
        sorted_keys(lines_, [](const NetLine &e) { return e.key; }));
    found_recs_.resize(all_keys_.size());
    core::ParallelEach(services_, all_keys_.size(), kLookupGrain, [&](std::size_t i) {
      found_recs_[i] = otx_.Find(all_keys_[i]);
    });
  }

  // (A) Decides each touched key's fate; false when the index and the side
  // disagree or a key is duplicated.
  bool PlanWorks() {
    works_.reserve(inserts_.size() + deletes_.size() + 16);
    std::size_t ki = 0;
    std::size_t ii = 0, di = 0, li = 0;
    while (ii < inserts_.size() || di < deletes_.size() || li < lines_.size()) {
      uint64_t key = std::numeric_limits<uint64_t>::max();
      if (ii < inserts_.size())
        key = std::min(key, inserts_[ii].first);
      if (di < deletes_.size())
        key = std::min(key, deletes_[di].first);
      if (li < lines_.size())
        key = std::min(key, lines_[li].key);
      const TpchQ3OrderDelta *ins = nullptr, *del = nullptr;
      if (ii < inserts_.size() && inserts_[ii].first == key)
        ins = inserts_[ii++].second;
      if (di < deletes_.size() && deletes_[di].first == key) {
        del = deletes_[di].second; // first delete of a key wins
        while (di < deletes_.size() && deletes_[di].first == key)
          ++di;
      }
      uint32_t lbeg = static_cast<uint32_t>(li);
      while (li < lines_.size() && lines_[li].key == key)
        ++li;
      const uint32_t lend = static_cast<uint32_t>(li);

      while (all_keys_[ki] != key)
        ++ki;
      const auto *found = found_recs_[ki];
      const bool had = found != nullptr;
      const bool is_ins = ins != nullptr, is_del = del != nullptr;
      if (!had && !is_ins) {
        // lineitem change / delete of an order outside every tracked segment
        if (is_del)
          AdjustOrphans(del->custkey, -1);
        continue;
      }
      OrderWork w;
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
        return false; // duplicate key
      } else if (is_ins) {
        // new order, or UPDATE of an existing one (possibly to another
        // customer and hence segment: the entry is re-placed there)
        const auto &d = *ins;
        if (d.custkey < 0)
          return false;
        if (!had && is_del)
          AdjustOrphans(del->custkey, -1); // old row was untracked
        const auto *cust = TrackedCustomer(d.custkey);
        if (!cust) {
          w.exists = false; // customer outside every tracked segment
          AdjustOrphans(d.custkey, 1);
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
        w.take_bin = SegmentTxn(w.old_rec.segment())
                         .PrepareBin(BinOf(w.old_rec.orderdate), false);
        if (!w.take_bin)
          return false; // index and side disagree
      }
      works_.push_back(std::move(w));
    }
    return true;
  }

  // Runs fn(work) for the works selected by `bin_of` (nullptr: skip), one task
  // per touched bin; the works of a bin run in order.
  template <class BinOfWork, class Fn>
  void ForEachWorkByBin(BinOfWork &&bin_of, Fn &&fn) {
    std::vector<std::pair<Q3SegmentTxn::Bin *, uint32_t>> order;
    for (uint32_t i = 0; i < works_.size(); ++i)
      if (auto *bin = bin_of(works_[i]))
        order.emplace_back(bin, i);
    std::sort(order.begin(), order.end());
    std::vector<std::pair<std::size_t, std::size_t>> groups;
    for (std::size_t i = 0; i < order.size();) {
      std::size_t j = i + 1;
      while (j < order.size() && order[j].first == order[i].first)
        ++j;
      groups.emplace_back(i, j);
      i = j;
    }
    core::ParallelEach(services_, groups.size(), kBinGrain, [&](std::size_t g) {
      for (std::size_t i = groups[g].first; i < groups[g].second; ++i)
        fn(works_[order[i].second]);
    });
  }

  // (B1) Removes the old entries; false when the index and the side disagree.
  bool TakeEntries() {
    std::atomic<bool> inconsistent{false};
    ForEachWorkByBin(
        [](OrderWork &w) { return w.take ? w.take_bin : nullptr; },
        [&](OrderWork &w) {
          if (!Q3SegmentTxn::TakeIn(*w.take_bin, w.old_rec.bound,
                                    w.key, w.day))
            inconsistent = true;
        });
    return !inconsistent;
  }

  // (B2) Computes the new entries, then resolves their target bins
  // (single-threaded).
  void BuildEntries() {
    core::ParallelEach(services_, works_.size(), kWorkGrain, [&](std::size_t i) {
      auto &w = works_[i];
      if (!w.exists)
        return;
      for (uint32_t l = w.line_begin; l < w.line_end; ++l) {
        auto &v = w.day[lines_[l].day];
        v += lines_[l].revenue;
        if (v == 0)
          w.day.erase(lines_[l].day);
      }
      w.rec.seg_flags = static_cast<uint8_t>(w.rec.seg_flags & 0x7F);
      w.rec.bound = 0;
      if (w.day.empty())
        return;
      FinalizeTpchQ3OrderSummary(w.buckets, w.day);
      if (w.buckets.empty())
        return;
      w.entry.bound = BoundOf(w.day);
      w.entry.orderkey = w.rec.key;
      w.entry.orderdate = w.rec.orderdate;
      w.entry.shippriority = w.rec.shippriority;
      w.rec.bound = w.entry.bound;
      w.rec.seg_flags = static_cast<uint8_t>(w.rec.seg_flags | 0x80);
      w.place = true;
    });
    for (auto &w : works_)
      if (w.place)
        w.put_bin = SegmentTxn(w.rec.segment()).PrepareBin(BinOf(w.rec.orderdate), true);
  }

  // (B3) Inserts the new entries.
  void PutEntries() {
    ForEachWorkByBin([](OrderWork &w) { return w.place ? w.put_bin : nullptr; },
                     [](OrderWork &w) {
                       Q3SegmentTxn::InsertIn(*w.put_bin, w.entry, w.buckets);
                     });
  }

  // (C) The side index changes, one row per order whose record changes, in key
  // order; groups of neighbouring shards are updated concurrently.
  void ApplyOrderRecords() {
    struct RecordChange {
      uint64_t key;
      bool exists;
      TpchQ3OrderRec rec;
    };
    std::vector<std::vector<RecordChange>> pieces(1);
    for (const auto &w : works_)
      if (w.exists || w.had)
        pieces.front().push_back({w.key, w.exists, w.rec});
    otx_.ApplyGrouped(
        pieces, [](const RecordChange &c) -> const uint64_t & { return c.key; },
        [&](std::size_t n, const auto &fn) { core::ParallelEach(services_, n, 1, fn); },
        [](std::size_t, auto &scope, std::vector<RecordChange> &rows) {
          for (const auto &c : rows) {
            if (c.exists)
              scope.Upsert(c.rec);
            else
              scope.Erase(c.key);
          }
        }, core::CappedGrain(kRowsPerGroup));
  }

  bool ScanOrdersOf(uint64_t custkey, std::vector<TpchQ3OrderInput> &orders) {
    return initial_scan_ &&
           initial_scan_->ScanOrdersByCustomer(
               custkey, [&](const TpchQ3OrderInput &o) { orders.push_back(o); });
  }

  // The customer's tracked orders move to `new_seg` (or are dropped when it
  // is 0).
  bool RehomeOrders(uint64_t custkey, core::PartitionId cur_seg, core::PartitionId new_seg,
                    std::size_t &order_count) {
    std::vector<TpchQ3OrderInput> orders;
    if (!ScanOrdersOf(custkey, orders))
      return false;
    order_count = orders.size();
    for (const auto &o : orders) {
      if (o.key < 0)
        return false;
      const auto okey = static_cast<uint64_t>(o.key);
      const auto *rec = otx_.Find(okey);
      if (!rec)
        continue;
      TpchQ3OrderRec r = *rec;
      if (r.segment() != cur_seg)
        return false;
      std::map<int32_t, int64_t> day;
      if (r.indexed() && !SegmentTxn(cur_seg).Take(r.orderdate, r.bound, okey, day))
        return false;
      if (new_seg == 0) {
        otx_.Erase(okey);
        continue;
      }
      r.seg_flags = static_cast<uint8_t>(new_seg);
      PlaceOrder(r, day);
      otx_.Upsert(r);
    }
    return true;
  }

  // The key had untracked orders: materializes them (post-write state).
  bool MaterializeOrphans(uint64_t custkey, core::PartitionId new_seg) {
    std::vector<TpchQ3OrderInput> orders;
    if (!ScanOrdersOf(custkey, orders))
      return false;
    std::vector<int64_t> order_keys;
    for (const auto &o : orders)
      order_keys.push_back(o.key);
    std::map<int64_t, std::map<int32_t, int64_t>> days;
    if (!initial_scan_->ScanLineitemsByOrders(
            order_keys, [&](const TpchQ3LineitemInput &li) {
              days[li.orderkey][li.shipdate_days] += TpchQ3RevenueRaw(
                  li.extended_price_raw, li.extended_price_scale, li.discount_raw,
                  li.discount_scale);
            }))
      return false;
    for (const auto &o : orders) {
      if (o.key < 0)
        return false;
      const auto okey = static_cast<uint64_t>(o.key);
      if (otx_.Find(okey))
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
      PlaceOrder(r, day);
      otx_.Upsert(r);
    }
    return true;
  }

  // Customer changes run against the post-write tables: orders of the
  // customer are fetched with one point query and moved between segment
  // indexes (or dropped / materialized) copy-on-write.
  bool MoveCustomer(uint64_t custkey, core::PartitionId new_seg, bool is_insert) {
    const auto *cur = ctx_.Find(custkey);
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
      std::size_t order_count = 0;
      if (!RehomeOrders(custkey, cur_seg, new_seg, order_count))
        return false;
      next.orphan_orders = new_seg == 0 ? static_cast<uint32_t>(order_count) : 0;
    } else if (new_seg != 0 && orphans > 0) {
      if (!MaterializeOrphans(custkey, new_seg))
        return false;
      next.orphan_orders = 0;
    } else {
      next.orphan_orders = new_seg == 0 ? orphans : 0;
    }
    ctx_.Upsert(next);
    return true;
  }

  void Publish(core::StagedMaintenanceOutcome &outcome) {
    const bool side_dirty = otx_.Dirty() || ctx_.Dirty();
    bool seg_dirty = false;
    for (const auto &entry : seg_txns_)
      seg_dirty = seg_dirty || entry.second.Dirty();
    if (!side_dirty && !seg_dirty)
      return; // nothing in any tracked segment changed

    outcome.action = core::MaintenanceAction::ApplyDelta;
    const auto for_each_index = [&](std::size_t n, auto &&fn) {
      core::ParallelEach(services_, n, kFinishGrain, fn);
    };
    for (auto &[segment, txn] : seg_txns_) {
      if (!txn.Dirty())
        continue;
      const auto version = published_.seg_version.count(segment)
                               ? published_.seg_version[segment]
                               : 0;
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ3Layout::kAccelerator, segment, version,
          MakeSegmentIntervalImage(segment, txn.Finish(for_each_index))));
    }
    if (side_dirty) {
      auto new_side = std::make_shared<TpchQ3Side>();
      new_side->orders = otx_.Finish(for_each_index);
      new_side->custs = ctx_.Finish(for_each_index);
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ3Layout::kAccelerator, core::TpchQ3Layout::kOrderSegmentSide,
          published_.side_version,
          MakeOrderSegmentSideImage(std::shared_ptr<const TpchQ3Side>(std::move(new_side)))));
    }
  }

  core::AuxEngineServices &services_;
  const TpchQ3TxnBuffer &buffer_;
  TpchQ3InitialScan *initial_scan_;
  PublishedIndexes published_;
  TpchQ3ShardIndex<TpchQ3OrderRec>::Txn otx_;
  TpchQ3ShardIndex<TpchQ3CustRec>::Txn ctx_;
  std::map<core::PartitionId, Q3SegmentTxn> seg_txns_;
  std::vector<OrderDeltaRef> inserts_, deletes_;
  std::vector<NetLine> lines_;
  std::vector<uint64_t> all_keys_;
  std::vector<const TpchQ3OrderRec *> found_recs_;
  std::vector<OrderWork> works_;
};

// One lineitem's revenue on one ship day of an order.
struct DayEntry {
  uint64_t orderkey;
  int64_t revenue;
  uint16_t date;
};

// Orders of one scan partition, with the orphan customers they reference.
struct OrdersPartial {
  core::RowPieces<TpchQ3OrderRec> orders;
  std::unordered_map<uint64_t, uint32_t> orphan_custs;
};

// Lineitem entries of one scan partition, by order bucket.
struct LinePartial {
  std::vector<core::RowPieces<DayEntry>> buckets;
};

// Orders staged for the interval index of one (segment, orderdate bin).
struct Staging {
  std::vector<TpchQ3Entry> entries;
  std::vector<TpchQ3ShipdateBucket> pool;
};

using OrderBuilder = TpchQ3ShardIndex<TpchQ3OrderRec>::Builder;
using CustBuilder = TpchQ3ShardIndex<TpchQ3CustRec>::Builder;
using StagingMap = std::map<std::pair<core::PartitionId, uint16_t>, Staging>;

template <class Builder> auto FinishShards(core::AuxEngineServices &services, Builder &index) {
  return index.Finish([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, 1, fn);
  });
}

template <class Builder> void SortShards(core::AuxEngineServices &services, Builder &index) {
  index.Sort([&](std::size_t n, const auto &fn) {
    core::ParallelEach(services, n, kSortShardGrain, fn);
  });
}

// The orders of tracked customers, sorted. Orders of other customers are
// counted on their customer key in `custs`. `bound` temporarily holds the head
// of the order's day-node chain.
OrderBuilder ScanOrders(core::AuxEngineServices &services, TpchQ3InitialScan &source,
                        CustBuilder &custs) {
  using scan::DateColumn;
  using scan::IntColumn;

  // `bound` temporarily holds the head of the order's day-node chain.
  auto scanned_orders = core::PartitionedBuild<OrdersPartial>(
      services, [] { return OrdersPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        source.ScanTable(services, "orders",
                         {IntColumn("o_orderkey"), DateColumn("o_orderdate"),
                          IntColumn("o_custkey"), IntColumn("o_shippriority")},
                         parts, sink);
      },
      [&](OrdersPartial &p, const auto &row) {
        const int64_t custkey = row.Int(2);
        if (custkey < 0)
          return;
        const auto *cust = custs.Find(static_cast<uint64_t>(custkey));
        if (!cust) {
          ++p.orphan_custs[static_cast<uint64_t>(custkey)];
          return;
        }
        TpchQ3OrderRec rec;
        rec.key = PackTpchQ3Orderkey(row.Int(0));
        rec.orderdate = PackTpchQ3Date(row.Date(1));
        rec.shippriority = PackTpchQ3Shippriority(static_cast<int32_t>(row.Int(3)));
        rec.seg_flags = cust->segment;
        rec.bound = 0;
        p.orders.Add(rec);
      },
      [](OrdersPartial &into, OrdersPartial &&from) {
        core::RowPieces<TpchQ3OrderRec>::Merge(into.orders, std::move(from.orders));
        for (const auto &[key, n] : from.orphan_custs)
          into.orphan_custs[key] += n;
      });
  OrderBuilder orders(core::PieceRows(scanned_orders.orders.pieces));
  orders.AddPieces(scanned_orders.orders.pieces, core::Parallelism::OnPool(services));
  SortShards(services, orders);
  if (!scanned_orders.orphan_custs.empty()) {
    for (const auto &entry : scanned_orders.orphan_custs) {
      TpchQ3CustRec rec;
      rec.key = entry.first;
      rec.orphan_orders = entry.second;
      custs.Add(rec);
    }
    SortShards(services, custs);
  }
  return orders;
}

// Stages the orders of one lineitem bucket: the bucket holds every entry of its
// orders, so sorting by (order, day) folds each order's day revenues locally.
void StageBucket(OrderBuilder &orders, std::vector<std::vector<DayEntry>> &pieces,
                 StagingMap &stages) {
  std::vector<DayEntry> entries;
  for (auto &piece : pieces) {
    entries.insert(entries.end(), piece.begin(), piece.end());
    std::vector<DayEntry>().swap(piece);
  }
  std::sort(entries.begin(), entries.end(), [](const DayEntry &a, const DayEntry &b) {
    return a.orderkey != b.orderkey ? a.orderkey < b.orderkey : a.date < b.date;
  });
  std::map<int32_t, int64_t> day_map;
  std::vector<TpchQ3ShipdateBucket> day_buckets;
  for (std::size_t at = 0; at < entries.size();) {
    const uint64_t orderkey = entries[at].orderkey;
    day_map.clear();
    while (at < entries.size() && entries[at].orderkey == orderkey) {
      const uint16_t date = entries[at].date;
      int64_t revenue = 0;
      for (; at < entries.size() && entries[at].orderkey == orderkey && entries[at].date == date; ++at)
        revenue += entries[at].revenue;
      if (revenue != 0)
        day_map[date] = revenue;
    }
    if (day_map.empty())
      continue;
    FinalizeTpchQ3OrderSummary(day_buckets, day_map);
    if (day_buckets.empty())
      continue;
    TpchQ3OrderRec &order = *orders.Find(orderkey);
    TpchQ3Entry entry;
    entry.bound = BoundOf(day_map);
    entry.orderkey = order.key;
    entry.orderdate = order.orderdate;
    entry.shippriority = order.shippriority;
    entry.maxship = day_buckets.back().shipdate;
    auto &stage = stages[{order.segment(), BinOf(order.orderdate)}];
    entry.bucket_begin = static_cast<uint32_t>(stage.pool.size());
    entry.bucket_count = static_cast<uint16_t>(day_buckets.size());
    stage.pool.insert(stage.pool.end(), day_buckets.begin(), day_buckets.end());
    stage.entries.push_back(entry);
    order.bound = entry.bound;
    order.seg_flags = static_cast<uint8_t>(order.seg_flags | 0x80);
  }
}

// Lineitems of known orders into (segment, orderdate-bin) streams, one staging set
// per order bucket: partitions fill buckets in scan order, buckets are staged in
// parallel (in bucket order).
std::vector<StagingMap> StageLineitems(core::AuxEngineServices &services,
                                       TpchQ3InitialScan &source, OrderBuilder &orders) {
  using scan::DateColumn;
  using scan::DecimalColumn;
  using scan::IntColumn;
  const std::size_t buckets = std::max<std::size_t>(1, services.WorkerCount());
  const auto bucket_of = [buckets](uint64_t orderkey) {
    return static_cast<std::size_t>((orderkey * 0x9E3779B97F4A7C15ULL) >> 40) % buckets;
  };
  auto scanned_lines = core::PartitionedBuild<LinePartial>(
      services, [&] { return LinePartial{std::vector<core::RowPieces<DayEntry>>(buckets)}; },
      [&](std::size_t parts, auto &&sink) {
        source.ScanTable(services, "lineitem",
                         {IntColumn("l_orderkey"), DecimalColumn("l_extendedprice", 2),
                          DecimalColumn("l_discount", 2), DateColumn("l_shipdate")},
                         parts, sink);
      },
      [&](LinePartial &p, const auto &row) {
        const int64_t orderkey = row.Int(0);
        if (orderkey < 0 || !orders.Find(static_cast<uint64_t>(orderkey)))
          return;
        auto &bucket = p.buckets[bucket_of(static_cast<uint64_t>(orderkey))].pieces.back();
        const DayEntry entry{static_cast<uint64_t>(orderkey),
                             TpchQ3RevenueRaw(row.Decimal(1), 2, row.Decimal(2), 2),
                             PackTpchQ3Date(row.Date(3))};
        // Lineitems of an order are mostly adjacent: fold a repeated day.
        if (!bucket.empty() && bucket.back().orderkey == entry.orderkey &&
            bucket.back().date == entry.date)
          bucket.back().revenue += entry.revenue;
        else
          bucket.push_back(entry);
      },
      [](LinePartial &into, LinePartial &&from) {
        for (std::size_t b = 0; b < into.buckets.size(); ++b)
          core::RowPieces<DayEntry>::Merge(into.buckets[b], std::move(from.buckets[b]));
      });
  std::vector<StagingMap> staged(buckets);
  core::ParallelEach(services, buckets, kBucketGrain, [&](std::size_t b) {
    StageBucket(orders, scanned_lines.buckets[b].pieces, staged[b]);
  });
  return staged;
}

// The slices' staging of one bin concatenated in slice order.
Staging JoinStaging(const std::vector<Staging *> &parts) {
  Staging joined;
  for (const Staging *part : parts) {
    const auto base = static_cast<uint32_t>(joined.pool.size());
    for (auto entry : part->entries) {
      entry.bucket_begin += base;
      joined.entries.push_back(entry);
    }
    joined.pool.insert(joined.pool.end(), part->pool.begin(), part->pool.end());
  }
  return joined;
}

// Bins are independent: joins, sorts and chunks them in parallel, attaches them in
// key order.
std::map<core::PartitionId, std::shared_ptr<TpchQ3Segment>>
BuildSegments(core::AuxEngineServices &services, std::vector<StagingMap> &slices) {
  std::map<std::pair<core::PartitionId, uint16_t>, std::vector<Staging *>> by_bin;
  for (auto &slice : slices)
    for (auto &[key, stage] : slice)
      by_bin[key].push_back(&stage);
  std::vector<const std::vector<Staging *> *> bins;
  bins.reserve(by_bin.size());
  for (const auto &entry : by_bin)
    bins.push_back(&entry.second);
  std::vector<std::shared_ptr<const TpchQ3BinList>> bin_lists(bins.size());
  core::ParallelEach(services, bins.size(), kBinGrain, [&](std::size_t i) {
    Staging stage = JoinStaging(*bins[i]);
    for (Staging *part : *bins[i])
      *part = Staging{}; // release the staging memory bin by bin
    std::sort(stage.entries.begin(), stage.entries.end(),
              [](const TpchQ3Entry &a, const TpchQ3Entry &b) {
                return EntryBefore(a, b.bound, b.orderkey);
              });
    bin_lists[i] = MakeBinList(stage.entries, stage.pool);
  });
  std::map<core::PartitionId, std::shared_ptr<TpchQ3Segment>> segments;
  std::size_t next_bin = 0;
  for (const auto &entry : by_bin) {
    const auto &key = entry.first;
    auto &segment = segments[key.first];
    if (!segment) {
      segment = std::make_shared<TpchQ3Segment>();
      segment->segment = key.first;
    }
    segment->bins[key.second] = std::move(bin_lists[next_bin++]);
  }
  return segments;
}

} // namespace

int64_t TpchQ3RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                         int64_t discount_raw, int32_t discount_scale) {
  if (extended_scale == 2 && discount_scale == 2)
    return extended_price_raw * (100 - discount_raw);
  // price * (1 - discount) at scale 4, rounded half away from zero.
  __int128 one = 1;
  for (int32_t i = 0; i < discount_scale; ++i)
    one *= 10;
  __int128 revenue = static_cast<__int128>(extended_price_raw) * (one - discount_raw);
  const int32_t scale = extended_scale + discount_scale;
  if (scale <= kRevenueScale) {
    for (int32_t i = scale; i < kRevenueScale; ++i)
      revenue *= 10;
  } else {
    __int128 divisor = 1;
    for (int32_t i = kRevenueScale; i < scale; ++i)
      divisor *= 10;
    const __int128 half = divisor / 2;
    revenue = (revenue >= 0 ? revenue + half : revenue - half) / divisor;
  }
  return static_cast<int64_t>(revenue);
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
      if (!core::StringCell(row, 0, mkt) || !core::IntCell(row, 1, custkey) || custkey < 0)
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
          orderkey < 0) {
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
    if (!ReadLineitem(row, input) || input.orderkey < 0) {
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
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  const auto total_deltas = buffer.Deltas().size() + buffer.OrderDeltas().size() +
                            buffer.CustomerDeltas().size();
  if (total_deltas == 0)
    return outcome;

  // RF-sized batches are always applied incrementally; the cost model is
  // only consulted for bulk changes.
  if (total_deltas > kBulkDeltaCount) {
    const auto decision =
        services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
            total_deltas, services.ExpectedQueriesBeforeNextWrite()));
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      core::FinishRebuildAll(outcome, decision);
      return outcome;
    }
  }

  PublishedIndexes published;
  if (!LoadPublishedIndexes(services, published)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  DeltaStager stager(services, buffer, initial_scan, std::move(published));
  if (!stager.Stage(outcome))
    core::FinishRebuildAll(outcome);
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

TpchQ3Module::BuiltState
TpchQ3Module::BuildState(core::AuxEngineServices &services) const {
  BuiltState built;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q3 requires a typed initial scan source");

  auto scanned_custs = core::PartitionedBuild<core::RowPieces<TpchQ3CustRec>>(
      services, [] { return core::RowPieces<TpchQ3CustRec>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(
            services, "customer",
            {scan::IntColumn("c_custkey"), scan::TextColumn("c_mktsegment")}, parts, sink);
      },
      [](core::RowPieces<TpchQ3CustRec> &into, const auto &row) {
        const auto partition = TpchQ3PartitionFromMktsegment(row.Text(1));
        if (partition == 0 || row.Int(0) < 0)
          return;
        TpchQ3CustRec rec;
        rec.key = static_cast<uint64_t>(row.Int(0));
        rec.segment = static_cast<uint8_t>(partition);
        into.Add(rec);
      },
      core::RowPieces<TpchQ3CustRec>::Merge);
  CustBuilder custs(core::PieceRows(scanned_custs.pieces));
  custs.AddPieces(scanned_custs.pieces, core::Parallelism::OnPool(services));
  SortShards(services, custs);

  auto orders = ScanOrders(services, *initial_scan, custs);
  auto staging = StageLineitems(services, *initial_scan, orders);
  for (auto &[id, segment] : BuildSegments(services, staging))
    built.segments[id] = segment;
  built.side.orders = FinishShards(services, orders);
  built.side.custs = FinishShards(services, custs);
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
