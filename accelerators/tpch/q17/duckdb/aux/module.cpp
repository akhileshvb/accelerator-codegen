#include "tpch/q17/duckdb/aux/module.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/q17/calibration_writes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {


// Parts per slice (~4000 us): sorting a part's few (quantity, price) buckets is ~1 us.
constexpr std::size_t kPartStatsGrain = 4096;
// Pages per slice (~800 us): sorting one 1024-part page is ~50 us.
constexpr std::size_t kPageSortGrain = 16;
// Page runs per slice (~160 us): restaging one 1024-part page run is ~20 us.
constexpr std::size_t kStageRunGrain = 8;

core::PartitionId AttrPartsPartition(const std::string &brand,
                                     const std::string &container) {
  const auto key = TpchQ17State::MakeAttrKey(brand, container);
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : key) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return TpchQ17Layout::kAttrPartsBase +
         static_cast<core::PartitionId>(hash % TpchQ17Layout::kAttrPartsSlots);
}

std::shared_ptr<const TpchQ17StateImage>
MakeChunkImage(const std::shared_ptr<const TpchQ17PartStatsChunk> &chunk) {
  auto image = std::make_shared<TpchQ17StateImage>();
  image->kind = TpchQ17ImageKind::PartStatsChunk;
  image->chunk_index = chunk ? chunk->chunk_index : 0;
  image->chunk = chunk;
  return image;
}

std::shared_ptr<const TpchQ17StateImage>
MakeAttrPartsImage(const std::shared_ptr<const TpchQ17AttrParts> &attr_parts) {
  auto image = std::make_shared<TpchQ17StateImage>();
  image->kind = TpchQ17ImageKind::AttrParts;
  image->attr_parts = attr_parts;
  return image;
}

const TpchQ17StateImage *AsQ17Image(const core::StateImage &image,
                                    TpchQ17ImageKind kind) {
  const auto *payload = image.As<TpchQ17StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  if (kind == TpchQ17ImageKind::PartStatsChunk && !payload->chunk)
    return nullptr;
  if (kind == TpchQ17ImageKind::AttrParts && !payload->attr_parts)
    return nullptr;
  return payload;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ17LineitemInput &out) {
  int64_t partkey = 0, quantity_raw = 0, extended_raw = 0;
  int32_t quantity_scale = 0, extended_scale = 0;
  if (!core::IntCell(row, 0, partkey) ||
      !core::DecimalCell(row, 1, quantity_raw, quantity_scale) ||
      !core::DecimalCell(row, 2, extended_raw, extended_scale))
    return false;
  out.partkey = partkey;
  out.quantity_raw = quantity_raw;
  out.extendedprice_raw = extended_raw;
  return true;
}


using Pair = TpchQ17QtyPricePair;
using PairVec = std::vector<Pair>;

constexpr int64_t kInt32Max = std::numeric_limits<int32_t>::max();

int64_t PageOf(int64_t partkey) {
  return partkey >> TpchQ17Layout::kPartChunkShift;
}

core::PartitionId PagePartition(int64_t page) {
  return TpchQ17Layout::kPartStatsChunkBase +
         static_cast<core::PartitionId>(page);
}

int64_t QuantityThresholdRaw(int64_t sum_qty_raw, int32_t count) {
  if (count <= 0)
    return 0;
  const double avg = static_cast<double>(sum_qty_raw) / 100.0 /
                     static_cast<double>(count);
  return static_cast<int64_t>(std::ceil(0.2 * avg * 100.0));
}

// Sum of prices of buckets with quantity < 0.2 * avg (pairs sorted by qty).
int64_t ContributionRaw(int64_t sum_qty_raw, int32_t count, const Pair *begin,
                        const Pair *end) {
  if (count <= 0 || begin == end)
    return 0;
  const int64_t threshold = QuantityThresholdRaw(sum_qty_raw, count);
  int64_t total = 0;
  for (const Pair *p = begin; p != end && p->quantity_raw < threshold; ++p)
    total += p->extendedprice_raw;
  return total;
}

