#include "tpch/q11/duckdb/aux/module.hpp"
#include "tpch/q11/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/dealt_rows.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

// Supplier buckets per slice when dealing their parts to nations.
constexpr std::size_t kSupplierBucketGrain = 16;
// Value buckets or supplier-part buckets per slice when aggregating a nation.
constexpr std::size_t kBucketGrain = 4;
// Rows per slice of a nation's sort, and chunks per slice when cutting it.
constexpr std::size_t kSortGrain = 65536;
constexpr std::size_t kChunkGrain = 64;
// Nation ops below which a commit applies its nations inline (untimed).
constexpr std::size_t kNationOpThreshold = 2048;
// Deltas in a commit below which the supplier bookkeeping stays on one thread (untimed).
constexpr std::size_t kBookkeepOpThreshold = 2048;

// Scale of the emitted value column.
constexpr int32_t kValueScale = 2;

#if defined(__SIZEOF_INT128__)
void AddAcc(Q11AccT &accumulator, int64_t supplycost_raw, int64_t availqty) {
  accumulator += static_cast<Q11AccT>(supplycost_raw) *
                 static_cast<Q11AccT>(availqty);
}
#else
void AddAcc(Q11AccT &accumulator, int64_t supplycost_raw, int64_t availqty) {
  accumulator += supplycost_raw * availqty;
}
#endif

