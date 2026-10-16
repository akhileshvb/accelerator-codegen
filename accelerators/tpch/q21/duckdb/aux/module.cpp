#include "tpch/q21/duckdb/aux/module.hpp"
#include "tpch/q21/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/parallel_stable_sort.hpp"
#include "tpch/common/typed_row_batch.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kVersionV2 = 2;
constexpr uint32_t kSupplierMagic = 0x53323151; // Q21S
constexpr uint32_t kOrdersMagic = 0x4f323151;   // Q21O
constexpr uint32_t kNationMagic = 0x4e323151;   // Q21N
constexpr uint32_t kNationNameIdxMagic = 0x49323151; // Q21I

// Contributing orders per build slice: a small-vector scan and a hash add each.
constexpr std::size_t kContributionGrain = 65536;

// Maintenance grains: an order is a few hash probes and a small-vector scan, a
// sort element a key compare, an apply item one shard write, a nation op one
// row compare.
constexpr std::size_t kOrderGrain = 2048;
constexpr std::size_t kSortGrain = 65536;
constexpr std::size_t kApplyGrain = 1024;
constexpr std::size_t kNationOpGrain = 4096;

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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
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

bool CharCell(const core::AuxRow &row, std::size_t index, char &out) {
  std::string value;
  if (!StringCell(row, index, value) || value.empty())
    return false;
  out = value[0];
  return true;
}

// TPC-H Q21: ORDER BY numwait DESC, s_name LIMIT 100.
constexpr std::size_t kTpchQ21Limit = 100;

bool BetterOutputRow(const TpchQ21OutputRow &lhs, const TpchQ21OutputRow &rhs) {
  if (lhs.numwait != rhs.numwait)
    return lhs.numwait > rhs.numwait;
  return lhs.name < rhs.name;
}

// Image rows ordered (numwait DESC, name ASC).
bool ImageRowBefore(const TpchQ21ImageRow &a, const TpchQ21ImageRow &b) {
  if (a.numwait != b.numwait)
    return a.numwait > b.numwait;
  return *a.name < *b.name;
}

std::shared_ptr<const TpchQ21StateImage>
MakeImage(TpchQ21ImageKind kind, int32_t nationkey = 0) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = kind;
  image->nationkey = nationkey;
  return image;
}

std::shared_ptr<const TpchQ21StateImage>
MakeDirImage(std::shared_ptr<const TpchQ21Dir> dir) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::SupplierMetadata;
  image->dir = std::move(dir);
  return image;
}

std::shared_ptr<const TpchQ21StateImage>
MakeCoreImage(std::shared_ptr<const TpchQ21Core> core) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::OrderSummaries;
  image->core = std::move(core);
  return image;
}

std::shared_ptr<const TpchQ21StateImage>
MakeIndexImage(std::shared_ptr<const TpchQ21NationIndex> index) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::NationNameIndex;
  image->nation_index = std::move(index);
  return image;
}

std::shared_ptr<const TpchQ21StateImage>
MakeNationImage(int32_t nationkey,
                std::shared_ptr<const TpchQ21NationRows> rows,
                const std::string &nation_name = "") {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = TpchQ21ImageKind::Nation;
  image->nationkey = nationkey;
  image->nation_name = nation_name;
  image->rows = std::move(rows);
  return image;
}

const TpchQ21StateImage *AsQ21Image(const core::StateImage &image,
                                    TpchQ21ImageKind kind) {
  const auto *payload = image.As<TpchQ21StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  switch (kind) {
  case TpchQ21ImageKind::SupplierMetadata:
    return payload->dir ? payload : nullptr;
  case TpchQ21ImageKind::OrderSummaries:
    return payload->core ? payload : nullptr;
  case TpchQ21ImageKind::NationNameIndex:
    return payload->nation_index ? payload : nullptr;
  case TpchQ21ImageKind::Nation:
    return payload->rows ? payload : nullptr;
  }
  return nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::PartitionId NationPartition(int32_t nationkey) {
  return TpchQ21Layout::kNationBase + static_cast<core::PartitionId>(nationkey);
}

// Key the read image by the query parameter rather than first loading a global
// name->nation metadata image.  FNV-1a is stable across processes; collisions
// are detected by the image's retained name and take the safe fallback path.
core::PartitionId NationNamePartition(const std::string &name) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : name) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return TpchQ21Layout::kNationNameBase +
         static_cast<core::PartitionId>(hash % TpchQ21Layout::kNationNameSlots);
}

// ── Contribution computation ────────────────────────────────────────────────
// Reduce the two correlated EXISTS predicates to per-order sufficient facts:
// Q21 can contribute only if exactly one supplier is late and at least one
// different supplier participates. Returns true with the sole late supplier and
// its late-item count when the order contributes.
bool ComputeOrderContribution(const OrderSummary &order, int64_t &suppkey,
                              int64_t &count) {
  if (order.orderstatus != 'F' || order.suppliers.size() <= 1)
    return false;
  uint32_t late_suppliers = 0;
  for (const auto &info : order.suppliers) {
    if (info.total_count <= 0 || info.late_count <= 0)
      continue;
    if (++late_suppliers > 1)
      return false; // NOT EXISTS fails for every candidate.
    suppkey = info.suppkey;
    count = info.late_count;
  }
  return late_suppliers == 1;
}

// ── Mutation helpers ────────────────────────────────────────────────────────
bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey, int64_t &suppkey,
                  int32_t &commitdate, int32_t &receiptdate) {
  return IntCell(row, 0, orderkey) && IntCell(row, 1, suppkey) &&
         DateCell(row, 2, commitdate) && DateCell(row, 3, receiptdate);
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey, std::string &name,
                  int32_t &nationkey) {
  int64_t nk = 0;
  if (!IntCell(row, 0, suppkey) || !StringCell(row, 1, name) ||
      !IntCell(row, 2, nk))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey = static_cast<int32_t>(nk);
  return true;
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, char &orderstatus) {
  return IntCell(row, 0, orderkey) && CharCell(row, 1, orderstatus);
}

// ── Nation row patching ─────────────────────────────────────────────────────
using RowVec = std::vector<TpchQ21ImageRow>;

// A net change of one s_name's merged row. `hint` is the row's current numwait
// when the changed suppliers account for all of it (a binary-search key).
struct NameChange {
  std::shared_ptr<const std::string> name;
  int64_t delta = 0;
  int64_t hint = 0;
};

