#include "tpch/q11/duckdb/aux/module.hpp"
#include "tpch/q11/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 2;
constexpr uint32_t kMetadataMagic = 0x4d313151; // Q11M
constexpr uint32_t kNationMagic = 0x4e313151;   // Q11N

// ── Cell helpers ────────────────────────────────────────────────────────────
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

// ── Hugeint / __int128 helpers ──────────────────────────────────────────────
#if defined(__SIZEOF_INT128__)
void WriteQ11Acc(ByteWriter &writer, Q11AccT value) {
  writer.U64(static_cast<uint64_t>(value));
  writer.U64(static_cast<uint64_t>(value >> 64));
}

bool ReadQ11Acc(ByteReader &reader, Q11AccT &value) {
  uint64_t lo = 0;
  uint64_t hi = 0;
  if (!reader.U64(lo) || !reader.U64(hi))
    return false;
  value = static_cast<Q11AccT>(hi) << 64;
  value |= static_cast<Q11AccT>(lo);
  return true;
}

void AddAcc(Q11AccT &accumulator, int64_t supplycost_raw, int64_t availqty) {
  accumulator += static_cast<Q11AccT>(supplycost_raw) *
                 static_cast<Q11AccT>(availqty);
}
#else
void WriteQ11Acc(ByteWriter &writer, Q11AccT value) {
  writer.I64(value);
}

bool ReadQ11Acc(ByteReader &reader, Q11AccT &value) {
  return reader.I64(value);
}

void AddAcc(Q11AccT &accumulator, int64_t supplycost_raw, int64_t availqty) {
  accumulator += supplycost_raw * availqty;
}
#endif

// ── StateImage helpers ──────────────────────────────────────────────────────
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

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// ── Row readers ─────────────────────────────────────────────────────────────
bool ReadPartsupp(const core::AuxRow &row, TpchQ11PartsuppInput &out) {
  int64_t partkey = 0, suppkey = 0, availqty = 0;
  int64_t supplycost_raw = 0;
  int32_t supplycost_scale = 0;
  if (!IntCell(row, 0, partkey) || !IntCell(row, 1, suppkey) ||
      !IntCell(row, 2, availqty) ||
      !RawDecimalCell(row, 3, supplycost_raw, supplycost_scale))
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
  int64_t nk = 0;
  if (!IntCell(row, 0, suppkey) || !IntCell(row, 1, nk))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey = static_cast<int32_t>(nk);
  return true;
}