std::shared_ptr<const TpchQ11StateImage>
MakeMetadataImage(const std::shared_ptr<const TpchQ11State> &state) {
  auto image = std::make_shared<TpchQ11StateImage>();
  image->kind = TpchQ11ImageKind::Metadata;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ11StateImage>
MakeNationImage(int32_t nationkey, const std::string &nation_name,
                std::shared_ptr<const TpchQ11NationData> data) {
  auto image = std::make_shared<TpchQ11StateImage>();
  image->kind = TpchQ11ImageKind::NationPreAggregated;
  image->nationkey = nationkey;
  image->nation_name = nation_name;
  image->data = std::move(data);
  return image;
}

const TpchQ11StateImage *AsQ11Image(const core::StateImage &image,
                                     TpchQ11ImageKind kind) {
  const auto *payload = image.As<TpchQ11StateImage>();
  return payload && payload->kind == kind ? payload : nullptr;
}

bool ReadPartsupp(const core::AuxRow &row, TpchQ11PartsuppInput &out) {
  int64_t partkey = 0, suppkey = 0, availqty = 0;
  int64_t supplycost_raw = 0;
  int32_t supplycost_scale = 0;
  if (!core::IntCell(row, 0, partkey) || !core::IntCell(row, 1, suppkey) ||
      !core::IntCell(row, 2, availqty) ||
      !core::DecimalCell(row, 3, supplycost_raw, supplycost_scale))
    return false;
  out.partkey = partkey;
  out.suppkey = suppkey;
  out.availqty = availqty;
  out.supplycost_raw = supplycost_raw;
  out.supplycost_scale = supplycost_scale;
  return true;
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey,
                  int32_t &nationkey) {
  return core::IntCell(row, 0, suppkey) && core::Int32Cell(row, 1, nationkey);
}

bool ReadNation(const core::AuxRow &row, int32_t &nationkey,
                std::string &name) {
  return core::Int32Cell(row, 0, nationkey) && core::StringCell(row, 1, name);
}

struct SupplierRow {
  int64_t suppkey = 0;
  int32_t nationkey = 0;
};

struct PartsuppRow {
  int64_t suppkey = 0;
  Q11SupplierPart part;
};

bool RowBefore(const TpchQ11OutputRow &lhs, const TpchQ11OutputRow &rhs) {
  if (lhs.value != rhs.value)
    return lhs.value > rhs.value;
  return lhs.partkey < rhs.partkey;
}

std::shared_ptr<const TpchQ11NationData> EmptyNationData() {
  auto data = std::make_shared<TpchQ11NationData>();
  data->value_buckets.assign(kQ11ValueBuckets, nullptr);
  return data;
}

// Copy-on-write working copy of a nation aggregate.  Only the touched value
// bucket(s) and sorted chunk(s) are copied; everything else is shared with the
// published base.  The base is never mutated.
class NationWork {
public:
  explicit NationWork(std::shared_ptr<const TpchQ11NationData> base)
      : data(*base), bucket_owned(kQ11ValueBuckets, 0) {
    if (data.value_buckets.size() != kQ11ValueBuckets)
      data.value_buckets.resize(kQ11ValueBuckets);
  }

  // Returns false on internal inconsistency (caller must rebuild).
  bool Apply(int64_t partkey, Q11AccT signed_value) {
    data.total += signed_value;
    const std::size_t bi = Q11ValueBucketIndex(partkey);
    const auto &bucket = data.value_buckets[bi];
    if (bucket) {
      const auto it = bucket->find(partkey);
      if (it != bucket->end()) {
        const Q11AccT old_value = it->second;
        const Q11AccT new_value = old_value + signed_value;
        if (new_value == old_value)
          return true;
        if (!RemoveRow({partkey, old_value}))
          return false;
        auto &mutable_bucket = MutBucket(bi);
        if (new_value <= 0) {
          mutable_bucket.erase(partkey);
        } else {
          mutable_bucket[partkey] = new_value;
          InsertRow({partkey, new_value});
        }
        return true;
      }
    }
    if (signed_value <= 0)
      return true;
    MutBucket(bi)[partkey] = signed_value;
    InsertRow({partkey, signed_value});
    return true;
  }

  std::shared_ptr<const TpchQ11NationData> Finish() {
    return std::make_shared<const TpchQ11NationData>(std::move(data));
  }

private:
  Q11ValueBucket &MutBucket(std::size_t bi) {
    auto &slot = data.value_buckets[bi];
    if (!bucket_owned[bi]) {
      slot = slot ? std::make_shared<Q11ValueBucket>(*slot)
                  : std::make_shared<Q11ValueBucket>();
      bucket_owned[bi] = 1;
    }
    // Created by this working copy via make_shared<non-const>; safe to mutate.
    return const_cast<Q11ValueBucket &>(*slot);
  }

  Q11SortedChunk &MutChunk(std::size_t ci) {
    auto &slot = data.chunks[ci];
    if (!owned_chunks.count(slot.get())) {
      auto copy = std::make_shared<Q11SortedChunk>(*slot);
      owned_chunks.insert(copy.get());
      slot = std::move(copy);
    }
    return const_cast<Q11SortedChunk &>(*slot);
  }

  // First chunk whose last row is not ordered before `key`.
  std::size_t FindChunk(const TpchQ11OutputRow &key) const {
    std::size_t lo = 0, hi = data.chunks.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      if (RowBefore(data.chunks[mid]->rows.back(), key))
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  bool RemoveRow(const TpchQ11OutputRow &row) {
    const std::size_t ci = FindChunk(row);
    if (ci >= data.chunks.size())
      return false;
    const auto &rows = data.chunks[ci]->rows;
    const auto pos = std::lower_bound(rows.begin(), rows.end(), row, RowBefore);
    if (pos == rows.end() || pos->partkey != row.partkey ||
        pos->value != row.value)
      return false;
    const auto offset = pos - rows.begin();
    auto &chunk = MutChunk(ci);
    chunk.rows.erase(chunk.rows.begin() + offset);
    if (chunk.rows.empty()) {
      owned_chunks.erase(data.chunks[ci].get());
      data.chunks.erase(data.chunks.begin() + static_cast<std::ptrdiff_t>(ci));
    }
    return true;
  }

  void InsertRow(const TpchQ11OutputRow &row) {
    if (data.chunks.empty()) {
      auto chunk = std::make_shared<Q11SortedChunk>();
      chunk->rows.push_back(row);
      owned_chunks.insert(chunk.get());
      data.chunks.push_back(std::move(chunk));
      return;
    }
    std::size_t ci = FindChunk(row);
    if (ci >= data.chunks.size())
      ci = data.chunks.size() - 1;
    auto &chunk = MutChunk(ci);
    const auto pos =
        std::lower_bound(chunk.rows.begin(), chunk.rows.end(), row, RowBefore);
    chunk.rows.insert(pos, row);
    if (chunk.rows.size() > kQ11ChunkMax) {
      auto tail = std::make_shared<Q11SortedChunk>();
      const auto mid = chunk.rows.begin() +
                       static_cast<std::ptrdiff_t>(chunk.rows.size() / 2);
      tail->rows.assign(mid, chunk.rows.end());
      chunk.rows.erase(mid, chunk.rows.end());
      owned_chunks.insert(tail.get());
      data.chunks.insert(data.chunks.begin() + static_cast<std::ptrdiff_t>(ci) + 1,
                         std::move(tail));
    }
  }

  TpchQ11NationData data;
  std::vector<char> bucket_owned;
  std::unordered_set<const Q11SortedChunk *> owned_chunks;
};

// Copy-on-write working copy of the per-supplier directory.
class SupplierWork {
public:
  explicit SupplierWork(
      std::vector<std::shared_ptr<const Q11SupplierBucket>> base)
      : buckets(std::move(base)), owned(kQ11SupplierBuckets, 0) {
    if (buckets.size() != kQ11SupplierBuckets)
      buckets.resize(kQ11SupplierBuckets);
  }

  const Q11SupplierEntry *Find(int64_t suppkey) const {
    const auto &bucket = buckets[Q11SupplierBucketIndex(suppkey)];
    if (!bucket)
      return nullptr;
    const auto it = bucket->find(suppkey);
    return it == bucket->end() ? nullptr : &it->second;
  }

  // Mutable entry; creates it when absent.
  Q11SupplierEntry &Mut(int64_t suppkey) {
    return MutBucket(Q11SupplierBucketIndex(suppkey))[suppkey];
  }

  void Erase(int64_t suppkey) {
    if (!Find(suppkey))
      return;
    MutBucket(Q11SupplierBucketIndex(suppkey)).erase(suppkey);
  }

  std::vector<std::shared_ptr<const Q11SupplierBucket>> Take() {
    return std::move(buckets);
  }

private:
  Q11SupplierBucket &MutBucket(std::size_t bi) {
    auto &slot = buckets[bi];
    if (!owned[bi]) {
      slot = slot ? std::make_shared<Q11SupplierBucket>(*slot)
                  : std::make_shared<Q11SupplierBucket>();
      owned[bi] = 1;
    }
    return const_cast<Q11SupplierBucket &>(*slot);
  }

  std::vector<std::shared_ptr<const Q11SupplierBucket>> buckets;
  std::vector<char> owned;
};

// A change of a nation's per-part value, logged by the supplier bookkeeping.
struct NationOp {
  std::size_t delta; // arrival index of the delta that caused it
  int32_t nationkey;
  int64_t partkey;
  Q11AccT value;
};

// A supplier insert/delete/nation move only touches that supplier's parts:
// contributions are tracked per supplier.
template <class Log>
void ApplySupplierDelta(SupplierWork &suppliers, const TpchQ11SupplierDelta &delta,
                        Log &&log) {
  if (delta.has_old) {
    const auto *entry = suppliers.Find(delta.suppkey);
    if (entry && entry->has_supplier) {
      for (const auto &part : entry->parts)
        log(entry->nationkey, part.partkey, -static_cast<Q11AccT>(part.value));
      auto &mutable_entry = suppliers.Mut(delta.suppkey);
      mutable_entry.has_supplier = false;
      if (mutable_entry.parts.empty())
        suppliers.Erase(delta.suppkey);
    }
  }
  if (delta.has_new) {
    const auto *existing = suppliers.Find(delta.suppkey);
    if (existing && existing->has_supplier) {
      // Duplicate insert (should not happen): detach old contribution.
      for (const auto &part : existing->parts)
        log(existing->nationkey, part.partkey, -static_cast<Q11AccT>(part.value));
    }
    auto &entry = suppliers.Mut(delta.suppkey);
    entry.nationkey = delta.new_nationkey;
    entry.has_supplier = true;
    for (const auto &part : entry.parts)
      log(entry.nationkey, part.partkey, static_cast<Q11AccT>(part.value));
  }
}

// False when the delta deletes a row the supplier never had.
template <class Log>
bool ApplyPartsuppDelta(SupplierWork &suppliers, const TpchQ11PartsuppDelta &delta,
                        Log &&log) {
  const int64_t signed_raw =
      static_cast<int64_t>(delta.sign) * static_cast<int64_t>(delta.value);
  if (!suppliers.Find(delta.suppkey) && delta.sign < 0)
    return false;
  auto &entry = suppliers.Mut(delta.suppkey);
  auto pos = std::lower_bound(
      entry.parts.begin(), entry.parts.end(), delta.partkey,
      [](const Q11SupplierPart &part, int64_t key) { return part.partkey < key; });
  if (pos != entry.parts.end() && pos->partkey == delta.partkey) {
    pos->value += signed_raw;
    pos->count += delta.sign;
    if (pos->count < 0)
      return false;
    if (pos->count == 0)
      entry.parts.erase(pos);
  } else {
    if (delta.sign < 0)
      return false;
    entry.parts.insert(pos, Q11SupplierPart{delta.partkey, signed_raw, 1});
  }
  if (entry.has_supplier)
    log(entry.nationkey, delta.partkey, static_cast<Q11AccT>(delta.sign) * delta.value);
  if (entry.parts.empty() && !entry.has_supplier)
    suppliers.Erase(delta.suppkey);
  return true;
}

// Applies the supplier deltas, then the partsupp deltas, to `suppliers` and
// returns the nation ops they imply in arrival order. Supplier buckets are
// disjoint, so each bucket's deltas run as one task. False: rebuild.
bool BookkeepSuppliers(core::AuxEngineServices &services, const TpchQ11TxnBuffer &buffer,
                       SupplierWork &suppliers, std::vector<NationOp> &ops) {
  const auto &supplier_deltas = buffer.SupplierDeltas();
  const auto &partsupp_deltas = buffer.PartsuppDeltas();
  struct Bucket {
    std::vector<std::size_t> deltas; // arrival indexes; partsupp ones follow the supplier ones
    std::vector<NationOp> ops;
    bool consistent = true;
  };
  std::map<std::size_t, Bucket> buckets;
  for (std::size_t i = 0; i < supplier_deltas.size(); ++i)
    buckets[Q11SupplierBucketIndex(supplier_deltas[i].suppkey)].deltas.push_back(i);
  for (std::size_t i = 0; i < partsupp_deltas.size(); ++i)
    buckets[Q11SupplierBucketIndex(partsupp_deltas[i].suppkey)].deltas.push_back(
        supplier_deltas.size() + i);
  std::vector<Bucket *> touched;
  for (auto &entry : buckets)
    touched.push_back(&entry.second);

  const std::size_t delta_count = supplier_deltas.size() + partsupp_deltas.size();
  core::ParallelEachWhen(
      services, touched.size(), delta_count, kBookkeepOpThreshold,
      [&](std::size_t i) {
        auto &bucket = *touched[i];
        for (const auto delta : bucket.deltas) {
          const auto log = [&](int32_t nationkey, int64_t partkey, Q11AccT value) {
            bucket.ops.push_back({delta, nationkey, partkey, value});
          };
          if (delta < supplier_deltas.size()) {
            ApplySupplierDelta(suppliers, supplier_deltas[delta], log);
          } else if (!ApplyPartsuppDelta(suppliers, partsupp_deltas[delta - supplier_deltas.size()],
                                         log)) {
            bucket.consistent = false;
            return;
          }
        }
      });
  for (const auto *bucket : touched)
    if (!bucket->consistent)
      return false;
  for (const auto *bucket : touched)
    ops.insert(ops.end(), bucket->ops.begin(), bucket->ops.end());
  std::stable_sort(ops.begin(), ops.end(),
                   [](const NationOp &a, const NationOp &b) { return a.delta < b.delta; });
  return true;
}

// The low 64 bits of an accumulator: the raw value of the emitted decimal.
int64_t ToRaw64(Q11AccT value) {
#if defined(__SIZEOF_INT128__)
  return static_cast<int64_t>(static_cast<uint64_t>(value));
#else
  return value;
#endif
}

// A row's value passes when value / 100 > total / 100 * fraction, i.e. when
// value > total * fraction (the scale cancels).
Q11AccT ThresholdRaw(Q11AccT total, double fraction) {
  return static_cast<Q11AccT>(static_cast<double>(ToRaw64(total)) * fraction);
}

const core::OutputSchema &EmptySchema() {
  static const core::OutputSchema schema{};
  return schema;
}

// Thread-local two-cell row: (BIGINT partkey, DECIMAL value).
core::AuxRow &RowBuffer() {
  thread_local core::AuxRow row = [] {
    core::AuxRow r;
    r.values.resize(2);
    r.values[0].type = core::AuxType::Int64;
    r.values[0].is_null = false;
    r.values[0].payload = int64_t{0};
    r.values[1].type = core::AuxType::ScaledDecimal;
    r.values[1].is_null = false;
    r.values[1].payload = core::ScaledDecimal{0, kValueScale};
    return r;
  }();
  return row;
}

// Sorts a supplier's parts by partkey and folds duplicates (bag semantics).
void FoldParts(std::vector<Q11SupplierPart> &parts) {
  std::sort(parts.begin(), parts.end(),
            [](const Q11SupplierPart &a, const Q11SupplierPart &b) { return a.partkey < b.partkey; });
  std::size_t out = 0;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (out > 0 && parts[out - 1].partkey == parts[i].partkey) {
      parts[out - 1].value += parts[i].value;
      parts[out - 1].count += parts[i].count;
    } else {
      parts[out++] = parts[i];
    }
  }
  parts.resize(out);
  parts.shrink_to_fit();
}

using SupplierBuckets = std::vector<std::shared_ptr<Q11SupplierBucket>>;

struct NationPart {
  int64_t partkey = 0;
  int64_t value = 0;
};

// One value bucket of a nation's aggregate: its parts summed per partkey.
struct ValueBucketAgg {
  std::shared_ptr<Q11ValueBucket> values; // value > 0 only; null when empty
  std::vector<TpchQ11OutputRow> rows;     // the same, by RowBefore
  Q11AccT total = 0;                      // over all parts, whatever their sign
};

// Sums are exact integers, so the order of the parts does not matter.
ValueBucketAgg AggregateValueBucket(std::vector<NationPart> &&parts) {
  std::sort(parts.begin(), parts.end(),
            [](const NationPart &a, const NationPart &b) { return a.partkey < b.partkey; });
  ValueBucketAgg agg;
  for (std::size_t begin = 0, end = 0; begin < parts.size(); begin = end) {
    Q11AccT sum = 0;
    for (end = begin; end < parts.size() && parts[end].partkey == parts[begin].partkey; ++end)
      sum += static_cast<Q11AccT>(parts[end].value);
    agg.total += sum;
    if (sum <= 0)
      continue;
    if (!agg.values)
      agg.values = std::make_shared<Q11ValueBucket>();
    (*agg.values)[parts[begin].partkey] = sum;
    agg.rows.push_back({parts[begin].partkey, sum});
  }
  std::sort(agg.rows.begin(), agg.rows.end(), RowBefore);
  return agg;
}

struct NationRow {
  std::size_t nation = 0;
  TpchQ11OutputRow row;
};

// The aggregates of the nations that have suppliers, from their value buckets
// (aggs[nation][bucket]): the buckets' rows of all nations are ordered by one parallel
// sort and cut into chunks.
std::vector<std::shared_ptr<const TpchQ11NationData>>
MakeNationData(core::AuxEngineServices &services,
               std::vector<std::vector<ValueBucketAgg>> &aggs) {
  std::vector<std::shared_ptr<TpchQ11NationData>> data(aggs.size());
  std::vector<std::size_t> bucket_offsets(aggs.size() * kQ11ValueBuckets + 1, 0);
  std::vector<std::size_t> nation_starts(aggs.size() + 1, 0);
  for (std::size_t n = 0; n < aggs.size(); ++n) {
    data[n] = std::make_shared<TpchQ11NationData>();
    for (std::size_t b = 0; b < kQ11ValueBuckets; ++b) {
      auto &agg = aggs[n][b];
      data[n]->total += agg.total;
      data[n]->value_buckets.push_back(std::move(agg.values));
      bucket_offsets[n * kQ11ValueBuckets + b + 1] =
          bucket_offsets[n * kQ11ValueBuckets + b] + agg.rows.size();
    }
    nation_starts[n + 1] = bucket_offsets[(n + 1) * kQ11ValueBuckets];
  }
  std::vector<NationRow> sorted(bucket_offsets.back());
  core::ParallelEach(services, aggs.size() * kQ11ValueBuckets, kBucketGrain, [&](std::size_t i) {
    auto &rows = aggs[i / kQ11ValueBuckets][i % kQ11ValueBuckets].rows;
    auto out = sorted.begin() + static_cast<std::ptrdiff_t>(bucket_offsets[i]);
    for (const auto &row : rows)
      *out++ = {i / kQ11ValueBuckets, row};
    rows = {};
  });
  core::ParallelStableSort(
      services, sorted,
      [](const NationRow &a, const NationRow &b) {
        return a.nation != b.nation ? a.nation < b.nation : RowBefore(a.row, b.row);
      },
      kSortGrain);

  std::vector<std::size_t> chunk_starts(aggs.size() + 1, 0);
  for (std::size_t n = 0; n < aggs.size(); ++n) {
    const std::size_t rows = nation_starts[n + 1] - nation_starts[n];
    chunk_starts[n + 1] = chunk_starts[n] + (rows + kQ11ChunkTarget - 1) / kQ11ChunkTarget;
    data[n]->chunks.resize(chunk_starts[n + 1] - chunk_starts[n]);
  }
  core::ParallelEach(services, chunk_starts.back(), kChunkGrain, [&](std::size_t i) {
    const std::size_t n = std::upper_bound(chunk_starts.begin(), chunk_starts.end(), i) -
                          chunk_starts.begin() - 1;
    const std::size_t begin = nation_starts[n] + (i - chunk_starts[n]) * kQ11ChunkTarget;
    const std::size_t end = std::min(nation_starts[n + 1], begin + kQ11ChunkTarget);
    auto chunk = std::make_shared<Q11SortedChunk>();
    for (std::size_t r = begin; r < end; ++r)
      chunk->rows.push_back(sorted[r].row);
    data[n]->chunks[i - chunk_starts[n]] = std::move(chunk);
  });
  return {data.begin(), data.end()};
}

// Per nation: its suppliers' parts, dealt by value bucket, summed bucket by bucket and
// merged into the aggregate.
std::unordered_map<int32_t, std::shared_ptr<const TpchQ11NationData>>
AggregateNations(core::AuxEngineServices &services, const TpchQ11State &state,
                 const SupplierBuckets &buckets) {
  std::vector<int32_t> nations;
  for (const auto &entry : state.nation_name_to_key)
    nations.push_back(entry.second);
  std::sort(nations.begin(), nations.end());
  nations.erase(std::unique(nations.begin(), nations.end()), nations.end());
  const auto nation_index = [&](int32_t nationkey) {
    const auto it = std::lower_bound(nations.begin(), nations.end(), nationkey);
    return it != nations.end() && *it == nationkey
               ? static_cast<std::size_t>(it - nations.begin())
               : nations.size();
  };

  const std::size_t deal_buckets = nations.size() * kQ11ValueBuckets;
  const std::size_t slices = core::SliceCount(services, buckets.size(), kSupplierBucketGrain);
  std::vector<DealtRows<NationPart>> dealt(slices, DealtRows<NationPart>(deal_buckets));
  std::vector<std::vector<char>> has_suppliers(slices, std::vector<char>(nations.size(), 0));
  core::RunSlices(services, slices, buckets.size(),
                  [&](std::size_t slice, std::size_t begin, std::size_t end) {
                    for (std::size_t b = begin; b < end; ++b) {
                      if (!buckets[b])
                        continue;
                      for (const auto &[suppkey, entry] : *buckets[b]) {
                        const std::size_t nation = nation_index(entry.nationkey);
                        if (!entry.has_supplier || nation == nations.size())
                          continue;
                        has_suppliers[slice][nation] = 1;
                        for (const auto &part : entry.parts)
                          dealt[slice].Add(nation * kQ11ValueBuckets + Q11ValueBucketIndex(part.partkey),
                                           part.partkey, {part.partkey, part.value});
                      }
                    }
                  });
  for (std::size_t slice = 1; slice < slices; ++slice)
    dealt[0].Merge(std::move(dealt[slice]));

  std::vector<std::vector<ValueBucketAgg>> aggs(nations.size(),
                                                std::vector<ValueBucketAgg>(kQ11ValueBuckets));
  core::ParallelEach(services, deal_buckets, kBucketGrain, [&](std::size_t bucket) {
    aggs[bucket / kQ11ValueBuckets][bucket % kQ11ValueBuckets] =
        AggregateValueBucket(dealt[0].TakeBucket(bucket));
  });

  const auto data = MakeNationData(services, aggs);
  std::unordered_map<int32_t, std::shared_ptr<const TpchQ11NationData>> out;
  for (std::size_t n = 0; n < nations.size(); ++n) {
    bool present = false;
    for (const auto &flags : has_suppliers)
      present = present || flags[n];
    if (present)
      out[nations[n]] = data[n];
  }
  return out;
}

void AccumulateSupplier(const core::MutationBatch &mutation,
                        TpchQ11TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, bool is_old) {
    int64_t suppkey = 0;
    int32_t nationkey = 0;
    if (!ReadSupplier(row, suppkey, nationkey)) {
      buffer.MarkRebuild();
      return;
    }
    if (is_old) {
      buffer.AddSupplierDelta({suppkey, nationkey, 0, true, false});
    } else {
      buffer.AddSupplierDelta({suppkey, 0, nationkey, false, true});
    }
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows)
      add(row, false);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows)
      add(row, true);
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      int64_t old_sk = 0, new_sk = 0;
      int32_t old_nk = 0, new_nk = 0;
      if (!ReadSupplier(mutation.old_rows[i], old_sk, old_nk) ||
          !ReadSupplier(mutation.new_rows[i], new_sk, new_nk)) {
        buffer.MarkRebuild();
        return;
      }
      if (old_sk != new_sk) {
        buffer.MarkRebuild();
        return;
      }
      if (old_nk == new_nk)
        continue;
      buffer.AddSupplierDelta({old_sk, old_nk, new_nk, true, true});
    }
    break;
  }
}