// Applies one lineitem delta to a sorted bucket vector; false if a delete
// finds no matching bucket (state inconsistent with the delta).
bool ApplyPairDelta(PairVec &pairs, const TpchQ17LineitemDelta &delta) {
  const auto qty = static_cast<int32_t>(delta.quantity_raw);
  auto pos = std::lower_bound(
      pairs.begin(), pairs.end(), qty,
      [](const Pair &p, int32_t t) { return p.quantity_raw < t; });
  const bool found = pos != pairs.end() && pos->quantity_raw == qty;
  if (delta.sign > 0) {
    if (found) {
      pos->row_count += 1;
      pos->extendedprice_raw += delta.extendedprice_raw;
    } else {
      pairs.insert(pos, Pair{qty, 1, delta.extendedprice_raw});
    }
    return true;
  }
  if (!found)
    return false;
  pos->row_count -= 1;
  pos->extendedprice_raw -= delta.extendedprice_raw;
  if (pos->row_count <= 0)
    pairs.erase(pos);
  return true;
}

bool ReadPartRow(const core::AuxRow &row, int64_t &partkey, std::string &brand,
                 std::string &container) {
  return core::IntCell(row, 0, partkey) && core::StringCell(row, 1, brand) &&
         core::StringCell(row, 2, container);
}

// Net change of each (brand, container) total, plus names to create it.
struct AttrDelta {
  int64_t total_delta = 0;
  std::string brand, container;
  bool named = false;
};
using AttrMap = std::map<core::PartitionId, AttrDelta>;

// The page runs of one commit: its deltas ordered by (partkey, arrival), so
// each 1024-part page is a contiguous run and is copied exactly once.
class PageStager {
public:
  struct Result {
    std::shared_ptr<const TpchQ17StateImage> image;
    core::Version version = 0;
    int64_t page = 0;
    bool missing = false; // no published page: only legal on an empty state
  };

  // Reads the published pages on the calling thread; StageRun only computes.
  PageStager(core::AuxEngineServices &services, const TpchQ17TxnBuffer &buffer)
      : buffer(buffer) {
    ops.reserve(buffer.LineitemDeltas().size() + buffer.PartDeltas().size());
    uint32_t seq = 0;
    uint32_t i = 0;
    for (const auto &delta : buffer.PartDeltas())
      ops.push_back({delta.partkey, seq++, true, i++});
    i = 0;
    for (const auto &delta : buffer.LineitemDeltas())
      ops.push_back({delta.partkey, seq++, false, i++});
    std::sort(ops.begin(), ops.end(), [](const Op &a, const Op &b) {
      return a.partkey != b.partkey ? a.partkey < b.partkey : a.seq < b.seq;
    });
    run_starts = core::RunStarts(ops, [](const Op &op) { return PageOf(op.partkey); });

    const auto visibility = services.CurrentVisibility();
    results.resize(RunCount());
    snapshots.reserve(RunCount());
    for (std::size_t r = 0; r < RunCount(); ++r)
      snapshots.push_back(services.StateStore().ReadPartitions(
          visibility, {PagePartition(PageOf(ops[run_starts[r]].partkey))}));
  }

  std::size_t RunCount() const { return run_starts.size() - 1; }

  // Stages page run r into results[r]; false asks for a full rebuild.
  bool StageRun(std::size_t r, AttrMap &attr_deltas);

  std::vector<Result> results;

private:
  struct Op {
    int64_t partkey;
    uint32_t seq;
    bool is_part;
    uint32_t index;
  };

  const TpchQ17TxnBuffer &buffer;
  std::vector<Op> ops;
  std::vector<std::size_t> run_starts; // page runs over `ops`, end sentinel last
  std::vector<core::StateSnapshot> snapshots;
};

