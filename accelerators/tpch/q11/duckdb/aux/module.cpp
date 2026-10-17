#include "tpch/q11/duckdb/aux/module.hpp"
#include "tpch/q11/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

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

// Suppliers per build slice: sorting ~80 parts is ~10 us, so hundreds pay for a task.
constexpr std::size_t kSupplierFoldGrain = 256;
// Nation aggregates per slice when building (~ms each).
constexpr std::size_t kNationJobGrain = 1;
// Nation ops below which a commit applies its nations inline.
constexpr std::size_t kNationOpThreshold = 2048;
// Deltas in a commit below which the supplier bookkeeping stays on one thread.
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

bool RowBefore(const TpchQ11OutputRow &lhs, const TpchQ11OutputRow &rhs) {
  if (lhs.value != rhs.value)
    return lhs.value > rhs.value;
  return lhs.partkey < rhs.partkey;
}

// Builds an immutable nation aggregate from rows already sorted by RowBefore
// (unique partkeys, value > 0).
std::shared_ptr<const TpchQ11NationData>
MakeNationData(const std::vector<TpchQ11OutputRow> &sorted, Q11AccT total) {
  auto data = std::make_shared<TpchQ11NationData>();
  data->total = total;
  std::vector<std::shared_ptr<Q11ValueBucket>> tmp(kQ11ValueBuckets);
  for (const auto &row : sorted) {
    auto &bucket = tmp[Q11ValueBucketIndex(row.partkey)];
    if (!bucket)
      bucket = std::make_shared<Q11ValueBucket>();
    (*bucket)[row.partkey] = row.value;
  }
  data->value_buckets.assign(tmp.begin(), tmp.end());
  for (std::size_t i = 0; i < sorted.size(); i += kQ11ChunkTarget) {
    const std::size_t end = std::min(sorted.size(), i + kQ11ChunkTarget);
    auto chunk = std::make_shared<Q11SortedChunk>();
    chunk->rows.assign(sorted.begin() + static_cast<std::ptrdiff_t>(i),
                       sorted.begin() + static_cast<std::ptrdiff_t>(end));
    data->chunks.push_back(std::move(chunk));
  }
  return data;
}