void AccumulateNation(const core::MutationBatch &mutation,
                      TpchQ11TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, bool is_old) {
    int32_t nationkey = 0;
    std::string name;
    if (!ReadNation(row, nationkey, name)) {
      buffer.MarkRebuild();
      return;
    }
    if (is_old) {
      buffer.AddNationDelta({nationkey, name, ""});
    } else {
      buffer.AddNationDelta({nationkey, "", name});
    }
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows)
      add(row, false);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows)
      add(row, true);
    break;
  case core::MutationKind::Update:
    if (mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      int32_t old_nk = 0, new_nk = 0;
      std::string old_name, new_name;
      if (!ReadNation(mutation.old_rows[i], old_nk, old_name) ||
          !ReadNation(mutation.new_rows[i], new_nk, new_name)) {
        buffer.MarkRebuild();
        return;
      }
      if (old_nk != new_nk) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddNationDelta({old_nk, old_name, new_name});
    }
    break;
  }
}

void AccumulatePartsupp(const core::MutationBatch &mutation,
                        TpchQ11TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ11PartsuppInput input;
    if (!ReadPartsupp(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    TpchQ11PartsuppDelta delta;
    delta.partkey = input.partkey;
    delta.suppkey = input.suppkey;
    Q11AccT product = 0;
    AddAcc(product, input.supplycost_raw, input.availqty);
    delta.value = product;
    delta.sign = sign;
    buffer.AddPartsuppDelta(delta);
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

// One nation's published aggregate and the supplier ops that change it.
struct NationSlot {
  std::string name;
  core::Version version = 0;
  std::shared_ptr<const TpchQ11NationData> data;
  std::vector<std::pair<int64_t, Q11AccT>> ops; // (partkey, signed value)
  std::optional<NationWork> work;
};
using NationSlots = std::unordered_map<int32_t, NationSlot>;

std::shared_ptr<const TpchQ11State>
LoadMetadata(core::AuxEngineServices &services, core::Version &version) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ11Layout::kMetadata});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ11Layout::kMetadata)
      continue;
    const auto *typed = AsQ11Image(image, TpchQ11ImageKind::Metadata);
    if (typed) {
      version = image.version;
      return typed->state;
    }
    break;
  }
  return nullptr;
}