bool PageStager::StageRun(std::size_t r, AttrMap &attr_deltas) {
  const std::size_t begin = run_starts[r], end = run_starts[r + 1];
  const auto page_index = PageOf(ops[begin].partkey);
  auto &result = results[r];
  result.page = page_index;

  TpchQ17PartStatsChunk chunk;
  chunk.chunk_index = page_index;
  bool have = false;
  for (const auto &image : snapshots[r].partitions) {
    const auto *typed = AsQ17Image(image, TpchQ17ImageKind::PartStatsChunk);
    if (!typed || !typed->chunk)
      continue;
    chunk.parts.reserve(typed->chunk->parts.size() + (end - begin));
    chunk.parts = typed->chunk->parts; // copy-on-write of one small page
    result.version = image.version;
    have = true;
  }
  result.missing = !have;
  auto &parts = chunk.parts;

  for (std::size_t i = begin; i < end;) {
    const auto partkey = ops[i].partkey;
    std::size_t k = i;
    while (k < end && ops[k].partkey == partkey)
      ++k;
    auto pos = std::lower_bound(
        parts.begin(), parts.end(), partkey,
        [](const TpchQ17PartEntry &e, int64_t key) { return e.partkey < key; });
    if (pos == parts.end() || pos->partkey != partkey) {
      TpchQ17PartEntry fresh;
      fresh.partkey = partkey;
      pos = parts.insert(pos, std::move(fresh));
    }
    TpchQ17PartEntry &entry = *pos;
    const auto old_attr = entry.attr_partition;
    const auto old_contribution = entry.contribution_raw;

    bool has_lineitems = false;
    for (std::size_t o = i; o < k; ++o) {
      if (!ops[o].is_part) {
        has_lineitems = true;
        continue;
      }
      const auto &op = buffer.PartDeltas()[ops[o].index];
      const auto op_attr = AttrPartsPartition(op.brand, op.container);
      if (op.sign < 0) {
        if (entry.attr_partition != op_attr)
          return false;
        entry.attr_partition = 0;
      } else {
        if (entry.attr_partition != 0)
          return false;
        entry.attr_partition = op_attr;
        auto &ad = attr_deltas[op_attr];
        ad.brand = op.brand;
        ad.container = op.container;
        ad.named = true;
      }
    }

    if (has_lineitems) {
      PairVec pairs;
      if (entry.pairs) {
        pairs.reserve(entry.pairs->size() + (k - i));
        pairs = *entry.pairs;
      }
      for (std::size_t o = i; o < k; ++o) {
        if (ops[o].is_part)
          continue;
        const auto &op = buffer.LineitemDeltas()[ops[o].index];
        if (!ApplyPairDelta(pairs, op))
          return false;
        entry.sum_qty_raw += static_cast<int64_t>(op.sign) * op.quantity_raw;
        entry.count += op.sign;
      }
      if (entry.count < 0)
        return false;
      entry.contribution_raw = ContributionRaw(entry.sum_qty_raw, entry.count, pairs.data(),
                                               pairs.data() + pairs.size());
      if (pairs.empty())
        entry.pairs.reset();
      else
        entry.pairs = std::make_shared<const PairVec>(std::move(pairs));
    }

    if (old_attr != 0)
      attr_deltas[old_attr].total_delta -= old_contribution;
    if (entry.attr_partition != 0)
      attr_deltas[entry.attr_partition].total_delta += entry.contribution_raw;
    if (entry.attr_partition == 0 && entry.count == 0 && !entry.pairs)
      parts.erase(pos);
    i = k;
  }
  result.image =
      MakeChunkImage(std::make_shared<const TpchQ17PartStatsChunk>(std::move(chunk)));
  return true;
}

// Distinct pages the commit touches.
std::size_t TouchedPages(const TpchQ17TxnBuffer &buffer) {
  std::vector<int64_t> pages;
  pages.reserve(buffer.LineitemDeltas().size() + buffer.PartDeltas().size());
  for (const auto &d : buffer.LineitemDeltas())
    pages.push_back(PageOf(d.partkey));
  for (const auto &d : buffer.PartDeltas())
    pages.push_back(PageOf(d.partkey));
  std::sort(pages.begin(), pages.end());
  return static_cast<std::size_t>(std::unique(pages.begin(), pages.end()) - pages.begin());
}