// Rewrites a nation's rows with `changes` in one pass: the touched rows are
// located (binary search by hint, one scan for the rest), dropped, and the
// re-priced rows merged back in order. False when a change would decrement a
// row that does not exist (inconsistent state).
bool PatchNationRows(const RowVec &rows, std::vector<NameChange> changes, RowVec &out) {
  std::stable_sort(changes.begin(), changes.end(),
                   [](const NameChange &a, const NameChange &b) { return *a.name < *b.name; });
  std::size_t kept = 0;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (kept != 0 && *changes[kept - 1].name == *changes[i].name) {
      changes[kept - 1].delta += changes[i].delta;
      changes[kept - 1].hint += changes[i].hint;
    } else if (kept++ != i) {
      changes[kept - 1] = std::move(changes[i]);
    }
  }
  changes.resize(kept);
  changes.erase(std::remove_if(changes.begin(), changes.end(),
                               [](const NameChange &c) { return c.delta == 0; }),
                changes.end());

  constexpr std::size_t kMissing = ~std::size_t{0};
  std::vector<std::size_t> at(changes.size(), kMissing);
  std::unordered_map<std::string_view, std::size_t> unlocated;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (changes[i].hint > 0) {
      const TpchQ21ImageRow probe{changes[i].name, changes[i].hint};
      const auto it = std::lower_bound(rows.begin(), rows.end(), probe, ImageRowBefore);
      if (it != rows.end() && it->numwait == probe.numwait && *it->name == *probe.name)
        at[i] = static_cast<std::size_t>(it - rows.begin());
    }
    if (at[i] == kMissing)
      unlocated.emplace(*changes[i].name, i);
  }
  if (!unlocated.empty()) {
    for (std::size_t r = 0; r < rows.size(); ++r) {
      const auto it = unlocated.find(*rows[r].name);
      if (it != unlocated.end())
        at[it->second] = r;
    }
  }

  std::vector<std::size_t> dropped;
  RowVec fresh;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const bool found = at[i] != kMissing;
    if (!found && changes[i].delta < 0)
      return false;
    if (found)
      dropped.push_back(at[i]);
    const int64_t sum = changes[i].delta + (found ? rows[at[i]].numwait : 0);
    if (sum > 0)
      fresh.push_back({changes[i].name, sum});
  }
  std::sort(dropped.begin(), dropped.end());
  std::sort(fresh.begin(), fresh.end(), ImageRowBefore);

  out.clear();
  out.reserve(rows.size() + fresh.size());
  std::size_t next_drop = 0, next_fresh = 0;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (next_drop < dropped.size() && dropped[next_drop] == r) {
      ++next_drop;
      continue;
    }
    for (; next_fresh < fresh.size() && ImageRowBefore(fresh[next_fresh], rows[r]); ++next_fresh)
      out.push_back(fresh[next_fresh]);
    out.push_back(rows[r]);
  }
  out.insert(out.end(), fresh.begin() + static_cast<std::ptrdiff_t>(next_fresh), fresh.end());
  return true;
}

// Full recomputation of one nation's rows from the numwait totals.
std::shared_ptr<const TpchQ21NationRows>
ComputeNationRows(int32_t nationkey, const TpchQ21Dir &dir,
                  const TpchQ21Core &core) {
  std::map<std::string, std::pair<std::shared_ptr<const std::string>, int64_t>>
      by_name;
  core.numwait.ForEach([&](int64_t suppkey, int64_t nw) {
    if (nw <= 0)
      return;
    const auto *supplier = dir.Find(suppkey);
    if (!supplier || supplier->nationkey != nationkey)
      return;
    auto &slot = by_name[*supplier->name];
    if (!slot.first)
      slot.first = supplier->name;
    slot.second += nw;
  });
  auto out = std::make_shared<TpchQ21NationRows>();
  out->rows.reserve(by_name.size());
  for (auto &[name, entry] : by_name) {
    (void)name;
    out->rows.push_back({entry.first, entry.second});
  }
  std::sort(out->rows.begin(), out->rows.end(), ImageRowBefore);
  return out;
}