// Loads every nation partition (cheap shared_ptr reads); copy-on-write work
// objects are created lazily for the nations a delta touches. False: the
// partitions do not match the metadata, so the state rebuilds.
bool LoadNations(core::AuxEngineServices &services, const TpchQ11State &metadata,
                 NationSlots &nations) {
  std::vector<core::PartitionId> partitions;
  partitions.reserve(metadata.nation_name_to_key.size());
  for (const auto &entry : metadata.nation_name_to_key)
    partitions.push_back(NationNamePartition(entry.first));
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ11Image(image, TpchQ11ImageKind::NationPreAggregated);
    if (!typed || typed->nation_name.empty() || !typed->data)
      return false;
    const auto name_it = metadata.nation_name_to_key.find(typed->nation_name);
    if (name_it == metadata.nation_name_to_key.end() ||
        name_it->second != typed->nationkey)
      return false;
    auto &slot = nations[typed->nationkey];
    slot.name = typed->nation_name;
    slot.version = image.version;
    slot.data = typed->data;
  }
  return nations.size() == metadata.nation_name_to_key.size();
}

// Nations are independent: each applies its own ops in arrival order. False:
// an op was inconsistent with the aggregate.
bool ApplyNationOps(core::AuxEngineServices &services, NationSlots &nations) {
  std::vector<NationSlot *> touched;
  std::size_t op_count = 0;
  for (auto &entry : nations)
    if (!entry.second.ops.empty()) {
      touched.push_back(&entry.second);
      op_count += entry.second.ops.size();
    }
  std::vector<uint8_t> inconsistent(touched.size(), 0);
  core::ParallelEachWhen(
      services, touched.size(), op_count, kNationOpThreshold, [&](std::size_t i) {
        auto &slot = *touched[i];
        slot.work.emplace(slot.data);
        for (const auto &[partkey, value] : slot.ops)
          if (!slot.work->Apply(partkey, value))
            inconsistent[i] = 1;
      });
  return std::none_of(inconsistent.begin(), inconsistent.end(),
                      [](uint8_t flag) { return flag != 0; });
}