// Appends the new image of every (brand, container) partition whose total
// moved. False when a partition needs names the commit did not supply.
bool StageAttrImages(const core::AuxStateStore &store, const core::VisibilityToken &visibility,
                     const AttrMap &attr_deltas, core::StagedMaintenanceOutcome &outcome) {
  for (const auto &[attr_partition, ad] : attr_deltas) {
    if (ad.total_delta == 0)
      continue; // an absent AttrParts image already reads as NULL
    const auto snapshot = store.ReadPartitions(visibility, {attr_partition});
    std::shared_ptr<TpchQ17AttrParts> next;
    core::Version version = 0;
    for (const auto &image : snapshot.partitions) {
      const auto *typed = AsQ17Image(image, TpchQ17ImageKind::AttrParts);
      if (!typed || !typed->attr_parts)
        continue;
      next = std::make_shared<TpchQ17AttrParts>(*typed->attr_parts);
      version = image.version;
      break;
    }
    if (next && ad.named && (next->brand != ad.brand || next->container != ad.container))
      next->shared = true; // a second pair in this partition
    if (!next) {
      if (!ad.named)
        return false;
      next = std::make_shared<TpchQ17AttrParts>();
      next->brand = ad.brand;
      next->container = ad.container;
    }
    next->total_contribution_raw += ad.total_delta;
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ17Layout::kAccelerator, attr_partition, version,
        MakeAttrPartsImage(std::const_pointer_cast<const TpchQ17AttrParts>(next))));
  }
  return true;
}

// FNV-1a over brand, '\0', container: the same hash as AttrPartsPartition,
// without materializing the key.
core::PartitionId AttrPartsPartitionView(const std::string &brand,
                                                const std::string &container) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : brand) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  hash *= 1099511628211ULL; // the '\0' separator: hash ^= 0
  for (const unsigned char c : container) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return TpchQ17Layout::kAttrPartsBase +
         static_cast<core::PartitionId>(hash % TpchQ17Layout::kAttrPartsSlots);
}

const std::string *StringArg(const core::QueryArguments &arguments,
                                    std::size_t i) {
  if (arguments.values.size() <= i || arguments.values[i].is_null)
    return nullptr;
  return std::get_if<std::string>(&arguments.values[i].payload);
}

// One-column nullable DOUBLE result, built once.
const core::OutputSchema &Q17Schema() {
  static const core::OutputSchema schema{
      {{"avg_yearly", core::AuxType::Double, true}}};
  return schema;
}

void EmitQ17(core::AuxRowSink &sink, int64_t contribution_raw) {
  thread_local core::AuxRow row = [] {
    core::AuxRow r;
    r.values.resize(1);
    return r;
  }();
  sink.Begin(Q17Schema());
  if (contribution_raw == 0) {
    row.values[0].type = core::AuxType::Double;
    row.values[0].is_null = true;
  } else {
    row.values[0].type = core::AuxType::Double;
    row.values[0].is_null = false;
    row.values[0].payload =
        static_cast<double>(contribution_raw) / 100.0 / 7.0;
  }
  sink.WriteRow(row);
  sink.End();
}

// Statistics of the lineitems seen per part key, in hash shards.
struct PartStatsShards {
  static constexpr std::size_t kShards = 64;
  std::array<std::unordered_map<int64_t, TpchQ17PartStats>, kShards> shards;