bool ReadNation(const core::AuxRow &row, int32_t &nationkey,
                std::string &name) {
  int64_t nk = 0;
  if (!IntCell(row, 0, nk) || !StringCell(row, 1, name))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey = static_cast<int32_t>(nk);
  return true;
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

void FlattenNationRows(const TpchQ11NationData &data,
                       std::vector<TpchQ11OutputRow> &out) {
  for (const auto &chunk : data.chunks)
    out.insert(out.end(), chunk->rows.begin(), chunk->rows.end());
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

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ11Metadata(const TpchQ11State &state) {
  ByteWriter writer;

  uint32_t nation_count = static_cast<uint32_t>(state.nation_name_to_key.size());
  uint32_t supplier_count = 0;
  for (const auto &bucket : state.supplier_buckets)
    if (bucket)
      supplier_count += static_cast<uint32_t>(bucket->size());

  core::WriteImageHeader(writer, kMetadataMagic, kVersion,
                         nation_count + supplier_count);

  std::vector<std::string> nation_names;
  for (const auto &entry : state.nation_name_to_key)
    nation_names.push_back(entry.first);
  std::sort(nation_names.begin(), nation_names.end());

  writer.U32(nation_count);
  for (const auto &name : nation_names) {
    writer.String(name);
    writer.I32(state.nation_name_to_key.at(name));
  }

  std::vector<int64_t> supplier_keys;
  supplier_keys.reserve(supplier_count);
  for (const auto &bucket : state.supplier_buckets)
    if (bucket)
      for (const auto &entry : *bucket)
        supplier_keys.push_back(entry.first);
  std::sort(supplier_keys.begin(), supplier_keys.end());

  writer.U32(supplier_count);
  for (const auto sk : supplier_keys) {
    const auto &entry =
        state.supplier_buckets[Q11SupplierBucketIndex(sk)]->at(sk);
    writer.I64(sk);
    writer.I32(entry.nationkey);
    writer.U32(entry.has_supplier ? 1 : 0);
    writer.U32(static_cast<uint32_t>(entry.parts.size()));
    for (const auto &part : entry.parts) {
      writer.I64(part.partkey);
      writer.I64(part.value);
      writer.I32(part.count);
    }
  }

  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ11NationPartition(
    int32_t nationkey, const std::string &nation_name,
    const TpchQ11NationData &data) {
  ByteWriter writer;
  uint32_t count = 0;
  for (const auto &chunk : data.chunks)
    count += static_cast<uint32_t>(chunk->rows.size());
  core::WriteImageHeader(writer, kNationMagic, kVersion, count);

  writer.String(nation_name);
  WriteQ11Acc(writer, data.total);

  for (const auto &chunk : data.chunks) {
    for (const auto &row : chunk->rows) {
      writer.I64(row.partkey);
      WriteQ11Acc(writer, row.value);
    }
  }

  (void)nationkey;
  return std::move(writer.bytes);
}

bool DecodeTpchQ11Metadata(const std::vector<uint8_t> &bytes,
                           TpchQ11State &state) {
  ByteReader reader{bytes};
  uint32_t total = 0;
  if (!core::ReadImageHeader(reader, kMetadataMagic, kVersion, total))
    return false;

  state.nation_name_to_key.clear();
  state.supplier_buckets.clear();

  uint32_t nation_count = 0;
  if (!reader.U32(nation_count))
    return false;
  for (uint32_t i = 0; i < nation_count; ++i) {
    std::string name;
    int32_t nk = 0;
    if (!reader.String(name) || !reader.I32(nk))
      return false;
    state.nation_name_to_key.emplace(std::move(name), nk);
  }

  std::vector<std::shared_ptr<Q11SupplierBucket>> tmp(kQ11SupplierBuckets);
  uint32_t supplier_count = 0;
  if (!reader.U32(supplier_count))
    return false;
  for (uint32_t i = 0; i < supplier_count; ++i) {
    int64_t sk = 0;
    Q11SupplierEntry entry;
    uint32_t has_supplier = 0, part_count = 0;
    if (!reader.I64(sk) || !reader.I32(entry.nationkey) ||
        !reader.U32(has_supplier) || !reader.U32(part_count))
      return false;
    entry.has_supplier = has_supplier != 0;
    entry.parts.reserve(part_count);
    for (uint32_t j = 0; j < part_count; ++j) {
      Q11SupplierPart part;
      if (!reader.I64(part.partkey) || !reader.I64(part.value) ||
          !reader.I32(part.count))
        return false;
      entry.parts.push_back(part);
    }
    auto &bucket = tmp[Q11SupplierBucketIndex(sk)];
    if (!bucket)
      bucket = std::make_shared<Q11SupplierBucket>();
    bucket->emplace(sk, std::move(entry));
  }
  state.supplier_buckets.assign(tmp.begin(), tmp.end());

  return reader.offset == bytes.size();
}

bool DecodeTpchQ11NationPartition(
    const std::vector<uint8_t> &bytes,
    int32_t &nationkey, std::string &nation_name,
    std::shared_ptr<const TpchQ11NationData> &data) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kNationMagic, kVersion, count))
    return false;

  Q11AccT total_value = 0;
  if (!reader.String(nation_name))
    return false;
  if (!ReadQ11Acc(reader, total_value))
    return false;

  std::vector<TpchQ11OutputRow> sorted_rows;
  sorted_rows.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ11OutputRow row;
    if (!reader.I64(row.partkey) || !ReadQ11Acc(reader, row.value))
      return false;
    sorted_rows.push_back(row);
  }
  std::sort(sorted_rows.begin(), sorted_rows.end(), RowBefore);
  data = MakeNationData(sorted_rows, total_value);

  (void)nationkey;
  return reader.offset == bytes.size();
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
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

