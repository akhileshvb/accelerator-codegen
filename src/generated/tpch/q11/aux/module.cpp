#include "tpch/q11/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
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
                Q11AccT total_value,
                const std::vector<TpchQ11OutputRow> &sorted_rows) {
  auto image = std::make_shared<TpchQ11StateImage>();
  image->kind = TpchQ11ImageKind::NationPreAggregated;
  image->nationkey = nationkey;
  image->nation_name = nation_name;
  image->total_value = total_value;
  image->sorted_rows = sorted_rows;
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

void ApplyPartsuppContribution(TpchQ11State &state,
                               const TpchQ11PartsuppInput &input) {
  Q11AccT product = 0;
  AddAcc(product, input.supplycost_raw, input.availqty);
  auto &contrib = state.supplier_part_contributions[input.suppkey][input.partkey];
  contrib.value += product;
  contrib.row_count++;
}

void ApplySignedPartValue(std::vector<TpchQ11OutputRow> &sorted_rows,
                          Q11AccT &total_value, int64_t partkey,
                          Q11AccT signed_value) {
  total_value += signed_value;
  auto cmp = [](const TpchQ11OutputRow &lhs, const TpchQ11OutputRow &rhs) {
    if (lhs.value != rhs.value)
      return lhs.value > rhs.value;
    return lhs.partkey < rhs.partkey;
  };
  auto it = std::find_if(sorted_rows.begin(), sorted_rows.end(),
                         [&](const TpchQ11OutputRow &row) {
                           return row.partkey == partkey;
                         });
  if (it != sorted_rows.end()) {
    it->value += signed_value;
    if (it->value <= 0) {
      sorted_rows.erase(it);
      return;
    }
    const TpchQ11OutputRow updated = *it;
    sorted_rows.erase(it);
    const auto insert_pos =
        std::lower_bound(sorted_rows.begin(), sorted_rows.end(), updated, cmp);
    sorted_rows.insert(insert_pos, updated);
    return;
  }
  if (signed_value <= 0)
    return;
  const TpchQ11OutputRow row{partkey, signed_value};
  const auto insert_pos =
      std::lower_bound(sorted_rows.begin(), sorted_rows.end(), row, cmp);
  sorted_rows.insert(insert_pos, row);
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

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ11Metadata(const TpchQ11State &state) {
  ByteWriter writer;

  uint32_t nation_count = static_cast<uint32_t>(state.nation_name_to_key.size());
  uint32_t supplier_count = static_cast<uint32_t>(state.supplier_to_nation.size());

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
    const auto nk = state.nation_name_to_key.at(name);
    const auto it = state.nationkey_to_suppliers.find(nk);
    if (it != state.nationkey_to_suppliers.end()) {
      writer.U32(static_cast<uint32_t>(it->second.size()));
      for (const auto sk : it->second)
        writer.I64(sk);
    } else {
      writer.U32(0);
    }
  }

  std::vector<int64_t> supplier_keys;
  for (const auto &entry : state.supplier_to_nation)
    supplier_keys.push_back(entry.first);
  std::sort(supplier_keys.begin(), supplier_keys.end());

  writer.U32(supplier_count);
  for (const auto sk : supplier_keys) {
    writer.I64(sk);
    writer.I32(state.supplier_to_nation.at(sk));
  }

  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ11NationPartition(
    int32_t nationkey, const std::string &nation_name,
    Q11AccT total_value,
    const std::vector<TpchQ11OutputRow> &sorted_rows) {
  ByteWriter writer;
  uint32_t count = static_cast<uint32_t>(sorted_rows.size());
  core::WriteImageHeader(writer, kNationMagic, kVersion, count);

  writer.String(nation_name);
  WriteQ11Acc(writer, total_value);

  for (const auto &row : sorted_rows) {
    writer.I64(row.partkey);
    WriteQ11Acc(writer, row.value);
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
  state.nationkey_to_suppliers.clear();
  state.supplier_to_nation.clear();

  uint32_t nation_count = 0;
  if (!reader.U32(nation_count))
    return false;
  for (uint32_t i = 0; i < nation_count; ++i) {
    std::string name;
    int32_t nk = 0;
    uint32_t sk_count = 0;
    if (!reader.String(name) || !reader.I32(nk) || !reader.U32(sk_count))
      return false;
    state.nation_name_to_key.emplace(std::move(name), nk);
    std::vector<int64_t> suppkeys;
    suppkeys.reserve(sk_count);
    for (uint32_t j = 0; j < sk_count; ++j) {
      int64_t sk = 0;
      if (!reader.I64(sk))
        return false;
      suppkeys.push_back(sk);
    }
    state.nationkey_to_suppliers.emplace(nk, std::move(suppkeys));
  }

  uint32_t supplier_count = 0;
  if (!reader.U32(supplier_count))
    return false;
  for (uint32_t i = 0; i < supplier_count; ++i) {
    int64_t sk = 0;
    int32_t nk = 0;
    if (!reader.I64(sk) || !reader.I32(nk))
      return false;
    state.supplier_to_nation.emplace(sk, nk);
  }

  return reader.offset == bytes.size();
}

bool DecodeTpchQ11NationPartition(
    const std::vector<uint8_t> &bytes,
    int32_t &nationkey, std::string &nation_name,
    Q11AccT &total_value,
    std::vector<TpchQ11OutputRow> &sorted_rows) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kNationMagic, kVersion, count))
    return false;

  if (!reader.String(nation_name))
    return false;
  if (!ReadQ11Acc(reader, total_value))
    return false;

  sorted_rows.clear();
  sorted_rows.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ11OutputRow row;
    if (!reader.I64(row.partkey) || !ReadQ11Acc(reader, row.value))
      return false;
    sorted_rows.push_back(row);
  }

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
  return {{config.partsupp_table, {0, 1, 2, 3}},
          {config.supplier_table, {0, 1}},
          {config.nation_table, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ11Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ11TxnBuffer>();
}

void TpchQ11Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ11State>(state);

  builder.Write(core::StateImage::Make(
      TpchQ11Layout::kAccelerator, TpchQ11Layout::kMetadata, 0,
      MakeMetadataImage(typed_state)));

  // Write per-nation and FNV-1a nation-name partitions
  for (const auto &[nk, sorted_rows] : state.nation_sorted_rows) {
    Q11AccT total = 0;
    auto tit = state.nation_total_value.find(nk);
    if (tit != state.nation_total_value.end())
      total = tit->second;

    // Find nation name for collision guard
    std::string name;
    for (const auto &[nm, key] : state.nation_name_to_key) {
      if (key == nk) { name = nm; break; }
    }

    const auto image = MakeNationImage(nk, name, total, sorted_rows);

    // Write FNV-1a nation-name partition (fast path)
    if (!name.empty()) {
      builder.Write(core::StateImage::Make(
          TpchQ11Layout::kAccelerator, NationNamePartition(name), 0, image));
    }
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
        buffer.AddSupplierDelta({suppkey, nationkey, 0});
      } else {
        buffer.AddSupplierDelta({suppkey, 0, nationkey});
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
        buffer.AddSupplierDelta({old_sk, old_nk, new_nk});
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

  if (!buffer.SupplierDeltas().empty() || !buffer.NationDeltas().empty()) {
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

  const auto visibility = services.CurrentVisibility();
  const auto metadata_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ11Layout::kMetadata});
  const TpchQ11State *metadata = nullptr;
  for (const auto &image : metadata_snapshot.partitions) {
    if (image.partition != TpchQ11Layout::kMetadata)
      continue;
    const auto *typed = AsQ11Image(image, TpchQ11ImageKind::Metadata);
    metadata = typed ? typed->state.get() : nullptr;
    break;
  }
  if (!metadata) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  std::unordered_map<int32_t, std::string> nation_names;
  for (const auto &entry : metadata->nation_name_to_key)
    nation_names.emplace(entry.second, entry.first);

  std::unordered_map<int32_t, std::vector<const TpchQ11PartsuppDelta *>>
      deltas_by_nation;
  std::set<int32_t> touched_nations;
  for (const auto &delta : buffer.PartsuppDeltas()) {
    const auto supplier_it = metadata->supplier_to_nation.find(delta.suppkey);
    if (supplier_it == metadata->supplier_to_nation.end()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    const int32_t nationkey = supplier_it->second;
    touched_nations.insert(nationkey);
    deltas_by_nation[nationkey].push_back(&delta);
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.PartsuppDeltas().size()),
       static_cast<uint64_t>(touched_nations.size()), true, true, true, false,
       false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  std::vector<core::PartitionId> nation_partitions;
  nation_partitions.reserve(touched_nations.size());
  for (const auto nationkey : touched_nations) {
    const auto name_it = nation_names.find(nationkey);
    if (name_it == nation_names.end()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    nation_partitions.push_back(NationNamePartition(name_it->second));
  }

  std::unordered_map<int32_t, core::Version> versions;
  std::unordered_map<int32_t, std::vector<TpchQ11OutputRow>> sorted_rows;
  std::unordered_map<int32_t, Q11AccT> total_values;
  std::unordered_map<int32_t, std::string> partition_names;

  const auto nation_snapshot =
      services.StateStore().ReadPartitions(visibility, nation_partitions);
  for (const auto &image : nation_snapshot.partitions) {
    const auto *typed =
        AsQ11Image(image, TpchQ11ImageKind::NationPreAggregated);
    if (!typed || typed->nation_name.empty()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    versions.emplace(typed->nationkey, image.version);
    sorted_rows.emplace(typed->nationkey, typed->sorted_rows);
    total_values.emplace(typed->nationkey, typed->total_value);
    partition_names.emplace(typed->nationkey, typed->nation_name);
  }
  if (versions.size() != touched_nations.size()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  for (const auto nationkey : touched_nations) {
    auto rows_it = sorted_rows.find(nationkey);
    auto total_it = total_values.find(nationkey);
    if (rows_it == sorted_rows.end() || total_it == total_values.end()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    for (const auto *delta : deltas_by_nation[nationkey]) {
      const Q11AccT signed_value =
          static_cast<Q11AccT>(delta->sign) * delta->value;
      ApplySignedPartValue(rows_it->second, total_it->second, delta->partkey,
                           signed_value);
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  for (const auto nationkey : touched_nations) {
    const auto name_it = partition_names.find(nationkey);
    const auto version_it = versions.find(nationkey);
    const auto rows_it = sorted_rows.find(nationkey);
    const auto total_it = total_values.find(nationkey);
    if (name_it == partition_names.end() || version_it == versions.end() ||
        rows_it == sorted_rows.end() || total_it == total_values.end()) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    const auto image = MakeNationImage(nationkey, name_it->second,
                                       total_it->second, rows_it->second);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ11Layout::kAccelerator, NationNamePartition(name_it->second),
        version_it->second, image));
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
    for (const auto &image : fast_snapshot.partitions) {
      const auto *typed = AsQ11Image(image, TpchQ11ImageKind::NationPreAggregated);
      if (!typed) continue;
      // Collision guard: verify stored nation_name matches query
      if (typed->nation_name != nation_name) continue;

      // Compute threshold in integer domain:
      //   row.value / 100.0 > (total / 100.0) * fraction
      // ⇔ row.value > total * fraction   (100.0 cancels)
      // Convert total to double once, multiply by fraction, cast back.
      Q11AccT threshold_raw = 0;
#if defined(__SIZEOF_INT128__)
      {
        double total_as_double = static_cast<double>(static_cast<int64_t>(
            static_cast<uint64_t>(typed->total_value)));
        threshold_raw = static_cast<Q11AccT>(total_as_double * fraction);
      }
#else
      threshold_raw = static_cast<Q11AccT>(
          static_cast<double>(typed->total_value) * fraction);
#endif

      // Inline emission: iterate pre-sorted rows, compare in integer domain
      sink.Begin(core::OutputSchema{});
      core::AuxRow aux_row;
      aux_row.values.reserve(2);
      for (const auto &row : typed->sorted_rows) {
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
          break; // sorted by value DESC
        }
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

  const auto state = BuildState(services);

  auto name_it = state.nation_name_to_key.find(nation_name);
  if (name_it == state.nation_name_to_key.end()) {
    EmitResult({}, sink);
    return;
  }
  const int32_t nationkey = name_it->second;

  auto nk_it = state.nation_sorted_rows.find(nationkey);
  if (nk_it == state.nation_sorted_rows.end()) {
    EmitResult({}, sink);
    return;
  }

  Q11AccT total_value = 0;
  auto tot_it = state.nation_total_value.find(nationkey);
  if (tot_it != state.nation_total_value.end())
    total_value = tot_it->second;

  auto output = FinalizeQ11FromSorted(nk_it->second, total_value, fraction);
  EmitResult(output, sink);
}

TpchQ11State TpchQ11Module::BuildState(
    core::AuxEngineServices &services) const {
  (void)services;
  TpchQ11State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q11 requires a typed initial scan source");

  initial_scan->ScanNations([&](const TpchQ11NationInput &input) {
    state.nation_name_to_key[input.name] = input.nationkey;
  });

  initial_scan->ScanSuppliers([&](const TpchQ11SupplierInput &input) {
    state.supplier_to_nation[input.suppkey] = input.nationkey;
    state.nationkey_to_suppliers[input.nationkey].push_back(input.suppkey);
  });

  initial_scan->ScanPartsupp([&](const TpchQ11PartsuppInput &input) {
    ApplyPartsuppContribution(state, input);
  });

  // Pre-aggregate per-nation: merge supplier contributions into per-nation maps
  std::unordered_map<int32_t, std::unordered_map<int64_t, Q11PartContribution>>
      nation_merged;
  std::unordered_map<int32_t, Q11AccT> nation_total;

  for (const auto &[suppkey, contribs] : state.supplier_part_contributions) {
    auto nk_it = state.supplier_to_nation.find(suppkey);
    if (nk_it == state.supplier_to_nation.end())
      continue;
    const int32_t nk = nk_it->second;
    auto &merged = nation_merged[nk];
    Q11AccT &total = nation_total[nk];
    for (const auto &[pk, contrib] : contribs) {
      auto &m = merged[pk];
      m.value += contrib.value;
      m.row_count += contrib.row_count;
      total += contrib.value;
    }
  }

  // Pre-sort per-nation: sort by value DESC, partkey ASC
  for (auto &[nk, merged] : nation_merged) {
    std::vector<TpchQ11OutputRow> sorted;
    sorted.reserve(merged.size());
    for (const auto &[pk, contrib] : merged)
      sorted.push_back({pk, contrib.value});
    std::sort(sorted.begin(), sorted.end(),
              [](const TpchQ11OutputRow &lhs, const TpchQ11OutputRow &rhs) {
                if (lhs.value != rhs.value)
                  return lhs.value > rhs.value;
                return lhs.partkey < rhs.partkey;
              });
    state.nation_sorted_rows[nk] = std::move(sorted);
  }
  state.nation_total_value = std::move(nation_total);

  return state;
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

} // namespace accelerator_codegen::aux_state::query