  static std::size_t ShardOf(int64_t partkey) {
    return (static_cast<uint64_t>(partkey) * 0x9E3779B97F4A7C15ULL) >> 58;
  }
  static void AddBucket(TpchQ17PartStats &stats, int64_t quantity_raw, int64_t rows,
                        int64_t price_raw) {
    if (quantity_raw != static_cast<int32_t>(quantity_raw) ||
        rows != static_cast<int32_t>(rows))
      throw std::runtime_error("TPC-H Q17: quantity or row count exceeds 32 bits");
    stats.sum_qty_raw += quantity_raw * rows;
    stats.count += static_cast<int32_t>(rows);
    for (auto &pair : stats.qty_price_pairs)
      if (pair.quantity_raw == quantity_raw) {
        pair.row_count += static_cast<int32_t>(rows);
        pair.extendedprice_raw += price_raw;
        return;
      }
    stats.qty_price_pairs.push_back({static_cast<int32_t>(quantity_raw),
                                     static_cast<int32_t>(rows), price_raw});
  }
  void Add(const TpchQ17LineitemInput &input) {
    AddBucket(shards[ShardOf(input.partkey)][input.partkey], input.quantity_raw, 1,
              input.extendedprice_raw);
  }
  // Moves shard `shard` of `from` into this one's.
  void FoldShard(std::size_t shard, PartStatsShards &&from) {
    for (auto &[partkey, theirs] : from.shards[shard]) {
      auto &mine = shards[shard][partkey];
      for (const auto &pair : theirs.qty_price_pairs)
        AddBucket(mine, pair.quantity_raw, pair.row_count, pair.extendedprice_raw);
    }
  }
};

} // namespace

void TpchQ17PartStats::RecomputeContribution() {
  precomputed_contribution_raw =
      ContributionRaw(sum_qty_raw, count, qty_price_pairs.data(),
                      qty_price_pairs.data() + qty_price_pairs.size());
}

std::string TpchQ17State::MakeAttrKey(const std::string &brand,
                                      const std::string &container) {
  std::string key;
  key.reserve(brand.size() + 1 + container.size());
  key.append(brand);
  key.push_back('\0');
  key.append(container);
  return key;
}

void TpchQ17State::SplitAttrKey(const std::string &key, std::string &brand,
                                std::string &container) {
  const auto nul = key.find('\0');
  if (nul == std::string::npos) {
    brand = key;
    container.clear();
    return;
  }
  brand = key.substr(0, nul);
  container = key.substr(nul + 1);
}