// ── Module ──────────────────────────────────────────────────────────────────
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

  if (mutation.table == config.supplier_table) {
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
    return;
  }

  if (mutation.table == config.nation_table) {
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
    return;
  }

  if (mutation.table != config.partsupp_table)
    return;

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

core::StagedMaintenanceOutcome
TpchQ11Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ11TxnBuffer &>(base_buffer);
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

  if (buffer.PartsuppDeltas().empty() && buffer.SupplierDeltas().empty() &&
      buffer.NationDeltas().empty()) {
    return outcome;
  }

  // Nation table changes are rare (static dimension): rebuild.
  if (!buffer.NationDeltas().empty()) {
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

  auto rebuild = [&]() {
    core::FinishRebuildAll(outcome);
    return outcome;
  };

  const auto visibility = services.CurrentVisibility();
  const auto metadata_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ11Layout::kMetadata});
  std::shared_ptr<const TpchQ11State> metadata;
  core::Version metadata_version = 0;
  for (const auto &image : metadata_snapshot.partitions) {
    if (image.partition != TpchQ11Layout::kMetadata)
      continue;
    const auto *typed = AsQ11Image(image, TpchQ11ImageKind::Metadata);
    if (typed) {
      metadata = typed->state;
      metadata_version = image.version;
    }
    break;
  }
  if (!metadata)
    return rebuild();

  // Load every nation partition (cheap shared_ptr reads); copy-on-write work
  // objects are created lazily for the nations a delta touches.
  struct NationSlot {
    std::string name;
    core::Version version = 0;
    std::shared_ptr<const TpchQ11NationData> data;
    std::optional<NationWork> work;
  };
  std::unordered_map<int32_t, NationSlot> nations;
  {
    std::vector<core::PartitionId> partitions;
    partitions.reserve(metadata->nation_name_to_key.size());
    for (const auto &entry : metadata->nation_name_to_key)
      partitions.push_back(NationNamePartition(entry.first));
    const auto nation_snapshot =
        services.StateStore().ReadPartitions(visibility, partitions);
    for (const auto &image : nation_snapshot.partitions) {
      const auto *typed =
          AsQ11Image(image, TpchQ11ImageKind::NationPreAggregated);
      if (!typed || typed->nation_name.empty() || !typed->data)
        return rebuild();
      const auto name_it = metadata->nation_name_to_key.find(typed->nation_name);
      if (name_it == metadata->nation_name_to_key.end() ||
          name_it->second != typed->nationkey)
        return rebuild();
      auto &slot = nations[typed->nationkey];
      slot.name = typed->nation_name;
      slot.version = image.version;
      slot.data = typed->data;
    }
    if (nations.size() != metadata->nation_name_to_key.size())
      return rebuild();
  }

  bool ok = true;
  auto nation_apply = [&](int32_t nationkey, int64_t partkey,
                          Q11AccT signed_value) {
    const auto it = nations.find(nationkey);
    if (it == nations.end())
      return; // supplier's nation has no nation row: contributes nowhere
    auto &slot = it->second;
    if (!slot.work)
      slot.work.emplace(slot.data);
    if (!slot.work->Apply(partkey, signed_value))
      ok = false;
  };

  SupplierWork suppliers(metadata->supplier_buckets);

  // Supplier deltas first: contributions are tracked per supplier, so a
  // supplier insert/delete/nation move only touches that supplier's parts.
  for (const auto &delta : buffer.SupplierDeltas()) {
    if (delta.has_old) {
      const auto *entry = suppliers.Find(delta.suppkey);
      if (entry && entry->has_supplier) {
        for (const auto &part : entry->parts)
          nation_apply(entry->nationkey, part.partkey,
                       -static_cast<Q11AccT>(part.value));
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
          nation_apply(existing->nationkey, part.partkey,
                       -static_cast<Q11AccT>(part.value));
      }
      auto &entry = suppliers.Mut(delta.suppkey);
      entry.nationkey = delta.new_nationkey;
      entry.has_supplier = true;
      for (const auto &part : entry.parts)
        nation_apply(entry.nationkey, part.partkey,
                     static_cast<Q11AccT>(part.value));
    }
    if (!ok)
      return rebuild();
  }

  for (const auto &delta : buffer.PartsuppDeltas()) {
    const int64_t signed_raw =
        static_cast<int64_t>(delta.sign) * static_cast<int64_t>(delta.value);
    const auto *existing = suppliers.Find(delta.suppkey);
    if (!existing && delta.sign < 0)
      return rebuild();
    auto &entry = suppliers.Mut(delta.suppkey);
    auto pos = std::lower_bound(
        entry.parts.begin(), entry.parts.end(), delta.partkey,
        [](const Q11SupplierPart &part, int64_t key) {
          return part.partkey < key;
        });
    if (pos != entry.parts.end() && pos->partkey == delta.partkey) {
      pos->value += signed_raw;
      pos->count += delta.sign;
      if (pos->count < 0)
        return rebuild();
      if (pos->count == 0)
        entry.parts.erase(pos);
    } else {
      if (delta.sign < 0)
        return rebuild();
      entry.parts.insert(pos, Q11SupplierPart{delta.partkey, signed_raw, 1});
    }
    if (entry.has_supplier)
      nation_apply(entry.nationkey, delta.partkey,
                   static_cast<Q11AccT>(delta.sign) * delta.value);
    if (entry.parts.empty() && !entry.has_supplier)
      suppliers.Erase(delta.suppkey);
    if (!ok)
      return rebuild();
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

  // ── Fast path: FNV-1a nation-name hash ─────────────────────────────────
  {
    const auto fast_snapshot = services.StateStore().ReadPartitions(
        visibility, {NationNamePartition(nation_name)});
    if (fast_snapshot.partitions.empty() &&
        services.StateStore().CheckValidity({Id(), {}}, visibility).IsValid()) {
      // The state has a partition for every nation: none for this name
      // means no such nation (e.g. renamed), so no rows.
      sink.Begin(core::OutputSchema{});
      sink.End();
      return;
    }
    for (const auto &image : fast_snapshot.partitions) {
      const auto *typed = AsQ11Image(image, TpchQ11ImageKind::NationPreAggregated);
      if (!typed) continue;
      // Collision guard: verify stored nation_name matches query
      if (typed->nation_name != nation_name || !typed->data) continue;

      // Compute threshold in integer domain:
      //   row.value / 100.0 > (total / 100.0) * fraction
      // ⇔ row.value > total * fraction   (100.0 cancels)
      // Convert total to double once, multiply by fraction, cast back.
      Q11AccT threshold_raw = 0;
#if defined(__SIZEOF_INT128__)
      {
        double total_as_double = static_cast<double>(static_cast<int64_t>(
            static_cast<uint64_t>(typed->data->total)));
        threshold_raw = static_cast<Q11AccT>(total_as_double * fraction);
      }
#else
      threshold_raw = static_cast<Q11AccT>(
          static_cast<double>(typed->data->total) * fraction);
#endif

      // Inline emission: iterate pre-sorted rows, compare in integer domain
      sink.Begin(core::OutputSchema{});
      core::AuxRow aux_row;
      aux_row.values.reserve(2);
      bool done = false;
      for (const auto &chunk : typed->data->chunks) {
       for (const auto &row : chunk->rows) {
        if (row.value > threshold_raw) {
          aux_row.values.clear();
          aux_row.values.push_back(
              core::AuxValue::Of(core::AuxType::Int64, row.partkey));
          core::ScaledDecimal decimal;
#if defined(__SIZEOF_INT128__)
          decimal.value = static_cast<int64_t>(static_cast<uint64_t>(row.value));
#else
          decimal.value = row.value;
#endif
          decimal.scale = 2;
          aux_row.values.push_back(
              core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal));
          sink.WriteRow(aux_row);
        } else {
          done = true; // sorted by value DESC
          break;
        }
       }
       if (done)
         break;
      }
      sink.End();
      return;
    }
  }

  // On any miss (missing partition, collision, invalid image):
  // fall back to full build from base tables.
  ExecuteFallback(services, arguments, sink);
}