void EmitRow(core::AuxRowSink &sink, const std::string &name, int64_t numwait) {
  core::AuxRow out;
  out.values.reserve(2);
  out.values.push_back(core::AuxValue::Of(core::AuxType::String, name));
  out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, numwait));
  sink.WriteRow(out);
}

} // namespace

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ21SupplierMetadata(const TpchQ21State &state) {
  std::map<int64_t, TpchQ21Supplier> merged;
  if (state.dir->base)
    merged.insert(state.dir->base->begin(), state.dir->base->end());
  state.dir->overlay.ForEach([&](int64_t key, const TpchQ21SupplierOverlay &ov) {
    if (ov.present)
      merged[key] = ov.supplier;
    else
      merged.erase(key);
  });
  ByteWriter writer;
  core::WriteImageHeader(writer, kSupplierMagic, kVersion,
                         static_cast<uint64_t>(merged.size()));
  for (const auto &[key, supplier] : merged) {
    writer.I64(key);
    writer.String(supplier.name ? *supplier.name : std::string());
    writer.I32(supplier.nationkey);
  }
  std::vector<std::string> nation_names;
  for (const auto &entry : state.nation_index->nation_name_to_keys)
    nation_names.push_back(entry.first);
  std::sort(nation_names.begin(), nation_names.end());
  writer.U32(static_cast<uint32_t>(nation_names.size()));
  for (const auto &name : nation_names) {
    writer.String(name);
    const auto &nkeys = state.nation_index->nation_name_to_keys.at(name);
    writer.U32(static_cast<uint32_t>(nkeys.size()));
    for (const auto nk : nkeys)
      writer.I32(nk);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ21OrderSummaries(const TpchQ21State &state) {
  std::map<int64_t, OrderSummary> merged;
  state.core->orders.ForEach([&](int64_t key, const OrderSummary &summary) {
    merged[key] = summary;
  });
  std::map<int64_t, int64_t> numwait;
  state.core->numwait.ForEach([&](int64_t key, int64_t nw) {
    if (nw != 0)
      numwait[key] = nw;
  });
  ByteWriter writer;
  core::WriteImageHeader(writer, kOrdersMagic, kVersionV2,
                         static_cast<uint64_t>(merged.size()));
  for (const auto &[key, summary] : merged) {
    writer.I64(key);
    writer.U8(static_cast<uint8_t>(summary.orderstatus));
    writer.U32(static_cast<uint32_t>(summary.suppliers.size()));
    for (const auto &info : summary.suppliers) {
      writer.I64(info.suppkey);
      writer.I64(info.total_count);
      writer.I64(info.late_count);
    }
  }
  writer.U32(static_cast<uint32_t>(numwait.size()));
  for (const auto &[key, nw] : numwait) {
    writer.I64(key);
    writer.I64(nw);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ21NationNameIndex(const TpchQ21State &state) {
  ByteWriter writer;
  core::WriteImageHeader(
      writer, kNationNameIdxMagic, kVersion,
      static_cast<uint64_t>(state.nation_index->nation_name_to_keys.size()));
  std::vector<std::string> nation_names;
  for (const auto &entry : state.nation_index->nation_name_to_keys)
    nation_names.push_back(entry.first);
  std::sort(nation_names.begin(), nation_names.end());
  for (const auto &name : nation_names) {
    writer.String(name);
    const auto &nkeys = state.nation_index->nation_name_to_keys.at(name);
    writer.U32(static_cast<uint32_t>(nkeys.size()));
    for (const auto nk : nkeys)
      writer.I32(nk);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ21Nation(int32_t nationkey,
                                          const TpchQ21State &state) {
  ByteWriter writer;
  const auto it = state.nations.find(nationkey);
  const uint64_t count =
      it == state.nations.end() ? 0
                                : static_cast<uint32_t>(it->second->rows.size());
  core::WriteImageHeader(writer, kNationMagic, kVersionV2, count);
  if (it != state.nations.end()) {
    for (const auto &row : it->second->rows) {
      writer.I64(row.numwait);
      writer.String(*row.name);
    }
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ21SupplierMetadata(const std::vector<uint8_t> &bytes,
                                    TpchQ21State &state) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kSupplierMagic, kVersion, count))
    return false;
  auto base = std::make_shared<std::unordered_map<int64_t, TpchQ21Supplier>>();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    std::string name;
    int32_t nationkey = 0;
    if (!reader.I64(key) || !reader.String(name) || !reader.I32(nationkey))
      return false;
    (*base)[key] = {std::make_shared<const std::string>(std::move(name)),
                    nationkey};
  }
  state.dir = std::make_shared<TpchQ21Dir>();
  state.dir->base = std::move(base);
  uint32_t name_count = 0;
  if (!reader.U32(name_count))
    return reader.offset == bytes.size();
  state.nation_index->nation_name_to_keys.clear();
  for (uint32_t i = 0; i < name_count; ++i) {
    std::string name;
    uint32_t nk_count = 0;
    if (!reader.String(name) || !reader.U32(nk_count))
      return false;
    std::vector<int32_t> nkeys;
    for (uint32_t j = 0; j < nk_count; ++j) {
      int32_t nk = 0;
      if (!reader.I32(nk))
        return false;
      nkeys.push_back(nk);
    }
    state.nation_index->nation_name_to_keys.emplace(std::move(name),
                                                    std::move(nkeys));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ21OrderSummaries(const std::vector<uint8_t> &bytes,
                                  TpchQ21State &state) {
  ByteReader reader_v2{bytes};
  ByteReader reader_v1{bytes};
  uint64_t count = 0;
  bool v2 = core::ReadImageHeader(reader_v2, kOrdersMagic, kVersionV2, count);
  if (!v2 && !core::ReadImageHeader(reader_v1, kOrdersMagic, kVersion, count))
    return false;
  ByteReader &reader = v2 ? reader_v2 : reader_v1;
  using Orders = core::ShardedCowHashMap<OrderSummary, 7>;
  state.core = std::make_shared<TpchQ21Core>(Orders::ShardBitsFor(count), 7);
  for (uint64_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint8_t status = 0;
    uint32_t supplier_count = 0;
    if (!reader.I64(key) || !reader.U8(status) || !reader.U32(supplier_count))
      return false;
    OrderSummary summary;
    summary.orderstatus = static_cast<char>(status);
    for (uint32_t j = 0; j < supplier_count; ++j) {
      SupplierOrderInfo info;
      if (!reader.I64(info.suppkey) || !reader.I64(info.total_count) ||
          !reader.I64(info.late_count))
        return false;
      summary.suppliers.push_back(info);
    }
    state.core->orders.Upsert(key, std::move(summary));
  }
  if (v2) {
    uint32_t nw_count = 0;
    if (!reader.U32(nw_count))
      return false;
    state.core->numwait =
        core::ShardedCowHashMap<int64_t, 7>(core::ShardedCowHashMap<int64_t, 7>::ShardBitsFor(nw_count));
    for (uint32_t i = 0; i < nw_count; ++i) {
      int64_t key = 0, nw = 0;
      if (!reader.I64(key) || !reader.I64(nw))
        return false;
      state.core->numwait[key] = nw;
    }
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ21NationNameIndex(const std::vector<uint8_t> &bytes,
                                   TpchQ21State &state) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kNationNameIdxMagic, kVersion, count))
    return false;
  state.nation_index->nation_name_to_keys.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string name;
    uint32_t nk_count = 0;
    if (!reader.String(name) || !reader.U32(nk_count))
      return false;
    std::vector<int32_t> nkeys;
    for (uint32_t j = 0; j < nk_count; ++j) {
      int32_t nk = 0;
      if (!reader.I32(nk))
        return false;
      nkeys.push_back(nk);
    }
    state.nation_index->nation_name_to_keys.emplace(std::move(name),
                                                    std::move(nkeys));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ21Nation(const std::vector<uint8_t> &bytes, int32_t nationkey,
                          TpchQ21State &state) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kNationMagic, kVersionV2, count))
    return false;
  auto rows = std::make_shared<TpchQ21NationRows>();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t numwait = 0;
    std::string name;
    if (!reader.I64(numwait) || !reader.String(name))
      return false;
    if (numwait > 0)
      rows->rows.push_back(
          {std::make_shared<const std::string>(std::move(name)), numwait});
  }
  std::sort(rows->rows.begin(), rows->rows.end(), ImageRowBefore);
  state.nations[nationkey] = std::move(rows);
  return reader.offset == bytes.size();
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ21TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ21TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitem_deltas.clear();
  order_deltas.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
bool TpchQ21TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ21LineitemDelta> &TpchQ21TxnBuffer::LineitemDeltas() const {
  return lineitem_deltas;
}
const std::vector<TpchQ21OrderDelta> &TpchQ21TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
const std::vector<TpchQ21SupplierDelta> &TpchQ21TxnBuffer::SupplierDeltas() const {
  return supplier_deltas;
}
const std::vector<TpchQ21NationDelta> &TpchQ21TxnBuffer::NationDeltas() const {
  return nation_deltas;
}
void TpchQ21TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  lineitem_deltas.clear();
  order_deltas.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
void TpchQ21TxnBuffer::AddLineitemDelta(TpchQ21LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ21TxnBuffer::AddOrderDelta(TpchQ21OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}
void TpchQ21TxnBuffer::AddSupplierDelta(TpchQ21SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(std::move(delta));
}
void TpchQ21TxnBuffer::AddNationDelta(TpchQ21NationDelta delta) {
  pending = true;
  nation_deltas.push_back(std::move(delta));
}

namespace {

// ── Order maintenance ───────────────────────────────────────────────────────
// An order touched by the commit, with its lineitem deltas as the range
// [first, last) of the orderkey-sorted delta index.
struct AffectedOrder {
  enum class Source : uint8_t { Tracked, Inserted, Lookup, Skip };
  int64_t orderkey = 0;
  bool has_order_delta = false;
  char first_old_status = 0; // status before the commit's first order delta
  bool status_changed = false;
  char new_status = 0; // status after the commit's last status change
  std::size_t first = 0, last = 0;
  Source source = Source::Skip;
  std::size_t lookup = 0; // index into the batched lookups (Source::Lookup)
};

// The new state of one affected order: put it in the map, take it out, or leave
// the map alone.
struct OrderUpdate {
  enum class Action : uint8_t { None, Put, Erase };
  Action action = Action::None;
  OrderSummary summary;
};

using KeyedIndex = std::vector<std::pair<int64_t, std::size_t>>;

template <class Deltas>
KeyedIndex SortedByOrder(const core::AuxEngineServices &services, const Deltas &deltas) {
  KeyedIndex index(deltas.size());
  for (std::size_t i = 0; i < deltas.size(); ++i)
    index[i] = {deltas[i].orderkey, i};
  ParallelStableSort(
      services, index, [](const auto &a, const auto &b) { return a.first < b.first; },
      kSortGrain);
  return index;
}

struct AffectedOrders {
  std::vector<AffectedOrder> orders; // ascending orderkey
  KeyedIndex lineitems;              // (orderkey, delta position), arrival order within an order
};

// Joins the orderkey-sorted order and lineitem deltas into one entry per order.
AffectedOrders CollectAffectedOrders(const core::AuxEngineServices &services,
                                     const TpchQ21TxnBuffer &buffer) {
  const auto &order_deltas = buffer.OrderDeltas();
  const auto by_order = SortedByOrder(services, order_deltas);
  AffectedOrders affected;
  affected.lineitems = SortedByOrder(services, buffer.LineitemDeltas());
  const auto &by_lineitem = affected.lineitems;
  std::size_t o = 0, l = 0;
  while (o < by_order.size() || l < by_lineitem.size()) {
    AffectedOrder order;
    order.orderkey = o == by_order.size()      ? by_lineitem[l].first
                     : l == by_lineitem.size() ? by_order[o].first
                                               : std::min(by_order[o].first, by_lineitem[l].first);
    for (; o < by_order.size() && by_order[o].first == order.orderkey; ++o) {
      const auto &delta = order_deltas[by_order[o].second];
      if (!order.has_order_delta)
        order.first_old_status = delta.old_orderstatus;
      order.has_order_delta = true;
      if (delta.old_orderstatus != delta.new_orderstatus) {
        order.status_changed = true;
        order.new_status = delta.new_orderstatus;
      }
    }
    order.first = l;
    while (l < by_lineitem.size() && by_lineitem[l].first == order.orderkey)
      ++l;
    order.last = l;
    affected.orders.push_back(order);
  }
  return affected;
}

// Absent orders contribute nothing, so an absent one is only tracked again when
// this commit inserts it; any other absent order is looked up in the table.
AffectedOrder::Source ClassifyOrder(const AffectedOrder &order, bool tracked) {
  using Source = AffectedOrder::Source;
  if (tracked)
    return Source::Tracked;
  if (order.status_changed && order.new_status == 0)
    return Source::Skip; // deleted by this commit
  if (!order.has_order_delta || order.first_old_status != 0 || !order.status_changed)
    return Source::Lookup;
  return Source::Inserted; // RF1: its lineitems cannot predate it
}

OrderSummary SummaryOf(const TpchQ21InitialScan::OrderLookup &lookup) {
  OrderSummary summary;
  summary.orderstatus = lookup.status;
  for (const auto &entry : lookup.suppliers)
    if (entry[1] > 0)
      summary.suppliers.push_back({entry[0], entry[1], entry[2]});
  return summary;
}

} // namespace

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ21Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ21Module::Dependencies() const {
  return {{config.supplier_table, {0, 1, 2}, {0, 1, 3}},
          {config.orders_table, {0, 1}, {0, 2}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 2, 11, 12}},
          {config.nation_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ21Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ21TxnBuffer>();
}

void TpchQ21Module::Build(core::AuxEngineServices &services,
                           core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const std::shared_ptr<const TpchQ21Dir> dir = state.dir;
  const std::shared_ptr<const TpchQ21Core> core_state = state.core;
  const std::shared_ptr<const TpchQ21NationIndex> index = state.nation_index;

  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kSupplierMetadata, 0,
      MakeDirImage(dir)));
  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kOrderSummaries, 0,
      MakeCoreImage(core_state)));
  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kNationNameIndex, 0,
      MakeIndexImage(index)));
  // Persist NationPartition images so StageMaintenance can patch them.
  for (const auto &[nk, rows] : state.nations) {
    if (rows->rows.empty())
      continue;
    builder.Write(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationPartition(nk), 0,
        MakeNationImage(nk, rows)));
  }
  // Materialize the result at the query-parameter grain: one FNV-1a-hashed
  // partition read directly from the nation name, collision-checked by name.
  for (const auto &[name, nationkeys] : index->nation_name_to_keys) {
    if (nationkeys.size() != 1)
      continue; // uncommon duplicate names retain the correctness slow path.
    const int32_t nk = nationkeys.front();
    const auto it = state.nations.find(nk);
    auto rows = it != state.nations.end()
                    ? it->second
                    : std::make_shared<const TpchQ21NationRows>();
    builder.Write(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationNamePartition(name), 0,
        MakeNationImage(nk, std::move(rows), name)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ21Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ21Module::AccumulateMutation(const core::MutationBatch &mutation,
                                        core::AuxEngineServices &,
                                        core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21TxnBuffer &>(base_buffer);

  if (mutation.table == config.supplier_table) {
    // Supplier changes patch the directory and the affected nations' rows.
    const auto remove = [&](const core::AuxRow &row) {
      int64_t key = 0;
      std::string name;
      int32_t nationkey = 0;
      if (!ReadSupplier(row, key, name, nationkey)) {
        buffer.MarkRebuild();
        return;
      }
      TpchQ21SupplierDelta d;
      d.suppkey = key;
      d.had_old = true;
      buffer.AddSupplierDelta(std::move(d));
    };
    const auto put = [&](const core::AuxRow &row) {
      TpchQ21SupplierDelta d;
      if (!ReadSupplier(row, d.suppkey, d.new_name, d.new_nationkey)) {
        buffer.MarkRebuild();
        return;
      }
      d.has_new = true;
      buffer.AddSupplierDelta(std::move(d));
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        put(row);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        remove(row);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, nk = 0;
        std::string oname, nname;
        int32_t onation = 0, nnation = 0;
        if (ReadSupplier(mutation.old_rows[i], ok, oname, onation) &&
            ReadSupplier(mutation.new_rows[i], nk, nname, nnation) &&
            ok == nk && oname == nname && onation == nnation)
          continue; // irrelevant columns changed only
        remove(mutation.old_rows[i]);
        put(mutation.new_rows[i]);
      }
      break;
    }
    return;
  }

  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0;
      char orderstatus = 0;
      if (!ReadOrder(row, orderkey, orderstatus)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta({orderkey, static_cast<char>(sign > 0 ? 0 : orderstatus),
                            static_cast<char>(sign > 0 ? orderstatus : 0)});
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
        int64_t old_orderkey = 0, new_orderkey = 0;
        char old_status = 0, new_status = 0;
        if (!ReadOrder(mutation.old_rows[i], old_orderkey, old_status) ||
            !ReadOrder(mutation.new_rows[i], new_orderkey, new_status) ||
            old_orderkey != new_orderkey) {
          buffer.MarkRebuild();
          return;
        }
        if (old_status == new_status)
          continue; // only untracked columns changed
        buffer.AddOrderDelta({old_orderkey, old_status, new_status});
      }
      break;
    }
    return;
  }

  if (mutation.table == config.nation_table) {
    // Nation changes can rename/remove a name whose parameter-keyed
    // NationNamePartition image would otherwise stay Valid and serve stale
    // rows.  Nations are effectively static; rebuild instead of patching.
    buffer.MarkRebuild();
    return;
  }

  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0, suppkey = 0;
    int32_t commitdate = 0, receiptdate = 0;
    if (!ReadLineitem(row, orderkey, suppkey, commitdate, receiptdate)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddLineitemDelta({orderkey, suppkey, commitdate, receiptdate, sign});
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
      int64_t ook = 0, osk = 0, nok = 0, nsk = 0;
      int32_t ocd = 0, ord = 0, ncd = 0, nrd = 0;
      if (ReadLineitem(mutation.old_rows[i], ook, osk, ocd, ord) &&
          ReadLineitem(mutation.new_rows[i], nok, nsk, ncd, nrd) &&
          ook == nok && osk == nsk && ocd == ncd && ord == nrd)
        continue; // only untracked columns changed
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ21Module::StageMaintenance(core::AuxEngineServices &services,
                                 const core::InvalidationPlan &,
                                 core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  const auto rebuild_all = [&]() {
    outcome = core::StagedMaintenanceOutcome{};
    outcome.action = core::MaintenanceAction::RebuildAll;
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

  const auto &lineitem_deltas = buffer.LineitemDeltas();
  const auto &order_deltas = buffer.OrderDeltas();
  const auto &supplier_deltas = buffer.SupplierDeltas();
  const auto &nation_deltas = buffer.NationDeltas();

  if (lineitem_deltas.empty() && order_deltas.empty() &&
      supplier_deltas.empty() && nation_deltas.empty())
    return outcome;

  const auto total_deltas = lineitem_deltas.size() + order_deltas.size() +
                            supplier_deltas.size() + nation_deltas.size();
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Load the three side partitions (pointer fetches; nothing is copied).
  const auto visibility = services.CurrentVisibility();
  const auto side_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ21Layout::kSupplierMetadata,
                   TpchQ21Layout::kOrderSummaries,
                   TpchQ21Layout::kNationNameIndex});
  std::shared_ptr<const TpchQ21Dir> parent_dir;
  std::shared_ptr<const TpchQ21Core> parent_core;
  std::shared_ptr<const TpchQ21NationIndex> index;
  for (const auto &image : side_snapshot.partitions) {
    if (image.partition == TpchQ21Layout::kSupplierMetadata) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::SupplierMetadata);
      if (!typed)
        return rebuild_all();
      parent_dir = typed->dir;
    } else if (image.partition == TpchQ21Layout::kOrderSummaries) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::OrderSummaries);
      if (!typed)
        return rebuild_all();
      parent_core = typed->core;
    } else if (image.partition == TpchQ21Layout::kNationNameIndex) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::NationNameIndex);
      if (!typed)
        return rebuild_all();
      index = typed->nation_index;
    }
  }
  if (!parent_dir || !parent_core || !index)
    return rebuild_all();

  // Staged copies share every shard with the parents; a write clones only the
  // shard it touches, so published versions are never mutated.
  auto next_core = std::make_shared<TpchQ21Core>(*parent_core);
  auto next_dir = std::make_shared<TpchQ21Dir>(*parent_dir);

  // ── Orders: classify, look up the untracked ones, compute the new states ──
  auto affected_orders = CollectAffectedOrders(services, buffer);
  auto &orders = affected_orders.orders;
  const auto &sorted_lineitems = affected_orders.lineitems;
  core::ParallelEach(services, orders.size(), kOrderGrain, [&](std::size_t i) {
    orders[i].source = ClassifyOrder(orders[i], parent_core->orders.Contains(orders[i].orderkey));
  });
  std::vector<int64_t> lookup_keys;
  for (auto &order : orders) {
    if (order.source == AffectedOrder::Source::Lookup) {
      order.lookup = lookup_keys.size();
      lookup_keys.push_back(order.orderkey);
    }
  }
  std::vector<TpchQ21InitialScan::OrderLookup> lookups;
  if (!lookup_keys.empty() &&
      (!initial_scan || !initial_scan->LookupOrderSummaries(lookup_keys, lookups)))
    return rebuild_all();

  using Numwait = std::unordered_map<int64_t, int64_t>;
  Numwait numwait_delta; // suppkey -> change
  std::vector<OrderUpdate> updates(orders.size());
  const auto compute_order = [&](std::size_t i, Numwait &sums) {
    using Source = AffectedOrder::Source;
    const AffectedOrder &order = orders[i];
    OrderUpdate &update = updates[i];
    if (order.source == Source::Skip)
      return;
    if (order.source == Source::Lookup) {
      const auto &lookup = lookups[order.lookup];
      if (!lookup.found || lookup.status != 'F')
        return;
      auto fresh = SummaryOf(lookup);
      int64_t supp = 0, count = 0;
      if (ComputeOrderContribution(fresh, supp, count)) {
        sums[supp] += count;
        update = {OrderUpdate::Action::Put, std::move(fresh)};
      }
      return;
    }
    const bool tracked = order.source == Source::Tracked;
    OrderSummary summary = tracked ? *parent_core->orders.Find(order.orderkey) : OrderSummary{};
    int64_t old_supp = 0, old_count = 0;
    const bool had_contrib = ComputeOrderContribution(summary, old_supp, old_count);
    if (order.status_changed)
      summary.orderstatus = order.new_status;
    for (std::size_t k = order.first; k < order.last; ++k) {
      const auto &delta = lineitem_deltas[sorted_lineitems[k].second];
      auto *info = summary.FindSupplier(delta.suppkey);
      if (!info) {
        summary.suppliers.push_back({delta.suppkey, 0, 0});
        info = &summary.suppliers.back();
      }
      info->total_count += delta.sign;
      if (delta.receiptdate > delta.commitdate)
        info->late_count += delta.sign;
      if (info->total_count <= 0 && info->late_count <= 0)
        summary.suppliers.erase(summary.suppliers.begin() + (info - summary.suppliers.data()));
    }
    int64_t new_supp = 0, new_count = 0;
    const bool has_contrib = ComputeOrderContribution(summary, new_supp, new_count);
    if (had_contrib)
      sums[old_supp] -= old_count;
    if (has_contrib)
      sums[new_supp] += new_count;
    if (has_contrib)
      update = {OrderUpdate::Action::Put, std::move(summary)};
    else if (tracked)
      update.action = OrderUpdate::Action::Erase;
  };
  core::ParallelReduce(
      services, orders.size(), kOrderGrain, numwait_delta,
      [&](Numwait &sums, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i)
          compute_order(i, sums);
      },
      [](Numwait &into, Numwait &&from) {
        for (const auto &[suppkey, delta] : from)
          into[suppkey] += delta;
      });

  // ── Apply the order updates: each task owns whole shards of the map ───────
  using ShardedItems = std::vector<std::pair<std::size_t, std::size_t>>; // (shard, item)
  const auto shard_runs = [](const ShardedItems &items) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < items.size(); ++i)
      if (i == 0 || items[i].first != items[i - 1].first)
        starts.push_back(i);
    starts.push_back(items.size());
    return starts;
  };
  const auto by_shard = [](const auto &a, const auto &b) { return a.first < b.first; };
  ShardedItems order_items;
  for (std::size_t i = 0; i < updates.size(); ++i)
    if (updates[i].action != OrderUpdate::Action::None)
      order_items.push_back({next_core->orders.ShardOf(orders[i].orderkey), i});
  ParallelStableSort(services, order_items, by_shard, kSortGrain);
  const auto order_runs = shard_runs(order_items);
  core::ParallelEach(services, order_runs.size() - 1, kApplyGrain, [&](std::size_t run) {
    for (std::size_t k = order_runs[run]; k < order_runs[run + 1]; ++k) {
      const std::size_t i = order_items[k].second;
      if (updates[i].action == OrderUpdate::Action::Put)
        next_core->orders.Upsert(orders[i].orderkey, std::move(updates[i].summary));
      else
        next_core->orders.Erase(orders[i].orderkey);
    }
  });

  // ── Apply the numwait deltas; remember each affected supplier's change ────
  struct Affected {
    int64_t suppkey = 0;
    int64_t old_nw = 0;
    int64_t new_nw = 0;
  };
  struct NumwaitChange {
    std::size_t shard = 0;
    Affected supplier;
    int64_t delta = 0;
  };
  std::vector<NumwaitChange> changes;
  for (const auto &[suppkey, delta] : numwait_delta)
    if (delta != 0)
      changes.push_back({next_core->numwait.ShardOf(suppkey), {suppkey, 0, 0}, delta});
  ParallelStableSort(
      services, changes,
      [](const NumwaitChange &a, const NumwaitChange &b) {
        return a.shard != b.shard ? a.shard < b.shard : a.supplier.suppkey < b.supplier.suppkey;
      },
      kSortGrain);
  std::vector<std::size_t> change_runs;
  for (std::size_t i = 0; i < changes.size(); ++i)
    if (i == 0 || changes[i].shard != changes[i - 1].shard)
      change_runs.push_back(i);
  change_runs.push_back(changes.size());
  core::ParallelEach(services, change_runs.size() - 1, kApplyGrain, [&](std::size_t run) {
    for (std::size_t k = change_runs[run]; k < change_runs[run + 1]; ++k) {
      auto &change = changes[k];
      const int64_t *cur = next_core->numwait.Find(change.supplier.suppkey);
      change.supplier.old_nw = cur ? *cur : 0;
      change.supplier.new_nw = change.supplier.old_nw + change.delta;
      if (change.supplier.new_nw == 0)
        next_core->numwait.Erase(change.supplier.suppkey);
      else
        next_core->numwait[change.supplier.suppkey] = change.supplier.new_nw;
    }
  });
  std::vector<Affected> affected;
  affected.reserve(changes.size() + supplier_deltas.size());
  for (const auto &change : changes)
    affected.push_back(change.supplier);
  const auto by_suppkey = [](const Affected &a, const Affected &b) { return a.suppkey < b.suppkey; };
  std::sort(affected.begin(), affected.end(), by_suppkey);

  // ── Supplier directory deltas ─────────────────────────────────────────────
  const auto numwait_changed = affected.size();
  for (const auto &delta : supplier_deltas) {
    if (!std::binary_search(affected.begin(), affected.begin() + numwait_changed,
                            Affected{delta.suppkey}, by_suppkey)) {
      const int64_t *cur = next_core->numwait.Find(delta.suppkey);
      affected.push_back({delta.suppkey, cur ? *cur : 0, cur ? *cur : 0});
    }
    // had_old only records that a row went away; the final directory state is
    // the last delta per supplier (delete then insert = replace).
    auto &slot = next_dir->overlay[delta.suppkey];
    if (delta.has_new) {
      slot.present = true;
      slot.supplier = {std::make_shared<const std::string>(delta.new_name),
                       delta.new_nationkey};
    } else {
      slot.present = false;
      slot.supplier = {};
    }
  }

  // ── Patch the affected nations' rows ──────────────────────────────────────
  std::map<int32_t, std::vector<NameChange>> name_changes;
  for (const auto &supplier : affected) {
    const auto *old_rec = parent_dir->Find(supplier.suppkey);
    const auto *new_rec = next_dir->Find(supplier.suppkey);
    const int64_t old_contribution = std::max<int64_t>(supplier.old_nw, 0);
    const int64_t new_contribution = std::max<int64_t>(supplier.new_nw, 0);
    if (old_rec && old_contribution > 0)
      name_changes[old_rec->nationkey].push_back({old_rec->name, -old_contribution, old_contribution});
    if (new_rec && new_contribution > 0)
      name_changes[new_rec->nationkey].push_back({new_rec->name, new_contribution, 0});
  }

  std::vector<int32_t> nations;
  std::vector<std::vector<NameChange>> nation_changes;
  std::size_t nation_ops = 0;
  for (auto &[nk, nation_ops_of] : name_changes) {
    nations.push_back(nk);
    nation_ops += nation_ops_of.size();
    nation_changes.push_back(std::move(nation_ops_of));
  }
  std::vector<std::shared_ptr<const TpchQ21NationRows>> nation_rows(nations.size());
  if (!nations.empty()) {
    std::vector<core::PartitionId> partitions;
    for (const auto nk : nations)
      partitions.push_back(NationPartition(nk));
    const auto nation_snapshot =
        services.StateStore().ReadPartitions(visibility, partitions);
    for (const auto &image : nation_snapshot.partitions) {
      if (image.partition < TpchQ21Layout::kNationBase)
        continue;
      const auto nk = static_cast<int32_t>(image.partition -
                                           TpchQ21Layout::kNationBase);
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
      if (!typed || typed->nationkey != nk)
        return rebuild_all();
      const auto at = std::find(nations.begin(), nations.end(), nk) - nations.begin();
      nation_rows[static_cast<std::size_t>(at)] = typed->rows;
    }
  }
  const RowVec no_rows; // an absent image = no waiters yet
  std::vector<RowVec> patched(nations.size());
  std::vector<char> patched_ok(nations.size(), 0);
  core::ParallelEach(services, nations.size(), nation_ops < kNationOpGrain ? nations.size() : 1,
                     [&](std::size_t i) {
                       patched_ok[i] = PatchNationRows(nation_rows[i] ? nation_rows[i]->rows : no_rows,
                                                       std::move(nation_changes[i]), patched[i]);
                     });
  if (std::find(patched_ok.begin(), patched_ok.end(), 0) != patched_ok.end())
    return rebuild_all();

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const std::shared_ptr<const TpchQ21Core> core_published = next_core;
  const std::shared_ptr<const TpchQ21Dir> dir_published = next_dir;

  // Only publish the partitions that changed.
  if (!lineitem_deltas.empty() || !order_deltas.empty()) {
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, TpchQ21Layout::kOrderSummaries, 0,
        MakeCoreImage(core_published)));
  }
  if (!supplier_deltas.empty()) {
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, TpchQ21Layout::kSupplierMetadata, 0,
        MakeDirImage(dir_published)));
  }

  // nationkey -> unique name (duplicate names keep the slow path).
  std::unordered_map<int32_t, std::string> nk_to_name;
  for (const auto &[name, nkeys] : index->nation_name_to_keys) {
    if (nkeys.size() != 1)
      continue;
    nk_to_name.emplace(nkeys.front(), name);
  }
  for (std::size_t i = 0; i < nations.size(); ++i) {
    const int32_t nk = nations[i];
    auto published = std::make_shared<TpchQ21NationRows>();
    published->rows = std::move(patched[i]);
    const std::shared_ptr<const TpchQ21NationRows> shared = published;
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationPartition(nk), 0,
        MakeNationImage(nk, shared)));
    const auto name_it = nk_to_name.find(nk);
    if (name_it != nk_to_name.end()) {
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ21Layout::kAccelerator, NationNamePartition(name_it->second), 0,
          MakeNationImage(nk, shared, name_it->second)));
    }
  }
  (void)nation_deltas;
  return outcome;
}