// Rows with value above `threshold_raw`, in order, through one reused row.
// Sorted value DESC: per chunk, one comparison against its last row decides
// "whole chunk passes"; otherwise a binary search finds the cut and the walk
// ends.
void EmitRowsAbove(const TpchQ11NationData &data, Q11AccT threshold_raw,
                   core::AuxRowSink &sink) {
  auto &aux_row = RowBuffer();
  for (const auto &chunk_ptr : data.chunks) {
    const auto &rows = chunk_ptr->rows;
    if (rows.empty())
      continue;
    const TpchQ11OutputRow *first = rows.data();
    const TpchQ11OutputRow *last = first + rows.size();
    bool done = false;
    if (!(last[-1].value > threshold_raw)) {
      last = std::partition_point(
          first, last,
          [&](const TpchQ11OutputRow &r) { return r.value > threshold_raw; });
      done = true;
    }
    for (const TpchQ11OutputRow *r = first; r != last; ++r) {
      aux_row.values[0].payload.emplace<int64_t>(r->partkey);
      std::get<core::ScaledDecimal>(aux_row.values[1].payload).value = ToRaw64(r->value);
      sink.WriteRow(aux_row);
    }
    if (done)
      break;
  }
}

} // namespace

core::PartitionId NationNamePartition(const std::string &name) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : name) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return TpchQ11Layout::kNationNameBase +
         static_cast<core::PartitionId>(hash % TpchQ11Layout::kNationNameSlots);
}

