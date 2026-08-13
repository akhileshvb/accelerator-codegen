#include "tpch/q21/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kSupplierMagic = 0x53323151; // Q21S
constexpr uint32_t kOrdersMagic = 0x4f323151;   // Q21O
constexpr uint32_t kNationMagic = 0x4e323151;   // Q21N
constexpr uint32_t kNationNameIdxMagic = 0x49323151; // Q21I

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

std::shared_ptr<const TpchQ21StateImage>
MakeStateImage(TpchQ21ImageKind kind, int32_t nationkey,
               const std::shared_ptr<const TpchQ21State> &state,
               const std::string &nation_name = "") {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->kind = kind;
  image->nationkey = nationkey;
  image->nation_name = nation_name;
  image->state = state;
  if (kind == TpchQ21ImageKind::Nation) {
    const auto nation_it = state->nation_numwait.find(nationkey);
    if (nation_it != state->nation_numwait.end()) {
      image->output_rows.reserve(nation_it->second.size());
      for (const auto &[suppkey, numwait] : nation_it->second) {
        if (numwait <= 0)
          continue;
        const auto name_it = state->supplier_names.find(suppkey);
        if (name_it != state->supplier_names.end())
          image->output_rows.push_back({name_it->second, numwait});
      }
      std::sort(image->output_rows.begin(), image->output_rows.end(),
                [](const TpchQ21OutputRow &lhs, const TpchQ21OutputRow &rhs) {
                  if (lhs.numwait != rhs.numwait)
                    return lhs.numwait > rhs.numwait;
                  return lhs.name < rhs.name;
                });
    }
  }
  return image;
}

const TpchQ21StateImage *AsQ21Image(const core::StateImage &image,
                                     TpchQ21ImageKind kind) {
  const auto *payload = image.As<TpchQ21StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
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

bool BetterOutputRow(const TpchQ21OutputRow &lhs, const TpchQ21OutputRow &rhs) {
  if (lhs.numwait != rhs.numwait)
    return lhs.numwait > rhs.numwait;
  return lhs.name < rhs.name;
}

// ── Contribution computation ────────────────────────────────────────────────
// Reduce the two correlated EXISTS predicates to per-order sufficient facts:
// the number of participating suppliers and the number (and identity) of
// suppliers with a late item.  Q21 can contribute only if exactly one supplier
// is late and at least one different supplier participates.  Keeping this
// reduction linear is important on build and touched-order repair paths.
std::unordered_map<int64_t, int64_t>
ComputeOrderContributions(const OrderSummary &order) {
  std::unordered_map<int64_t, int64_t> contributions;
  if (order.orderstatus != 'F' || order.suppliers.size() <= 1)
    return contributions;

  int64_t sole_late_suppkey = 0;
  int64_t sole_late_count = 0;
  uint32_t late_supplier_count = 0;
  for (const auto &[suppkey, info] : order.suppliers) {
    if (info.total_count <= 0 || info.late_count <= 0)
      continue;
    ++late_supplier_count;
    if (late_supplier_count > 1)
      return contributions; // NOT EXISTS fails for every candidate.
    sole_late_suppkey = suppkey;
    sole_late_count = info.late_count;
  }

  if (late_supplier_count == 1)
    contributions.emplace(sole_late_suppkey, sole_late_count);
  return contributions;
}

// ── Mutation helpers ────────────────────────────────────────────────────────
bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey, int64_t &suppkey,
                  int32_t &commitdate, int32_t &receiptdate) {
  return IntCell(row, 0, orderkey) && IntCell(row, 1, suppkey) &&
         DateCell(row, 2, commitdate) && DateCell(row, 3, receiptdate);
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey, std::string &name,
                  int64_t &nationkey_raw) {
  int64_t nk = 0;
  if (!IntCell(row, 0, suppkey) || !StringCell(row, 1, name) ||
      !IntCell(row, 2, nk))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey_raw = nk;
  return true;
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, char &orderstatus) {
  return IntCell(row, 0, orderkey) && CharCell(row, 1, orderstatus);
}