bool TpchQ21Module::CheckValidity(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ21Module::Execute(core::AuxEngineServices &services,
                             const core::QueryArguments &arguments,
                             core::AuxRowSink &sink) {
  if (arguments.values.empty() || arguments.values[0].is_null) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::string nation_name;
  if (std::holds_alternative<std::string>(arguments.values[0].payload)) {
    nation_name = std::get<std::string>(arguments.values[0].payload);
  } else {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto visibility = services.CurrentVisibility();

  // ── Fast path: single-partition read via FNV-1a nation-name hash ──────
  {
    const auto fast_snapshot = services.StateStore().ReadPartitions(
        visibility, {NationNamePartition(nation_name)});
    for (const auto &image : fast_snapshot.partitions) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
      if (!typed) continue;
      // Collision guard: verify the stored nation_name matches the query.
      if (typed->nation_name != nation_name) continue;
      // An empty hit falls through to the NationPartition path rather than
      // being treated as authoritative.
      if (!typed->rows->rows.empty()) {
        sink.Begin(core::OutputSchema{});
        // rows are kept in output order (numwait DESC, s_name): TPC-H's LIMIT 100
        const auto n = std::min(typed->rows->rows.size(), kTpchQ21Limit);
        for (std::size_t i = 0; i < n; ++i)
          EmitRow(sink, *typed->rows->rows[i].name, typed->rows->rows[i].numwait);
        sink.End();
        return;
      }
      break;
    }
  }

  // ── Slow path: resolve nation name via the tiny NationNameIndex ────────
  const auto idx_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ21Layout::kNationNameIndex});
  std::shared_ptr<const TpchQ21NationIndex> idx_state;
  for (const auto &image : idx_snapshot.partitions) {
    if (image.partition == TpchQ21Layout::kNationNameIndex) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::NationNameIndex);
      if (typed) idx_state = typed->nation_index;
      break;
    }
  }
  if (!idx_state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto name_it = idx_state->nation_name_to_keys.find(nation_name);
  if (name_it == idx_state->nation_name_to_keys.end() || name_it->second.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  std::vector<std::shared_ptr<const TpchQ21NationRows>> nations;
  if (!LoadNationRows(services, name_it->second, nations)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitMerged(nations, sink);
}

void TpchQ21Module::ExecuteFallback(core::AuxEngineServices &services,
                                     const core::QueryArguments &arguments,
                                     core::AuxRowSink &sink) {
  std::string nation_name;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload)) {
    nation_name = std::get<std::string>(arguments.values[0].payload);
  }
  const auto full_state = BuildState(services);
  std::vector<std::shared_ptr<const TpchQ21NationRows>> nations;
  const auto name_it = full_state.nation_index->nation_name_to_keys.find(nation_name);
  if (name_it != full_state.nation_index->nation_name_to_keys.end()) {
    for (const auto nk : name_it->second) {
      const auto it = full_state.nations.find(nk);
      if (it != full_state.nations.end())
        nations.push_back(it->second);
    }
  }
  EmitMerged(nations, sink);
}