bool TpchQ11TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ11TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  partsupp_deltas.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
bool TpchQ11TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ11PartsuppDelta> &TpchQ11TxnBuffer::PartsuppDeltas() const {
  return partsupp_deltas;
}
const std::vector<TpchQ11SupplierDelta> &TpchQ11TxnBuffer::SupplierDeltas() const {
  return supplier_deltas;
}
const std::vector<TpchQ11NationDelta> &TpchQ11TxnBuffer::NationDeltas() const {
  return nation_deltas;
}
void TpchQ11TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  partsupp_deltas.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
void TpchQ11TxnBuffer::AddPartsuppDelta(TpchQ11PartsuppDelta delta) {
  pending = true;
  partsupp_deltas.push_back(delta);
}
void TpchQ11TxnBuffer::AddSupplierDelta(TpchQ11SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(delta);
}
void TpchQ11TxnBuffer::AddNationDelta(TpchQ11NationDelta delta) {
  pending = true;
  nation_deltas.push_back(delta);
}

core::AcceleratorId TpchQ11Module::Id() const { return TpchQ11Layout::kAccelerator; }

std::vector<core::TableDependency> TpchQ11Module::Dependencies() const {
  return {{config.partsupp_table, {0, 1, 2, 3}, {0, 1, 2, 3}},
          {config.supplier_table, {0, 1}, {0, 3}},
          {config.nation_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ11Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ11TxnBuffer>();
}

void TpchQ11Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto built = BuildState(services);

  builder.Write(core::StateImage::Make(
      TpchQ11Layout::kAccelerator, TpchQ11Layout::kMetadata, 0,
      MakeMetadataImage(built.state)));

  // One FNV-1a nation-name partition per nation (empty nations included, so
  // deltas never find a missing partition).
  const auto empty = EmptyNationData();
  for (const auto &[name, nk] : built.state->nation_name_to_key) {
    if (name.empty())
      continue;
    const auto data_it = built.nations.find(nk);
    const auto image = MakeNationImage(
        nk, name, data_it != built.nations.end() ? data_it->second : empty);
    builder.Write(core::StateImage::Make(
        TpchQ11Layout::kAccelerator, NationNamePartition(name), 0, image));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ11Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.partsupp_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ11Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ11TxnBuffer &>(base_buffer);
  if (mutation.table == config.supplier_table)
    AccumulateSupplier(mutation, buffer);
  else if (mutation.table == config.nation_table)
    AccumulateNation(mutation, buffer);
  else if (mutation.table == config.partsupp_table)
    AccumulatePartsupp(mutation, buffer);
}

core::StagedMaintenanceOutcome
TpchQ11Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ11TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (!buffer.RequiresRebuild() && buffer.PartsuppDeltas().empty() &&
      buffer.SupplierDeltas().empty() && buffer.NationDeltas().empty())
    return outcome;

  // Nation table changes are rare (static dimension): rebuild.
  if (buffer.RequiresRebuild() || !buffer.NationDeltas().empty()) {
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }

  core::Version metadata_version = 0;
  const auto metadata = LoadMetadata(services, metadata_version);
  NationSlots nations;
  if (!metadata || !LoadNations(services, *metadata, nations)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  // The nations' own work is only logged here and applied nation by nation
  // afterwards.
  SupplierWork suppliers(metadata->supplier_buckets);
  std::vector<NationOp> nation_ops;
  if (!BookkeepSuppliers(services, buffer, suppliers, nation_ops)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  for (const auto &op : nation_ops) {
    const auto it = nations.find(op.nationkey);
    if (it != nations.end()) // a nation without a nation row contributes nowhere
      it->second.ops.emplace_back(op.partkey, op.value);
  }
  if (!ApplyNationOps(services, nations)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  auto new_state = std::make_shared<TpchQ11State>();
  new_state->nation_name_to_key = metadata->nation_name_to_key;
  new_state->supplier_buckets = suppliers.Take();

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ11Layout::kAccelerator, TpchQ11Layout::kMetadata, metadata_version,
      MakeMetadataImage(std::move(new_state))));
  for (auto &[nationkey, slot] : nations) {
    if (!slot.work)
      continue;
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ11Layout::kAccelerator, NationNamePartition(slot.name),
        slot.version,
        MakeNationImage(nationkey, slot.name, slot.work->Finish())));
  }
  return outcome;
}