std::shared_ptr<const TpchQ11NationData> EmptyNationData() {
  return MakeNationData({}, 0);
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

// Per supplier: sorts the parts by partkey and folds duplicates (bag semantics).
void FoldSupplierParts(core::AuxEngineServices &services,
                       std::unordered_map<int64_t, Q11SupplierEntry> &suppliers) {
  std::vector<Q11SupplierEntry *> entries;
  entries.reserve(suppliers.size());
  for (auto &[suppkey, entry] : suppliers)
    entries.push_back(&entry);
  core::ParallelSlices(
      services, entries.size(), kSupplierFoldGrain,
      [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t e = begin; e < end; ++e) {
          auto &parts = entries[e]->parts;
          std::sort(parts.begin(), parts.end(),
                    [](const Q11SupplierPart &a, const Q11SupplierPart &b) {
                      return a.partkey < b.partkey;
                    });
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
      });
}

// Per nation: merges its suppliers' parts and sorts them into the aggregate.
// Sums are exact integers, so supplier order does not matter.
std::unordered_map<int32_t, std::shared_ptr<const TpchQ11NationData>>
AggregateNations(core::AuxEngineServices &services, const TpchQ11State &state,
                 const std::unordered_map<int64_t, Q11SupplierEntry> &suppliers) {
  std::unordered_set<int32_t> known_nations;
  for (const auto &entry : state.nation_name_to_key)
    known_nations.insert(entry.second);
  std::unordered_map<int32_t, std::vector<const Q11SupplierEntry *>> by_nation;
  for (const auto &[suppkey, entry] : suppliers) {
    if (entry.has_supplier && known_nations.count(entry.nationkey))
      by_nation[entry.nationkey].push_back(&entry);
  }
  const std::vector<std::pair<int32_t, std::vector<const Q11SupplierEntry *>>>
      jobs(by_nation.begin(), by_nation.end());
  std::vector<std::shared_ptr<const TpchQ11NationData>> nation_data(jobs.size());
  core::ParallelEach(services, jobs.size(), kNationJobGrain, [&](std::size_t j) {
    std::unordered_map<int64_t, Q11AccT> merged;
    Q11AccT total = 0;
    for (const auto *entry : jobs[j].second) {
      for (const auto &part : entry->parts) {
        merged[part.partkey] += static_cast<Q11AccT>(part.value);
        total += static_cast<Q11AccT>(part.value);
      }
    }
    std::vector<TpchQ11OutputRow> sorted;
    sorted.reserve(merged.size());
    for (const auto &[pk, value] : merged)
      if (value > 0)
        sorted.push_back({pk, value});
    std::sort(sorted.begin(), sorted.end(), RowBefore);
    nation_data[j] = MakeNationData(sorted, total);
  });
  std::unordered_map<int32_t, std::shared_ptr<const TpchQ11NationData>> out;
  for (std::size_t j = 0; j < jobs.size(); ++j)
    out[jobs[j].first] = std::move(nation_data[j]);
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

// ── Partition helpers ───────────────────────────────────────────────────────
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
  initial_scan->ScanNations([&](const TpchQ11NationInput &input) {
    state->nation_name_to_key[input.name] = input.nationkey;
  });

  std::unordered_map<int64_t, Q11SupplierEntry> all;
  initial_scan->ScanSuppliers([&](const TpchQ11SupplierInput &input) {
    auto &entry = all[input.suppkey];
    entry.nationkey = input.nationkey;
    entry.has_supplier = true;
  });

  // Partsupp rows are grouped by supplier per scan partition; the groups are
  // joined supplier by supplier (their order within a supplier is folded away
  // by the sort below).
  using scan::DecimalColumn;
  using scan::IntColumn;
  auto by_supplier = core::PartitionedBuild<std::unordered_map<int64_t, std::vector<Q11SupplierPart>>>(
      services, [] { return std::unordered_map<int64_t, std::vector<Q11SupplierPart>>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "partsupp",
                                {IntColumn("ps_partkey"), IntColumn("ps_suppkey"),
                                 IntColumn("ps_availqty"), DecimalColumn("ps_supplycost", 2)},
                                parts, sink);
      },
      [](auto &groups, const auto &row) {
        Q11AccT product = 0;
        AddAcc(product, row.Decimal(3), row.Int(2));
        groups[row.Int(1)].push_back({row.Int(0), static_cast<int64_t>(product), 1});
      },
      [](auto &into, auto &&from) {
        for (auto &[suppkey, parts] : from) {
          auto &target = into[suppkey];
          target.insert(target.end(), parts.begin(), parts.end());
        }
      });
  for (auto &[suppkey, parts] : by_supplier)
    all[suppkey].parts = std::move(parts);

  FoldSupplierParts(services, all);

  TpchQ11Built built;
  built.nations = AggregateNations(services, *state, all);

  // Distribute suppliers into COW buckets (moving, no copies).
  std::vector<std::shared_ptr<Q11SupplierBucket>> tmp(kQ11SupplierBuckets);
  for (auto &[suppkey, entry] : all) {
    auto &bucket = tmp[Q11SupplierBucketIndex(suppkey)];
    if (!bucket)
      bucket = std::make_shared<Q11SupplierBucket>();
    bucket->emplace(suppkey, std::move(entry));
  }
  all.clear();
  state->supplier_buckets.assign(tmp.begin(), tmp.end());
  tmp.clear();

  built.state = std::move(state);
  return built;
}

std::vector<core::CalibrationWrite> TpchQ11Module::CalibrationWrites() const {
  return TpchQ11CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