namespace {

// Order keys in hash shards: the fulfilled orders, or the per-supplier
// lineitem counts (an OrderSummary) of the orders seen. The shards are the
// coarsest level of the final order map's shard hash, so a task owning one
// build shard writes only its own shards of that map.
struct OrderShards {
  static constexpr unsigned kBits = 6;
  static constexpr std::size_t kShards = std::size_t{1} << kBits;
  using Shard = core::FlatHashMap<OrderSummary>;
  std::array<Shard, kShards> shards;

  static std::size_t ShardOf(int64_t orderkey) {
    return core::ShardedCowHashMap<OrderSummary, 7>::ShardOf(orderkey, kBits);
  }
  void Fulfilled(int64_t orderkey) { shards[ShardOf(orderkey)][orderkey].orderstatus = 'F'; }
  bool IsFulfilled(int64_t orderkey) const { return shards[ShardOf(orderkey)].Contains(orderkey); }
  void Add(const TpchQ21LineitemInput &input) {
    auto &summary = shards[ShardOf(input.orderkey)][input.orderkey];
    summary.orderstatus = 'F';
    AddSupplier(summary, input.suppkey, 1, input.receiptdate > input.commitdate ? 1 : 0);
  }
  static void AddSupplier(OrderSummary &summary, int64_t suppkey, int64_t total, int64_t late) {
    if (auto *info = summary.FindSupplier(suppkey)) {
      info->total_count += total;
      info->late_count += late;
    } else {
      summary.suppliers.push_back({suppkey, total, late});
    }
  }
};

// Folds `from` into `into`, one pool task per run of shards.
void FoldShards(const core::AuxEngineServices &services, OrderShards &into, OrderShards &&from) {
  core::ParallelSlices(services, OrderShards::kShards, 1,
                       [&](std::size_t, std::size_t first, std::size_t last) {
                         for (std::size_t s = first; s < last; ++s)
                           for (auto [orderkey, theirs] : from.shards[s]) {
                             auto &mine = into.shards[s][orderkey];
                             if (theirs.orderstatus)
                               mine.orderstatus = theirs.orderstatus;
                             for (const auto &info : theirs.suppliers)
                               OrderShards::AddSupplier(mine, info.suppkey, info.total_count,
                                                        info.late_count);
                           }
                       });
}

} // namespace