bool TpchQ17TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ17TxnBuffer::Clear() {
  pending = false;
  part_rebuild = false;
  lineitem_deltas.clear();
  part_deltas.clear();
}
bool TpchQ17TxnBuffer::RequiresRebuild() const { return false; }
bool TpchQ17TxnBuffer::HasPartRebuild() const { return part_rebuild; }
const std::vector<TpchQ17LineitemDelta> &
TpchQ17TxnBuffer::LineitemDeltas() const {
  return lineitem_deltas;
}
const std::vector<TpchQ17PartDelta> &TpchQ17TxnBuffer::PartDeltas() const {
  return part_deltas;
}
void TpchQ17TxnBuffer::MarkPartRebuild() {
  pending = true;
  part_rebuild = true;
  lineitem_deltas.clear();
  part_deltas.clear();
}
void TpchQ17TxnBuffer::AddLineitemDelta(TpchQ17LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ17TxnBuffer::AddPartDelta(TpchQ17PartDelta delta) {
  pending = true;
  part_deltas.push_back(delta);
}

core::AcceleratorId TpchQ17Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ17Module::Dependencies() const {
  return {{config.part_table, {0, 1, 2}, {0, 3, 6}},
          {config.lineitem_table, {0, 1, 2}, {1, 4, 5}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ17Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ17TxnBuffer>();
}

void TpchQ17Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = BuildState(services);

  std::unordered_map<int64_t, TpchQ17PartStatsChunk> pages;
  std::unordered_map<core::PartitionId, TpchQ17AttrParts> attrs;

  // Pass 1: parts that have lineitems (stats moved into shared buckets).
  for (auto &[partkey, stats] : state.part_stats) {
    const auto page_index = PageOf(partkey);
    auto &page = pages[page_index];
    page.chunk_index = page_index;
    TpchQ17PartEntry entry;
    entry.partkey = partkey;
    entry.sum_qty_raw = stats.sum_qty_raw;
    entry.count = stats.count;
    entry.contribution_raw = stats.precomputed_contribution_raw;
    if (!stats.qty_price_pairs.empty()) {
      stats.qty_price_pairs.shrink_to_fit();
      entry.pairs = std::make_shared<const PairVec>(
          std::move(stats.qty_price_pairs));
    }
    page.parts.push_back(std::move(entry));
  }
  state.part_stats = {};
  state.part_attr_keys = {};
  const auto by_key = [](const TpchQ17PartEntry &a, const TpchQ17PartEntry &b) {
    return a.partkey < b.partkey;
  };
  std::vector<TpchQ17PartStatsChunk *> page_list;
  page_list.reserve(pages.size());
  for (auto &entry : pages)
    page_list.push_back(&entry.second);
  core::ParallelEach(services, page_list.size(), kPageSortGrain, [&](std::size_t i) {
    std::sort(page_list[i]->parts.begin(), page_list[i]->parts.end(), by_key);
  });

  // Pass 2: attach (brand, container); parts without lineitems get an entry.
  std::unordered_map<int64_t, std::vector<TpchQ17PartEntry>> extras;
  for (auto &[attr_key, partkeys] : state.attribute_index) {
    std::string brand, container;
    TpchQ17State::SplitAttrKey(attr_key, brand, container);
    const auto attr_partition = AttrPartsPartition(brand, container);
    auto &attr = attrs[attr_partition];
    if (!attr.brand.empty() && (attr.brand != brand || attr.container != container))
      attr.shared = true;
    attr.brand = brand;
    attr.container = container;
    for (const auto partkey : partkeys) {
      const auto page_index = PageOf(partkey);
      auto page_it = pages.find(page_index);
      TpchQ17PartEntry *found = nullptr;
      if (page_it != pages.end()) {
        auto &parts = page_it->second.parts;
        auto pos = std::lower_bound(
            parts.begin(), parts.end(), partkey,
            [](const TpchQ17PartEntry &e, int64_t k) { return e.partkey < k; });
        if (pos != parts.end() && pos->partkey == partkey)
          found = &*pos;
      }
      if (!found) {
        auto &ex = extras[page_index];
        auto pos = std::find_if(ex.begin(), ex.end(),
                                [&](const TpchQ17PartEntry &e) {
                                  return e.partkey == partkey;
                                });
        if (pos == ex.end()) {
          ex.emplace_back();
          ex.back().partkey = partkey;
          pos = ex.end() - 1;
        }
        found = &*pos;
      }
      found->attr_partition = attr_partition;
      attr.total_contribution_raw += found->contribution_raw;
    }
  }
  state.attribute_index = {};
  for (auto &[page_index, ex] : extras) {
    auto &page = pages[page_index];
    page.chunk_index = page_index;
    for (auto &e : ex)
      page.parts.push_back(std::move(e));
    std::sort(page.parts.begin(), page.parts.end(), by_key);
  }

  for (auto &[page_index, page] : pages) {
    const auto chunk_ptr =
        std::make_shared<const TpchQ17PartStatsChunk>(std::move(page));
    builder.Write(core::StateImage::Make(TpchQ17Layout::kAccelerator,
                                         PagePartition(page_index), 0,
                                         MakeChunkImage(chunk_ptr)));
  }
  for (auto &[partition, attr] : attrs) {
    const auto parts_ptr =
        std::make_shared<const TpchQ17AttrParts>(std::move(attr));
    builder.Write(core::StateImage::Make(TpchQ17Layout::kAccelerator, partition,
                                         0, MakeAttrPartsImage(parts_ptr)));
  }

  builder.Finish();
}

core::InvalidationPlan
TpchQ17Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.part_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ17Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ17TxnBuffer &>(base_buffer);

  if (mutation.table == config.part_table) {
    auto add_part = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ17PartDelta delta;
      if (!ReadPartRow(row, delta.partkey, delta.brand, delta.container)) {
        buffer.MarkPartRebuild();
        return;
      }
      delta.sign = sign;
      buffer.AddPartDelta(std::move(delta));
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        add_part(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        add_part(row, -1);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkPartRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, nk = 0;
        std::string ob, oc, nb, nc;
        if (ReadPartRow(mutation.old_rows[i], ok, ob, oc) &&
            ReadPartRow(mutation.new_rows[i], nk, nb, nc) && ok == nk &&
            ob == nb && oc == nc)
          continue; // watched columns unchanged
        add_part(mutation.old_rows[i], -1);
        add_part(mutation.new_rows[i], 1);
      }
      break;
    }
    return;
  }

  if (mutation.table != config.lineitem_table)
    return;

  auto add_lineitem = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ17LineitemInput input;
    if (!ReadLineitem(row, input) ||
        std::llabs(input.quantity_raw) > kInt32Max) {
      buffer.MarkPartRebuild();
      return;
    }
    buffer.AddLineitemDelta(
        {input.partkey, input.quantity_raw, input.extendedprice_raw, sign});
  };

  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows)
      add_lineitem(row, 1);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows)
      add_lineitem(row, -1);
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkPartRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      add_lineitem(mutation.old_rows[i], -1);
      add_lineitem(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ17Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ17TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.HasPartRebuild()) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (buffer.LineitemDeltas().empty() && buffer.PartDeltas().empty())
    return outcome;

  auto request = core::MaintenanceRequest::IncrementalSingleState(
      buffer.LineitemDeltas().size() + buffer.PartDeltas().size(),
      services.ExpectedQueriesBeforeNextWrite());
  request.affected_partition_count = std::max<std::size_t>(1, TouchedPages(buffer));
  if (services.ChooseMaintenance(request).action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  PageStager stager(services, buffer);
  struct Staged {
    AttrMap attrs;
    bool rebuild = false;
  };
  Staged staged;
  core::ParallelReduce(
      services, stager.RunCount(), kStageRunGrain, staged,
      [&](Staged &acc, std::size_t first, std::size_t last) {
        for (std::size_t r = first; r < last && !acc.rebuild; ++r)
          acc.rebuild = !stager.StageRun(r, acc.attrs);
      },
      [](Staged &into, Staged &&from) {
        into.rebuild |= from.rebuild;
        for (auto &[partition, delta] : from.attrs) {
          auto &merged = into.attrs[partition];
          merged.total_delta += delta.total_delta;
          if (delta.named) {
            merged.brand = std::move(delta.brand);
            merged.container = std::move(delta.container);
            merged.named = true;
          }
        }
      });
  const auto visibility = services.CurrentVisibility();
  const auto &store = services.StateStore();
  const bool unpublished_page_on_invalid_state =
      std::any_of(stager.results.begin(), stager.results.end(), [&](const auto &result) {
        return result.missing && !store.CheckValidity({Id(), {}}, visibility).IsValid();
      });
  if (staged.rebuild || unpublished_page_on_invalid_state) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.reserve(stager.results.size() + staged.attrs.size());
  for (auto &result : stager.results) {
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ17Layout::kAccelerator, PagePartition(result.page), result.version,
        std::move(result.image)));
  }
  if (!StageAttrImages(store, visibility, staged.attrs, outcome))
    core::FinishRebuildAll(outcome);
  return outcome;
}