bool TpchQ11Module::CheckValidity(
    core::AuxEngineServices &services, const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ11Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (arguments.values.size() < 2 ||
      arguments.values[0].is_null ||
      arguments.values[1].is_null)
    return;

  std::string nation_name;
  double fraction = 0.0;

  if (std::holds_alternative<std::string>(arguments.values[0].payload)) {
    nation_name = std::get<std::string>(arguments.values[0].payload);
  } else {
    return;
  }

  if (std::holds_alternative<double>(arguments.values[1].payload)) {
    fraction = std::get<double>(arguments.values[1].payload);
  } else if (std::holds_alternative<int64_t>(arguments.values[1].payload)) {
    fraction = static_cast<double>(
        std::get<int64_t>(arguments.values[1].payload));
  } else {
    return;
  }

  if (!std::isfinite(fraction))
    return;

  const auto visibility = services.CurrentVisibility();

  // Fast path: the FNV-1a nation-name partition.
  thread_local std::vector<core::PartitionId> wanted(1);
  wanted[0] = NationNamePartition(nation_name);
  const auto snapshot = services.StateStore().ReadPartitions(visibility, wanted);
  if (snapshot.partitions.empty() &&
      services.StateStore().CheckValidity({Id(), {}}, visibility).IsValid()) {
    // The state has a partition for every nation: none for this name
    // means no such nation (e.g. renamed), so no rows.
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ11Image(image, TpchQ11ImageKind::NationPreAggregated);
    if (!typed)
      continue;
    // Collision guard: verify stored nation_name matches query
    if (typed->nation_name != nation_name || !typed->data)
      continue;
    sink.Begin(EmptySchema());
    EmitRowsAbove(*typed->data, ThresholdRaw(typed->data->total, fraction), sink);
    sink.End();
    return;
  }

  // On any miss (missing partition, collision, invalid image):
  // fall back to full build from base tables.
  ExecuteFallback(services, arguments, sink);
}