TpchQ21State
TpchQ21Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ21State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q21 aux requires a typed initial scan source");

  // Suppliers.
  using Suppliers = std::unordered_map<int64_t, TpchQ21Supplier>;
  state.dir->base = std::make_shared<const Suppliers>(core::PartitionedBuild<Suppliers>(
      services, [] { return Suppliers{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanSuppliers(services, parts,
                                    [&](std::size_t p, const TpchQ21SupplierInput &input) {
                                      sink(p, TypedRowBatch<TpchQ21SupplierInput>{input});
                                    });
      },
      [](Suppliers &partial, const TpchQ21SupplierInput &input) {
        partial[input.suppkey] = {std::make_shared<const std::string>(input.name),
                                  input.nationkey};
      },
      [](Suppliers &into, Suppliers &&from) {
        for (auto &[suppkey, supplier] : from)
          into[suppkey] = std::move(supplier);
      }));

  // Nations.
  initial_scan->ScanNations([&](const TpchQ21NationInput &input) {
    state.nation_index->nation_name_to_keys[input.name].push_back(
        input.nationkey);
  });

  // Fulfilled orders, then their per-supplier lineitem counts; both in hash
  // shards so partial states fold shard by shard in parallel.
  const auto fulfilled = core::PartitionedBuild<OrderShards>(
      services, [] { return OrderShards{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(services, parts,
                                 [&](std::size_t p, const TpchQ21OrderInput &input) {
                                   sink(p, TypedRowBatch<TpchQ21OrderInput>{input});
                                 });
      },
      [](OrderShards &partial, const TpchQ21OrderInput &input) {
        if (input.orderstatus == 'F')
          partial.Fulfilled(input.orderkey);
      },
      [&](OrderShards &into, OrderShards &&from) { FoldShards(services, into, std::move(from)); });
  auto summaries = core::PartitionedBuild<OrderShards>(
      services, [] { return OrderShards{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ21LineitemInput &input) {
                                      sink(p, TypedRowBatch<TpchQ21LineitemInput>{input});
                                    });
      },
      [&](OrderShards &partial, const TpchQ21LineitemInput &input) {
        if (fulfilled.IsFulfilled(input.orderkey))
          partial.Add(input);
      },
      [&](OrderShards &into, OrderShards &&from) { FoldShards(services, into, std::move(from)); });

  // Contributions: count the contributing orders and their numwait, then copy
  // exactly those into the final map, shard by shard. The copies are fresh,
  // exactly-sized allocations: the build's per-order scratch (most of it for
  // orders that do not contribute) frees without leaving the state scattered
  // through half-used heap pages.
  struct Contributions {
    std::unordered_map<int64_t, int64_t> numwait;
    std::size_t orders = 0;
  };
  Contributions totals;
  core::ParallelReduce(
      services, OrderShards::kShards, 1, totals,
      [&](Contributions &acc, std::size_t first, std::size_t last) {
        for (std::size_t s = first; s < last; ++s)
          for (auto [orderkey, order] : summaries.shards[s]) {
            (void)orderkey;
            int64_t suppkey = 0, count = 0;
            if (!ComputeOrderContribution(order, suppkey, count))
              continue;
            ++acc.orders;
            if (count != 0)
              acc.numwait[suppkey] += count;
          }
      },
      [](Contributions &into, Contributions &&from) {
        into.orders += from.orders;
        for (const auto &[suppkey, nw] : from.numwait)
          into.numwait[suppkey] += nw;
      });
  const auto &numwait = totals.numwait;
  state.core = std::make_shared<TpchQ21Core>(
      std::max(OrderShards::kBits,
               core::ShardedCowHashMap<OrderSummary, 7>::ShardBitsFor(totals.orders)),
      core::ShardedCowHashMap<int64_t, 7>::ShardBitsFor(numwait.size()));
  core::ParallelSlices(services, OrderShards::kShards, 1,
                       [&](std::size_t, std::size_t first, std::size_t last) {
                         for (std::size_t s = first; s < last; ++s)
                           for (auto [orderkey, order] : summaries.shards[s]) {
                             int64_t suppkey = 0, count = 0;
                             if (!ComputeOrderContribution(order, suppkey, count))
                               continue;
                             auto &kept = state.core->orders[orderkey];
                             kept.orderstatus = order.orderstatus;
                             kept.suppliers.assign(order.suppliers.begin(), order.suppliers.end());
                           }
                       });
  for (const auto &[suppkey, nw] : numwait)
    state.core->numwait[suppkey] = nw;

  // Per-nation answer rows.
  std::map<int32_t, std::map<std::string,
                             std::pair<std::shared_ptr<const std::string>, int64_t>>>
      per_nation;
  for (const auto &[suppkey, nw] : numwait) {
    if (nw <= 0)
      continue;
    const auto *supplier = state.dir->Find(suppkey);
    if (!supplier)
      continue;
    auto &slot = per_nation[supplier->nationkey][*supplier->name];
    if (!slot.first)
      slot.first = supplier->name;
    slot.second += nw;
  }
  std::vector<decltype(per_nation)::iterator> groups;
  for (auto it = per_nation.begin(); it != per_nation.end(); ++it)
    groups.push_back(it);
  std::vector<std::shared_ptr<TpchQ21NationRows>> nation_rows(groups.size());
  services.ParallelFor(groups.size(), [&](std::size_t i) {
    auto rows = std::make_shared<TpchQ21NationRows>();
    rows->rows.reserve(groups[i]->second.size());
    for (auto &[name, entry] : groups[i]->second) {
      (void)name;
      rows->rows.push_back({entry.first, entry.second});
    }
    std::sort(rows->rows.begin(), rows->rows.end(), ImageRowBefore);
    nation_rows[i] = std::move(rows);
  });
  for (std::size_t i = 0; i < groups.size(); ++i)
    state.nations[groups[i]->first] = std::move(nation_rows[i]);
  return state;
}