bool ReadNation(const core::AuxRow &row, int64_t &nationkey_raw,
                std::string &name) {
  int64_t nk = 0;
  if (!IntCell(row, 0, nk) || !StringCell(row, 1, name))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey_raw = nk;
  return true;
}

// ── Order summary rebuild from initial scan ─────────────────────────────────
OrderSummary BuildOrderSummary(int64_t orderkey,
                                TpchQ21InitialScan *scan) {
  OrderSummary summary;
  summary.orderstatus = 0;
  bool have_status = false;
  scan->ScanOrders([&](const TpchQ21OrderInput &input) {
    if (input.orderkey != orderkey) return;
    summary.orderstatus = input.orderstatus;
    have_status = true;
  });
  if (!have_status) return summary;

  scan->ScanLineitems([&](const TpchQ21LineitemInput &input) {
    if (input.orderkey != orderkey) return;
    auto &info = summary.suppliers[input.suppkey];
    info.total_count++;
    if (input.receiptdate > input.commitdate)
      info.late_count++;
  });
  return summary;
}

// ── Apply contribution deltas to state ──────────────────────────────────────
void ApplyContributionDeltas(
    TpchQ21State &state,
    const std::unordered_map<int64_t, int64_t> &old_contribs,
    const std::unordered_map<int64_t, int64_t> &new_contribs) {
  std::set<int64_t> affected_suppkeys;
  for (const auto &[sk, _] : old_contribs) affected_suppkeys.insert(sk);
  for (const auto &[sk, _] : new_contribs) affected_suppkeys.insert(sk);

  for (const auto suppkey : affected_suppkeys) {
    const auto old_val = old_contribs.count(suppkey) ? old_contribs.at(suppkey) : 0;
    const auto new_val = new_contribs.count(suppkey) ? new_contribs.at(suppkey) : 0;
    const int64_t delta = new_val - old_val;
    if (delta == 0) continue;

    const auto nation_it = state.supplier_nations.find(suppkey);
    if (nation_it == state.supplier_nations.end()) continue;
    const int32_t nationkey = nation_it->second;

    auto &nation_map = state.nation_numwait[nationkey];
    nation_map[suppkey] += delta;
    if (nation_map[suppkey] == 0)
      nation_map.erase(suppkey);
    if (nation_map.empty())
      state.nation_numwait.erase(nationkey);
  }
}

} // namespace

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ21SupplierMetadata(const TpchQ21State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kSupplierMagic, kVersion,
                         static_cast<uint32_t>(state.supplier_names.size()));
  std::vector<int64_t> keys;
  for (const auto &entry : state.supplier_names)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  for (const auto key : keys) {
    writer.I64(key);
    writer.String(state.supplier_names.at(key));
    const auto nation_it = state.supplier_nations.find(key);
    writer.I32(nation_it != state.supplier_nations.end() ? nation_it->second : 0);
  }
  // Also encode nation_name_to_keys for backward compatibility
  std::vector<std::string> nation_names;
  for (const auto &entry : state.nation_name_to_keys)
    nation_names.push_back(entry.first);
  std::sort(nation_names.begin(), nation_names.end());
  writer.U32(static_cast<uint32_t>(nation_names.size()));
  for (const auto &name : nation_names) {
    writer.String(name);
    const auto &nkeys = state.nation_name_to_keys.at(name);
    writer.U32(static_cast<uint32_t>(nkeys.size()));
    for (const auto nk : nkeys)
      writer.I32(nk);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ21OrderSummaries(const TpchQ21State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kOrdersMagic, kVersion,
                         static_cast<uint32_t>(state.order_summaries.size()));
  std::vector<int64_t> keys;
  for (const auto &entry : state.order_summaries)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  for (const auto key : keys) {
    const auto &summary = state.order_summaries.at(key);
    writer.I64(key);
    writer.U8(static_cast<uint8_t>(summary.orderstatus));
    writer.U32(static_cast<uint32_t>(summary.suppliers.size()));
    for (const auto &[suppkey, info] : summary.suppliers) {
      writer.I64(suppkey);
      writer.I64(info.total_count);
      writer.I64(info.late_count);
    }
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ21NationNameIndex(const TpchQ21State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kNationNameIdxMagic, kVersion,
                         static_cast<uint32_t>(state.nation_name_to_keys.size()));
  std::vector<std::string> nation_names;
  for (const auto &entry : state.nation_name_to_keys)
    nation_names.push_back(entry.first);
  std::sort(nation_names.begin(), nation_names.end());
  for (const auto &name : nation_names) {
    writer.String(name);
    const auto &nkeys = state.nation_name_to_keys.at(name);
    writer.U32(static_cast<uint32_t>(nkeys.size()));
    for (const auto nk : nkeys)
      writer.I32(nk);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ21Nation(int32_t nationkey,
                                          const TpchQ21State &state) {
  ByteWriter writer;
  const auto it = state.nation_numwait.find(nationkey);
  uint32_t count = 0;
  std::vector<int64_t> keys;
  if (it != state.nation_numwait.end()) {
    for (const auto &[suppkey, nw] : it->second) {
      if (nw != 0)
        keys.push_back(suppkey);
    }
    std::sort(keys.begin(), keys.end());
    count = static_cast<uint32_t>(keys.size());
  }
  core::WriteImageHeader(writer, kNationMagic, kVersion, count);
  for (const auto suppkey : keys) {
    writer.I64(suppkey);
    writer.I64(it->second.at(suppkey));
    // Include supplier name for direct output
    const auto name_it = state.supplier_names.find(suppkey);
    writer.String(name_it != state.supplier_names.end() ? name_it->second : "");
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ21SupplierMetadata(const std::vector<uint8_t> &bytes,
                                    TpchQ21State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kSupplierMagic, kVersion, count))
    return false;
  state.supplier_names.clear();
  state.supplier_nations.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    std::string name;
    int32_t nationkey = 0;
    if (!reader.I64(key) || !reader.String(name) || !reader.I32(nationkey))
      return false;
    state.supplier_names.emplace(key, std::move(name));
    state.supplier_nations.emplace(key, nationkey);
  }
  // Read nation_name_to_keys for backward compat (may be absent in old format)
  uint32_t name_count = 0;
  if (!reader.U32(name_count))
    return reader.offset == bytes.size(); // old format without nation names
  state.nation_name_to_keys.clear();
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
    state.nation_name_to_keys.emplace(std::move(name), std::move(nkeys));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ21OrderSummaries(const std::vector<uint8_t> &bytes,
                                  TpchQ21State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kOrdersMagic, kVersion, count))
    return false;
  state.order_summaries.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint8_t status = 0;
    uint32_t supplier_count = 0;
    if (!reader.I64(key) || !reader.U8(status) || !reader.U32(supplier_count))
      return false;
    OrderSummary summary;
    summary.orderstatus = static_cast<char>(status);
    for (uint32_t j = 0; j < supplier_count; ++j) {
      int64_t suppkey = 0, total = 0, late = 0;
      if (!reader.I64(suppkey) || !reader.I64(total) || !reader.I64(late))
        return false;
      summary.suppliers[suppkey] = {total, late};
    }
    state.order_summaries.emplace(key, std::move(summary));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ21NationNameIndex(const std::vector<uint8_t> &bytes,
                                   TpchQ21State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kNationNameIdxMagic, kVersion, count))
    return false;
  state.nation_name_to_keys.clear();
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
    state.nation_name_to_keys.emplace(std::move(name), std::move(nkeys));
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ21Nation(const std::vector<uint8_t> &bytes, int32_t nationkey,
                          TpchQ21State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kNationMagic, kVersion, count))
    return false;
  auto &nation_map = state.nation_numwait[nationkey];
  nation_map.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t suppkey = 0, numwait = 0;
    std::string name;
    if (!reader.I64(suppkey) || !reader.I64(numwait) || !reader.String(name))
      return false;
    if (numwait != 0)
      nation_map[suppkey] = numwait;
    if (!name.empty() && state.supplier_names.find(suppkey) == state.supplier_names.end())
      state.supplier_names.emplace(suppkey, std::move(name));
  }
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
  supplier_deltas.push_back(delta);
}
void TpchQ21TxnBuffer::AddNationDelta(TpchQ21NationDelta delta) {
  pending = true;
  nation_deltas.push_back(delta);
}

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ21Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ21Module::Dependencies() const {
  return {{config.supplier_table, {0, 1, 2}},
          {config.orders_table, {0, 1}},
          {config.lineitem_table, {0, 1, 2, 3}},
          {config.nation_table, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ21Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ21TxnBuffer>();
}

void TpchQ21Module::Build(core::AuxEngineServices &services,
                           core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ21State>(state);

  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kSupplierMetadata, 0,
      MakeStateImage(TpchQ21ImageKind::SupplierMetadata, 0, typed_state)));
  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kOrderSummaries, 0,
      MakeStateImage(TpchQ21ImageKind::OrderSummaries, 0, typed_state)));
  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kNationNameIndex, 0,
      MakeStateImage(TpchQ21ImageKind::NationNameIndex, 0, typed_state)));
  // Persist NationPartition images so StageMaintenance can load/apply deltas
  // without publishing empty NationNamePartition hits after the first write.
  for (const auto &[nk, _] : state.nation_numwait) {
    (void)_;
    builder.Write(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationPartition(nk), 0,
        MakeStateImage(TpchQ21ImageKind::Nation, nk, typed_state)));
  }
  // Materialize the result at the query-parameter grain.  This enables the
  // fast read path: one FNV-1a-hashed partition read directly from the nation
  // name, with collision detection via the stored nation_name.
  for (const auto &[name, nationkeys] : state.nation_name_to_keys) {
    if (nationkeys.size() != 1)
      continue; // uncommon duplicate names retain the correctness slow path.
    const int32_t nk = nationkeys.front();
    builder.Write(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationNamePartition(name), 0,
        MakeStateImage(TpchQ21ImageKind::Nation, nk, typed_state, name)));
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
    buffer.MarkRebuild();
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
        buffer.AddOrderDelta({old_orderkey, old_status, new_status});
      }
      break;
    }
    return;
  }

  if (mutation.table == config.nation_table) {
    auto add = [&](const core::AuxRow &row, bool is_old) {
      int64_t nationkey_raw = 0;
      std::string name;
      if (!ReadNation(row, nationkey_raw, name)) {
        buffer.MarkRebuild();
        return;
      }
      if (is_old) {
        buffer.AddNationDelta({static_cast<int32_t>(nationkey_raw), name, ""});
      } else {
        buffer.AddNationDelta({static_cast<int32_t>(nationkey_raw), "", name});
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
        int64_t old_nk = 0, new_nk = 0;
        std::string old_name, new_name;
        if (!ReadNation(mutation.old_rows[i], old_nk, old_name) ||
            !ReadNation(mutation.new_rows[i], new_nk, new_name)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddNationDelta({static_cast<int32_t>(old_nk), old_name, new_name});
      }
      break;
    }
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
      {static_cast<uint64_t>(total_deltas), 1, true, true, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Load current state from side partitions
  const auto visibility = services.CurrentVisibility();
  TpchQ21State state;

  const auto side_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ21Layout::kSupplierMetadata,
                   TpchQ21Layout::kOrderSummaries,
                   TpchQ21Layout::kNationNameIndex});
  bool have_supp = false, have_orders = false, have_nameidx = false;
  for (const auto &image : side_snapshot.partitions) {
    if (image.partition == TpchQ21Layout::kSupplierMetadata) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::SupplierMetadata);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      state.supplier_names = typed->state->supplier_names;
      state.supplier_nations = typed->state->supplier_nations;
      have_supp = true;
    }
    if (image.partition == TpchQ21Layout::kOrderSummaries) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::OrderSummaries);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      state.order_summaries = typed->state->order_summaries;
      have_orders = true;
    }
    if (image.partition == TpchQ21Layout::kNationNameIndex) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::NationNameIndex);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      state.nation_name_to_keys = typed->state->nation_name_to_keys;
      have_nameidx = true;
    }
  }
  if (!have_supp || !have_orders || !have_nameidx) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Track which nation partitions we need to load
  std::set<int32_t> needed_nationkeys;
  std::set<int64_t> affected_orderkeys;

  // Handle supplier deltas
  for (const auto &delta : supplier_deltas) {
    const auto nation_it = state.supplier_nations.find(delta.suppkey);
    if (nation_it != state.supplier_nations.end())
      needed_nationkeys.insert(nation_it->second);
    needed_nationkeys.insert(delta.old_nationkey);
    needed_nationkeys.insert(delta.new_nationkey);
  }

  // Collect order status changes (deferred: apply inside the order loop)
  std::unordered_map<int64_t, std::pair<char, char>> order_status_changes;
  for (const auto &delta : order_deltas) {
    affected_orderkeys.insert(delta.orderkey);
    if (delta.old_orderstatus != delta.new_orderstatus) {
      order_status_changes[delta.orderkey] = {delta.old_orderstatus, delta.new_orderstatus};
      const auto summary_it = state.order_summaries.find(delta.orderkey);
      if (summary_it == state.order_summaries.end())
        continue;
      for (const auto &[suppkey, _] : summary_it->second.suppliers) {
        const auto nation_it = state.supplier_nations.find(suppkey);
        if (nation_it != state.supplier_nations.end())
          needed_nationkeys.insert(nation_it->second);
      }
    }
  }

  // Handle lineitem deltas: group by orderkey
  std::unordered_map<int64_t, std::vector<TpchQ21LineitemDelta>> deltas_by_order;
  for (const auto &delta : lineitem_deltas) {
    deltas_by_order[delta.orderkey].push_back(delta);
    affected_orderkeys.insert(delta.orderkey);
  }

  // Pre-compute needed nationkeys from all affected orders (before applying deltas)
  for (const auto orderkey : affected_orderkeys) {
    const auto summary_it = state.order_summaries.find(orderkey);
    if (summary_it == state.order_summaries.end())
      continue;
    for (const auto &[suppkey, _] : summary_it->second.suppliers) {
      const auto nation_it = state.supplier_nations.find(suppkey);
      if (nation_it != state.supplier_nations.end())
        needed_nationkeys.insert(nation_it->second);
    }
    auto it = deltas_by_order.find(orderkey);
    if (it != deltas_by_order.end()) {
      for (const auto &delta : it->second) {
        const auto nation_it = state.supplier_nations.find(delta.suppkey);
        if (nation_it != state.supplier_nations.end())
          needed_nationkeys.insert(nation_it->second);
      }
    }
  }

  // Load needed nation partitions BEFORE processing orders
  if (!needed_nationkeys.empty()) {
    std::vector<core::PartitionId> nation_partitions;
    for (const auto nk : needed_nationkeys)
      nation_partitions.push_back(NationPartition(nk));
    const auto nation_snapshot = services.StateStore().ReadPartitions(
        visibility, nation_partitions);
    std::set<int32_t> loaded_nations;
    for (const auto &image : nation_snapshot.partitions) {
      if (image.partition < TpchQ21Layout::kNationBase)
        continue;
      const auto nk = static_cast<int32_t>(
          image.partition - TpchQ21Layout::kNationBase);
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
      if (!typed || typed->nationkey != nk) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      const auto nk_it = typed->state->nation_numwait.find(nk);
      if (nk_it != typed->state->nation_numwait.end()) {
        state.nation_numwait[nk] = nk_it->second;
        loaded_nations.insert(nk);
      }
    }
    // Missing nation images would publish empty NationNamePartition hits.
    for (const auto nk : needed_nationkeys) {
      if (loaded_nations.find(nk) == loaded_nations.end()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
    }
  }

  // For each affected order, recompute contributions
  for (const auto orderkey : affected_orderkeys) {
    auto summary_it = state.order_summaries.find(orderkey);
    if (summary_it == state.order_summaries.end()) {
      // Unknown order: cannot apply a local delta fairly — rebuild.
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    auto &summary = summary_it->second;

    // Compute old contributions BEFORE applying any changes
    const auto old_contribs = ComputeOrderContributions(summary);

    // Apply order status change if any
    auto status_it = order_status_changes.find(orderkey);
    if (status_it != order_status_changes.end()) {
      summary.orderstatus = status_it->second.second; // new_status
    }

    // Apply lineitem deltas
    auto it = deltas_by_order.find(orderkey);
    if (it != deltas_by_order.end()) {
      for (const auto &delta : it->second) {
        auto &info = summary.suppliers[delta.suppkey];
        info.total_count += delta.sign;
        if (delta.receiptdate > delta.commitdate)
          info.late_count += delta.sign;
        // Ensure we track the nation of affected suppliers
        const auto nation_it = state.supplier_nations.find(delta.suppkey);
        if (nation_it != state.supplier_nations.end())
          needed_nationkeys.insert(nation_it->second);
        // Clean up zero-count suppliers
        if (info.total_count <= 0 && info.late_count <= 0)
          summary.suppliers.erase(delta.suppkey);
      }
    }

    // Compute new contributions
    const auto new_contribs = ComputeOrderContributions(summary);

    // Apply deltas to state
    ApplyContributionDeltas(state, old_contribs, new_contribs);

    // Track nations of all affected suppliers
    for (const auto &[suppkey, _] : old_contribs) {
      const auto nation_it = state.supplier_nations.find(suppkey);
      if (nation_it != state.supplier_nations.end())
        needed_nationkeys.insert(nation_it->second);
    }
    for (const auto &[suppkey, _] : new_contribs) {
      const auto nation_it = state.supplier_nations.find(suppkey);
      if (nation_it != state.supplier_nations.end())
        needed_nationkeys.insert(nation_it->second);
    }
  }

  // Handle nation deltas (rename)
  for (const auto &delta : nation_deltas) {
    if (!delta.old_name.empty()) {
      auto &name_vec = state.nation_name_to_keys[delta.old_name];
      name_vec.erase(std::remove(name_vec.begin(), name_vec.end(), delta.nationkey),
                     name_vec.end());
      if (name_vec.empty())
        state.nation_name_to_keys.erase(delta.old_name);
    }
    if (!delta.new_name.empty()) {
      state.nation_name_to_keys[delta.new_name].push_back(delta.nationkey);
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto shared_state = std::make_shared<const TpchQ21State>(std::move(state));

  // Publish updated side partitions
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kSupplierMetadata, 0,
      MakeStateImage(TpchQ21ImageKind::SupplierMetadata, 0, shared_state)));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kOrderSummaries, 0,
      MakeStateImage(TpchQ21ImageKind::OrderSummaries, 0, shared_state)));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kNationNameIndex, 0,
      MakeStateImage(TpchQ21ImageKind::NationNameIndex, 0, shared_state)));

  // Build nationkey -> name reverse lookup
  std::unordered_map<int32_t, std::string> nk_to_name;
  for (const auto &[name, nkeys] : shared_state->nation_name_to_keys) {
    for (const auto nk : nkeys) {
      if (nk_to_name.find(nk) == nk_to_name.end())
        nk_to_name[nk] = name;
    }
  }

  // Publish updated nation partitions + corresponding NationNamePartition entries
  // Only republish nations we actually loaded/updated — never invent empty hits.
  std::set<int32_t> published_nations;
  for (const auto nk : needed_nationkeys)
    published_nations.insert(nk);
  for (const auto nk : published_nations) {
    if (shared_state->nation_numwait.find(nk) ==
        shared_state->nation_numwait.end()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ21Layout::kAccelerator, NationPartition(nk), 0,
        MakeStateImage(TpchQ21ImageKind::Nation, nk, shared_state)));
    // Also update the NationNamePartition entry so the single-read fast path stays fresh
    auto name_it = nk_to_name.find(nk);
    if (name_it != nk_to_name.end()) {
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ21Layout::kAccelerator, NationNamePartition(name_it->second), 0,
          MakeStateImage(TpchQ21ImageKind::Nation, nk, shared_state, name_it->second)));
    }
  }

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
  // Try the NationNamePartition first.  On a hit the image already carries
  // pre-sorted output_rows; a valid hit needs only one bounded partition
  // fetch with zero decode/reconstruct work.
  {
    const auto fast_snapshot = services.StateStore().ReadPartitions(
        visibility, {NationNamePartition(nation_name)});
    for (const auto &image : fast_snapshot.partitions) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
      if (!typed) continue;
      // Collision guard: verify the stored nation_name matches the query.
      if (typed->nation_name != nation_name) continue;
      // Empty output_rows on the name partition can be a stale/missing nation
      // image after maintenance — fall through to the NationPartition path
      // rather than treating an empty hit as authoritative when the nation
      // key is known to the index.
      if (!typed->output_rows.empty()) {
        sink.Begin(core::OutputSchema{});
        for (const auto &row : typed->output_rows) {
          core::AuxRow out;
          out.values.reserve(2);
          out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
          out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.numwait));
          sink.WriteRow(out);
        }
        sink.End();
        return;
      }
      break;
    }
  }

  // ── Slow path: resolve nation name via the tiny NationNameIndex ────────
  // This avoids decoding the full supplier metadata; only the ~25-entry
  // nation_name → nationkeys map is read.
  const auto idx_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ21Layout::kNationNameIndex});
  const TpchQ21State *idx_state = nullptr;
  for (const auto &image : idx_snapshot.partitions) {
    if (image.partition == TpchQ21Layout::kNationNameIndex) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::NationNameIndex);
      if (typed) idx_state = typed->state.get();
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

  const auto &nationkeys = name_it->second;
  if (nationkeys.size() != 1) {
    // Preserve the general (non-TPC-H) duplicate-name semantics on the slow
    // path rather than silently dropping a partition.
    TpchQ21State state;
    if (!LoadStateForNations(services, nationkeys, state)) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    EmitResult(state, sink);
    return;
  }

  const int32_t nationkey = nationkeys.front();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {NationPartition(nationkey)});
  const TpchQ21StateImage *nation_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == NationPartition(nationkey)) {
      nation_image = AsQ21Image(image, TpchQ21ImageKind::Nation);
      break;
    }
  }

  sink.Begin(core::OutputSchema{});
  if (nation_image && nation_image->nationkey == nationkey) {
    for (const auto &row : nation_image->output_rows) {
      core::AuxRow out;
      out.values.reserve(2);
      out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
      out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.numwait));
      sink.WriteRow(out);
    }
  }
  sink.End();
}