TpchQ11Built TpchQ11Module::BuildState(
    core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q11 requires a typed initial scan source");

  auto state = std::make_shared<TpchQ11State>();
  initial_scan->ScanNationsSerial([&](const TpchQ11NationInput &input) {
    state->nation_name_to_key[input.name] = input.nationkey;
  });

  using scan::DecimalColumn;
  using scan::IntColumn;
  auto suppliers = DealTable<SupplierRow>(
      services, *initial_scan, "supplier", {IntColumn("s_suppkey"), IntColumn("s_nationkey")},
      kQ11SupplierBuckets, [](DealtRows<SupplierRow> &rows, const auto &row) {
        const int64_t suppkey = row.Int(0);
        rows.Add(Q11SupplierBucketIndex(suppkey), suppkey,
                 {suppkey, static_cast<int32_t>(row.Int(1))});
      });
  auto partsupps = DealTable<PartsuppRow>(
      services, *initial_scan, "partsupp",
      {IntColumn("ps_partkey"), IntColumn("ps_suppkey"), IntColumn("ps_availqty"),
       DecimalColumn("ps_supplycost", 2)},
      kQ11SupplierBuckets, [](DealtRows<PartsuppRow> &rows, const auto &row) {
        Q11AccT product = 0;
        AddAcc(product, row.Decimal(3), row.Int(2));
        const int64_t suppkey = row.Int(1);
        rows.Add(Q11SupplierBucketIndex(suppkey), suppkey,
                 {suppkey, {row.Int(0), static_cast<int64_t>(product), 1}});
      });

  // A supplier bucket owns its suppliers and their partsupp rows; the rows of a
  // supplier fold into one sorted entry (their order is folded away by the sort).
  SupplierBuckets buckets(kQ11SupplierBuckets);
  core::ParallelEach(services, kQ11SupplierBuckets, 1, [&](std::size_t b) {
    auto bucket = std::make_shared<Q11SupplierBucket>();
    suppliers.ForEach(b, [&](const SupplierRow &row) {
      auto &entry = (*bucket)[row.suppkey];
      entry.nationkey = row.nationkey;
      entry.has_supplier = true;
    });
    partsupps.ForEach(b, [&](const PartsuppRow &row) {
      (*bucket)[row.suppkey].parts.push_back(row.part);
    });
    for (auto &[suppkey, entry] : *bucket)
      FoldParts(entry.parts);
    if (!bucket->empty())
      buckets[b] = std::move(bucket);
  });

  TpchQ11Built built;
  built.nations = AggregateNations(services, *state, buckets);
  state->supplier_buckets.assign(buckets.begin(), buckets.end());

  built.state = std::move(state);
  return built;
}

std::vector<core::CalibrationWrite> TpchQ11Module::CalibrationWrites() const {
  return TpchQ11CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