void TpchQ11Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  if (arguments.values.size() < 2 ||
      arguments.values[0].is_null ||
      arguments.values[1].is_null)
    return;

  std::string nation_name;
  double fraction = 0.0;
  if (std::holds_alternative<std::string>(arguments.values[0].payload))
    nation_name = std::get<std::string>(arguments.values[0].payload);
  else
    return;
  if (std::holds_alternative<double>(arguments.values[1].payload))
    fraction = std::get<double>(arguments.values[1].payload);
  else if (std::holds_alternative<int64_t>(arguments.values[1].payload))
    fraction = static_cast<double>(
        std::get<int64_t>(arguments.values[1].payload));
  else
    return;
  if (!std::isfinite(fraction))
    return;

  const auto built = BuildState(services);

  auto name_it = built.state->nation_name_to_key.find(nation_name);
  if (name_it == built.state->nation_name_to_key.end()) {
    EmitResult({}, sink);
    return;
  }
  const int32_t nationkey = name_it->second;

  auto nk_it = built.nations.find(nationkey);
  if (nk_it == built.nations.end()) {
    EmitResult({}, sink);
    return;
  }

  std::vector<TpchQ11OutputRow> rows;
  FlattenNationRows(*nk_it->second, rows);
  auto output = FinalizeQ11FromSorted(rows, nk_it->second->total, fraction);
  EmitResult(output, sink);
}