bool TpchQ21Module::LoadNationRows(
    core::AuxEngineServices &services, const std::vector<int32_t> &nationkeys,
    std::vector<std::shared_ptr<const TpchQ21NationRows>> &out) const {
  std::vector<core::PartitionId> partitions;
  for (const auto nk : nationkeys)
    partitions.push_back(NationPartition(nk));
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  for (const auto &image : snapshot.partitions) {
    if (image.partition < TpchQ21Layout::kNationBase)
      continue;
    const auto nk = static_cast<int32_t>(image.partition -
                                         TpchQ21Layout::kNationBase);
    const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
    if (!typed || typed->nationkey != nk)
      return false;
    out.push_back(typed->rows);
  }
  // A nation without a published image has no waiters.
  return true;
}

void TpchQ21Module::EmitMerged(
    const std::vector<std::shared_ptr<const TpchQ21NationRows>> &nations,
    core::AuxRowSink &sink) const {
  sink.Begin(core::OutputSchema{});
  if (nations.size() == 1) {
    const auto &rows = nations.front()->rows;
    const auto n = std::min(rows.size(), kTpchQ21Limit);
    for (std::size_t i = 0; i < n; ++i)
      EmitRow(sink, *rows[i].name, rows[i].numwait);
    sink.End();
    return;
  }
  // Several nations share the name: merge rows by s_name across them.
  std::map<std::string, int64_t> by_name;
  for (const auto &n : nations)
    for (const auto &row : n->rows)
      by_name[*row.name] += row.numwait;
  std::vector<TpchQ21OutputRow> output_rows;
  output_rows.reserve(by_name.size());
  for (auto &[name, numwait] : by_name)
    output_rows.push_back({name, numwait});
  std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
  if (output_rows.size() > kTpchQ21Limit)
    output_rows.resize(kTpchQ21Limit);
  for (const auto &row : output_rows)
    EmitRow(sink, row.name, row.numwait);
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ21Module::CalibrationWrites() const {
  return TpchQ21CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