void TpchQ21Module::ExecuteFallback(core::AuxEngineServices &services,
                                     const core::QueryArguments &arguments,
                                     core::AuxRowSink &sink) {
  std::string nation_name;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload)) {
    nation_name = std::get<std::string>(arguments.values[0].payload);
  }
  // Build full state and filter by nation_name
  auto full_state = BuildState(services);
  // Filter to only matching nations
  TpchQ21State filtered_state;
  const auto name_it = full_state.nation_name_to_keys.find(nation_name);
  if (name_it != full_state.nation_name_to_keys.end()) {
    for (const auto nk : name_it->second) {
      const auto nk_it = full_state.nation_numwait.find(nk);
      if (nk_it != full_state.nation_numwait.end())
        filtered_state.nation_numwait[nk] = nk_it->second;
    }
  }
  filtered_state.supplier_names = std::move(full_state.supplier_names);
  EmitResult(filtered_state, sink);
}

TpchQ21State
TpchQ21Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ21State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q21 aux requires a typed initial scan source");

  // Scan suppliers
  initial_scan->ScanSuppliers([&](const TpchQ21SupplierInput &input) {
    state.supplier_names[input.suppkey] = input.name;
    state.supplier_nations[input.suppkey] = input.nationkey;
  });

  // Scan nations
  initial_scan->ScanNations([&](const TpchQ21NationInput &input) {
    state.nation_name_to_keys[input.name].push_back(input.nationkey);
  });

  // Scan orders
  initial_scan->ScanOrders([&](const TpchQ21OrderInput &input) {
    state.order_summaries[input.orderkey].orderstatus = input.orderstatus;
  });

  // Scan lineitems and build order summaries
  initial_scan->ScanLineitems([&](const TpchQ21LineitemInput &input) {
    auto it = state.order_summaries.find(input.orderkey);
    if (it == state.order_summaries.end() || it->second.orderstatus != 'F')
      return;
    auto &summary = it->second;
    auto &info = summary.suppliers[input.suppkey];
    info.total_count++;
    if (input.receiptdate > input.commitdate)
      info.late_count++;
  });

  // Compute contributions for fulfilled orders only.
  for (auto &[orderkey, summary] : state.order_summaries) {
    if (summary.orderstatus != 'F')
      continue;
    const auto contribs = ComputeOrderContributions(summary);
    for (const auto &[suppkey, count] : contribs) {
      if (count == 0) continue;
      const auto nation_it = state.supplier_nations.find(suppkey);
      if (nation_it == state.supplier_nations.end()) continue;
      state.nation_numwait[nation_it->second][suppkey] += count;
    }
  }

  for (auto it = state.order_summaries.begin();
       it != state.order_summaries.end();) {
    if (it->second.orderstatus != 'F' || it->second.suppliers.size() <= 1 ||
        ComputeOrderContributions(it->second).empty()) {
      it = state.order_summaries.erase(it);
    } else {
      ++it;
    }
  }

  return state;
}