TpchQ11Built TpchQ11Module::BuildState(
    core::AuxEngineServices &services) const {
  (void)services;
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

  initial_scan->ScanPartsupp([&](const TpchQ11PartsuppInput &input) {
    Q11AccT product = 0;
    AddAcc(product, input.supplycost_raw, input.availqty);
    all[input.suppkey].parts.push_back(
        {input.partkey, static_cast<int64_t>(product), 1});
  });

  // Per-supplier: sort by partkey and fold duplicates (bag semantics).
  for (auto &[suppkey, entry] : all) {
    auto &parts = entry.parts;
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

  // Merge supplier contributions into per-nation maps.
  std::unordered_set<int32_t> known_nations;
  for (const auto &entry : state->nation_name_to_key)
    known_nations.insert(entry.second);
  std::unordered_map<int32_t, std::unordered_map<int64_t, Q11AccT>> merged;
  std::unordered_map<int32_t, Q11AccT> totals;
  for (const auto &[suppkey, entry] : all) {
    if (!entry.has_supplier || !known_nations.count(entry.nationkey))
      continue;
    auto &nation_map = merged[entry.nationkey];
    Q11AccT &total = totals[entry.nationkey];
    for (const auto &part : entry.parts) {
      nation_map[part.partkey] += static_cast<Q11AccT>(part.value);
      total += static_cast<Q11AccT>(part.value);
    }
  }

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

  TpchQ11Built built;
  for (auto it = merged.begin(); it != merged.end();) {
    std::vector<TpchQ11OutputRow> sorted;
    sorted.reserve(it->second.size());
    for (const auto &[pk, value] : it->second)
      if (value > 0)
        sorted.push_back({pk, value});
    std::sort(sorted.begin(), sorted.end(), RowBefore);
    built.nations[it->first] = MakeNationData(sorted, totals[it->first]);
    it = merged.erase(it);
  }
  built.state = std::move(state);
  return built;
}

void TpchQ11Module::EmitResult(const std::vector<TpchQ11OutputRow> &rows,
                               core::AuxRowSink &sink) const {
  sink.Begin(core::OutputSchema{});

  core::AuxRow aux_row;
  aux_row.values.reserve(2);
  for (const auto &row : rows) {
    aux_row.values.clear();
    aux_row.values.push_back(
        core::AuxValue::Of(core::AuxType::Int64, row.partkey));
    core::ScaledDecimal decimal;
#if defined(__SIZEOF_INT128__)
    decimal.value = static_cast<int64_t>(static_cast<uint64_t>(row.value));
#else
    decimal.value = row.value;
#endif
    decimal.scale = 2;
    aux_row.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal));
    sink.WriteRow(aux_row);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ11Module::CalibrationWrites() const {
  return TpchQ11CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query