bool TpchQ17Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ17Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  const std::string *brand = StringArg(arguments, 0);
  const std::string *container = StringArg(arguments, 1);
  if (!brand || !container || brand->empty() || container->empty() ||
      arguments.values.size() < 2) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto &store = services.StateStore();
  const auto visibility = services.CurrentVisibility();
  const auto partition = AttrPartsPartitionView(*brand, *container);

  // One observation: whole-state validity plus the partition's handle.
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = partition;
  const auto snapshot = store.ReadHitPartitions(visibility, hit_key, wanted);
  if (!snapshot.available) {
    if (!store.CheckValidity(hit_key, visibility).IsValid()) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    // A valid state without this brand/container's partition: no such part.
    EmitQ17(sink, 0);
    return;
  }
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ17Image(image, TpchQ17ImageKind::AttrParts);
    if (!typed)
      continue;
    const auto &attr = *typed->attr_parts;
    if (attr.shared) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    if (attr.brand != *brand || attr.container != *container)
      continue;
    EmitQ17(sink, attr.total_contribution_raw);
    return;
  }
  EmitQ17(sink, 0);
}

TpchQ17State TpchQ17Module::BuildState(
    core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q17 requires a typed initial scan source");

  TpchQ17State state = core::PartitionedBuild<TpchQ17State>(
      services, [] { return TpchQ17State{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanParts(services, parts,
                                [&](std::size_t p, const TpchQ17PartInput &input) {
                                  sink(p, core::SingleRowBatch<TpchQ17PartInput>{input});
                                });
      },
      [](TpchQ17State &partial, const TpchQ17PartInput &input) {
        const auto key = TpchQ17State::MakeAttrKey(input.brand, input.container);
        partial.attribute_index[key].insert(input.key);
        partial.part_attr_keys[input.key] = key;
      },
      [](TpchQ17State &into, TpchQ17State &&from) {
        for (auto &[key, partkeys] : from.attribute_index)
          into.attribute_index[key].insert(partkeys.begin(), partkeys.end());
        for (auto &[partkey, key] : from.part_attr_keys)
          into.part_attr_keys[partkey] = std::move(key);
      });

  // Lineitem statistics per part, in hash shards so partial states fold shard
  // by shard in parallel.
  auto shards = core::PartitionedBuild<PartStatsShards>(
      services, [] { return PartStatsShards{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ17LineitemInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ17LineitemInput>{input});
                                    });
      },
      [](PartStatsShards &partial, const TpchQ17LineitemInput &input) {
        partial.Add(input);
      },
      [&](PartStatsShards &into, PartStatsShards &&from) {
        core::ParallelEach(services, PartStatsShards::kShards, 1,
                           [&](std::size_t s) { into.FoldShard(s, std::move(from)); });
      });
  std::vector<TpchQ17PartStats *> stats_list;
  for (auto &shard : shards.shards)
    for (auto &entry : shard)
      stats_list.push_back(&entry.second);
  core::ParallelEach(services, stats_list.size(), kPartStatsGrain, [&](std::size_t i) {
    auto &pairs = stats_list[i]->qty_price_pairs;
    std::sort(pairs.begin(), pairs.end(),
              [](const TpchQ17QtyPricePair &lhs, const TpchQ17QtyPricePair &rhs) {
                return lhs.quantity_raw < rhs.quantity_raw;
              });
    stats_list[i]->RecomputeContribution();
  });
  state.part_stats.reserve(stats_list.size());
  for (auto &shard : shards.shards)
    for (auto &entry : shard)
      state.part_stats.emplace(entry.first, std::move(entry.second));
  return state;
}

// The fallback: the answer from a fresh build of the state.
void TpchQ17Module::EmitResult(core::AuxEngineServices &services,
                               const std::string &brand,
                               const std::string &container,
                               core::AuxRowSink &sink) const {
  int64_t contribution_raw = 0;
  if (!brand.empty() && !container.empty()) {
    const auto state = BuildState(services);
    const auto parts =
        state.attribute_index.find(TpchQ17State::MakeAttrKey(brand, container));
    if (parts != state.attribute_index.end()) {
      for (const auto partkey : parts->second) {
        const auto stats = state.part_stats.find(partkey);
        if (stats != state.part_stats.end())
          contribution_raw += stats->second.precomputed_contribution_raw;
      }
    }
  }
  sink.Begin({{{"avg_yearly", core::AuxType::Double, true}}});
  if (contribution_raw == 0) {
    sink.WriteRow({{core::AuxValue::Null(core::AuxType::Double)}});
  } else {
    sink.WriteRow({{core::AuxValue::Of(
        core::AuxType::Double,
        static_cast<double>(contribution_raw) / 100.0 / 7.0)}});
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ17Module::CalibrationWrites() const {
  return TpchQ17CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