bool TpchQ21Module::LoadStateForNations(
    core::AuxEngineServices &services,
    const std::vector<int32_t> &nationkeys,
    TpchQ21State &state) const {
  std::vector<core::PartitionId> partitions;
  for (const auto nk : nationkeys)
    partitions.push_back(NationPartition(nk));
  // Also load supplier names
  partitions.push_back(TpchQ21Layout::kSupplierMetadata);

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);

  bool have_names = false;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ21Layout::kSupplierMetadata) {
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::SupplierMetadata);
      if (!typed) return false;
      state.supplier_names = typed->state->supplier_names;
      have_names = true;
      continue;
    }
    if (image.partition >= TpchQ21Layout::kNationBase) {
      const auto nk = static_cast<int32_t>(
          image.partition - TpchQ21Layout::kNationBase);
      const auto *typed = AsQ21Image(image, TpchQ21ImageKind::Nation);
      if (!typed || typed->nationkey != nk) return false;
      const auto nk_it = typed->state->nation_numwait.find(nk);
      if (nk_it != typed->state->nation_numwait.end())
        state.nation_numwait[nk] = nk_it->second;
    }
  }
  return have_names;
}

void TpchQ21Module::EmitResult(const TpchQ21State &state,
                                core::AuxRowSink &sink) const {
  std::vector<TpchQ21OutputRow> output_rows;

  for (const auto &[nationkey, supp_map] : state.nation_numwait) {
    for (const auto &[suppkey, numwait] : supp_map) {
      if (numwait <= 0) continue;
      const auto name_it = state.supplier_names.find(suppkey);
      if (name_it == state.supplier_names.end()) continue;
      TpchQ21OutputRow row;
      row.name = name_it->second;
      row.numwait = numwait;
      output_rows.push_back(std::move(row));
    }
  }

  std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(2);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.numwait));
    sink.WriteRow(out);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query