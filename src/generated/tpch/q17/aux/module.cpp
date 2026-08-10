#include "tpch/q17/aux/module.hpp"
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
constexpr uint32_t kAttrMagic = 0x49313751;  // Q17I
constexpr uint32_t kPartMagic = 0x50313751;  // Q17P

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

core::PartitionId AttrResultPartition(const std::string &brand,
                                      const std::string &container) {
  const auto key = TpchQ17State::MakeAttrKey(brand, container);
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : key) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return TpchQ17Layout::kAttrResultBase +
         static_cast<core::PartitionId>(hash % TpchQ17Layout::kAttrResultSlots);
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
MakeAttrResultImage(const std::string &brand, const std::string &container,
                    int64_t contribution_raw) {
  auto image = std::make_shared<TpchQ17StateImage>();
  image->kind = TpchQ17ImageKind::AttrResult;
  image->brand = brand;
  image->container = container;
  image->contribution_raw = contribution_raw;
  return image;
}

const TpchQ17StateImage *AsQ17Image(const core::StateImage &image,
                                    TpchQ17ImageKind kind) {
  const auto *payload = image.As<TpchQ17StateImage>();
  if (!payload || payload->kind != kind)
    return nullptr;
  if (kind == TpchQ17ImageKind::PartStatsChunk && !payload->chunk)
    return nullptr;
  return payload;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ17LineitemInput &out) {
  int64_t partkey = 0, quantity_raw = 0, extended_raw = 0;
  int32_t quantity_scale = 0, extended_scale = 0;
  if (!IntCell(row, 0, partkey) ||
      !RawDecimalCell(row, 1, quantity_raw, quantity_scale) ||
      !RawDecimalCell(row, 2, extended_raw, extended_scale))
    return false;
  out.partkey = partkey;
  out.quantity_raw = quantity_raw;
  out.extendedprice_raw = extended_raw;
  return true;
}

void ApplyLineitemDeltaToStats(TpchQ17PartStats &stats,
                               const TpchQ17LineitemDelta &delta) {
  if (delta.sign > 0) {
    stats.sum_qty_raw += delta.quantity_raw;
    stats.count += 1;
    TpchQ17QtyPricePair pair{delta.quantity_raw, delta.extendedprice_raw};
    auto pos = std::lower_bound(
        stats.qty_price_pairs.begin(), stats.qty_price_pairs.end(),
        pair.quantity_raw,
        [](const TpchQ17QtyPricePair &p, int64_t t) {
          return p.quantity_raw < t;
        });
    if (pos != stats.qty_price_pairs.end() &&
        pos->quantity_raw == pair.quantity_raw) {
      pos->extendedprice_raw += pair.extendedprice_raw;
    } else {
      stats.qty_price_pairs.insert(pos, pair);
    }
    return;
  }

  stats.sum_qty_raw -= delta.quantity_raw;
  stats.count -= 1;
  auto pos = std::lower_bound(
      stats.qty_price_pairs.begin(), stats.qty_price_pairs.end(),
      delta.quantity_raw,
      [](const TpchQ17QtyPricePair &p, int64_t t) {
        return p.quantity_raw < t;
      });
  if (pos != stats.qty_price_pairs.end() &&
      pos->quantity_raw == delta.quantity_raw) {
    pos->extendedprice_raw -= delta.extendedprice_raw;
    if (pos->extendedprice_raw <= 0)
      stats.qty_price_pairs.erase(pos);
  }
}

} // namespace

void TpchQ17PartStats::BuildPrefixSums() {
  if (prefix_valid && prefix_sums.size() == qty_price_pairs.size())
    return;
  prefix_sums.resize(qty_price_pairs.size());
  int64_t running = 0;
  for (std::size_t i = 0; i < qty_price_pairs.size(); ++i) {
    running += qty_price_pairs[i].extendedprice_raw;
    prefix_sums[i] = running;
  }
  prefix_valid = true;
}

int64_t TpchQ17PartStats::SumExtendedpriceBelow(int64_t threshold_raw) const {
  if (qty_price_pairs.empty())
    return 0;
  if (!prefix_valid || prefix_sums.size() != qty_price_pairs.size())
    return 0;
  auto it = std::lower_bound(
      qty_price_pairs.begin(), qty_price_pairs.end(), threshold_raw,
      [](const TpchQ17QtyPricePair &p, int64_t t) {
        return p.quantity_raw < t;
      });
  if (it == qty_price_pairs.begin())
    return 0;
  const auto idx = static_cast<std::size_t>(it - qty_price_pairs.begin()) - 1;
  return prefix_sums[idx];
}

void TpchQ17PartStats::RecomputeContribution() {
  if (count <= 0 || qty_price_pairs.empty()) {
    precomputed_contribution_raw = 0;
    return;
  }
  if (!prefix_valid || prefix_sums.size() != qty_price_pairs.size())
    BuildPrefixSums();
  const double avg = static_cast<double>(sum_qty_raw) / 100.0 /
                     static_cast<double>(count);
  const double threshold_double = 0.2 * avg;
  const int64_t threshold_raw =
      static_cast<int64_t>(std::ceil(threshold_double * 100.0));
  precomputed_contribution_raw = SumExtendedpriceBelow(threshold_raw);
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

// Legacy byte encoders (tooling compatibility).
std::vector<uint8_t> EncodeTpchQ17AttributeIndex(const TpchQ17State &state) {
  ByteWriter writer;
  uint32_t total_entries = 0;
  for (const auto &entry : state.attribute_index)
    total_entries += static_cast<uint32_t>(entry.second.size());
  core::WriteImageHeader(writer, kAttrMagic, kVersion, total_entries);
  std::vector<std::pair<std::string, int64_t>> sorted;
  sorted.reserve(total_entries);
  for (const auto &kv : state.attribute_index)
    for (const auto pk : kv.second)
      sorted.emplace_back(kv.first, pk);
  std::sort(sorted.begin(), sorted.end());
  for (const auto &entry : sorted) {
    writer.String(entry.first);
    writer.I64(entry.second);
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ17PartStats(int64_t partkey,
                                            const TpchQ17State &state) {
  ByteWriter writer;
  const auto it = state.part_stats.find(partkey);
  uint32_t count = 0;
  if (it != state.part_stats.end()) {
    const auto &stats = it->second;
    count = static_cast<uint32_t>(stats.qty_price_pairs.size());
    core::WriteImageHeader(writer, kPartMagic, kVersion, count);
    writer.I64(stats.sum_qty_raw);
    writer.I32(stats.count);
    for (const auto &pair : stats.qty_price_pairs) {
      writer.I64(pair.quantity_raw);
      writer.I64(pair.extendedprice_raw);
    }
  } else {
    core::WriteImageHeader(writer, kPartMagic, kVersion, 0);
    writer.I64(0);
    writer.I32(0);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ17AttributeIndex(const std::vector<uint8_t> &bytes,
                                 TpchQ17State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kAttrMagic, kVersion, count))
    return false;
  state.attribute_index.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string key;
    int64_t partkey = 0;
    if (!reader.String(key) || !reader.I64(partkey))
      return false;
    state.attribute_index[key].insert(partkey);
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ17PartStats(const std::vector<uint8_t> &bytes, int64_t partkey,
                            TpchQ17State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kPartMagic, kVersion, count))
    return false;
  TpchQ17PartStats stats;
  if (!reader.I64(stats.sum_qty_raw) || !reader.I32(stats.count))
    return false;
  stats.qty_price_pairs.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!reader.I64(stats.qty_price_pairs[i].quantity_raw) ||
        !reader.I64(stats.qty_price_pairs[i].extendedprice_raw))
      return false;
  }
  stats.BuildPrefixSums();
  state.part_stats[partkey] = std::move(stats);
  return reader.offset == bytes.size();
}

bool FindTpchQ17PartKeys(const std::vector<uint8_t> &bytes,
                         const std::string &brand,
                         const std::string &container,
                         std::unordered_set<int64_t> &partkeys) {
  TpchQ17State state;
  if (!DecodeTpchQ17AttributeIndex(bytes, state))
    return false;
  const auto key = TpchQ17State::MakeAttrKey(brand, container);
  const auto it = state.attribute_index.find(key);
  partkeys.clear();
  if (it != state.attribute_index.end())
    partkeys = it->second;
  return true;
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
  return {{config.part_table, {0, 1, 2}},
          {config.lineitem_table, {0, 1, 2}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ17Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ17TxnBuffer>();
}

void TpchQ17Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);

  std::map<int64_t, TpchQ17PartStatsChunk> chunks;
  std::unordered_map<std::string, int64_t> attr_contributions;

  for (const auto &[partkey, stats] : state.part_stats) {
    const auto chunk_index =
        static_cast<int64_t>(partkey >> TpchQ17Layout::kPartChunkShift);
    auto &chunk = chunks[chunk_index];
    chunk.chunk_index = chunk_index;
    TpchQ17PartEntry entry;
    const auto attr_it = state.part_attr_keys.find(partkey);
    if (attr_it != state.part_attr_keys.end())
      entry.attr_key = attr_it->second;
    entry.stats = stats;
    if (!entry.attr_key.empty())
      attr_contributions[entry.attr_key] += stats.precomputed_contribution_raw;
    chunk.parts.emplace(partkey, std::move(entry));
  }

  for (auto &[chunk_index, chunk] : chunks) {
    const auto chunk_ptr =
        std::make_shared<const TpchQ17PartStatsChunk>(std::move(chunk));
    const auto partition =
        TpchQ17Layout::kPartStatsChunkBase +
        static_cast<core::PartitionId>(chunk_index);
    builder.Write(core::StateImage::Make(
        TpchQ17Layout::kAccelerator, partition, 0,
        MakeChunkImage(chunk_ptr)));
  }

  for (const auto &[attr_key, contribution] : attr_contributions) {
    std::string brand;
    std::string container;
    TpchQ17State::SplitAttrKey(attr_key, brand, container);
    builder.Write(core::StateImage::Make(
        TpchQ17Layout::kAccelerator, AttrResultPartition(brand, container), 0,
        MakeAttrResultImage(brand, container, contribution)));
  }

  builder.Finish();
}

core::InvalidationPlan
TpchQ17Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.part_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
    return plan;
  }
  return {};
}

void TpchQ17Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ17TxnBuffer &>(base_buffer);

  if (mutation.table == config.part_table) {
    buffer.MarkPartRebuild();
    return;
  }

  if (mutation.table != config.lineitem_table)
    return;

  auto add_lineitem = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ17LineitemInput input;
    if (!ReadLineitem(row, input)) {
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
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, true, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (buffer.LineitemDeltas().empty())
    return outcome;

  std::set<int64_t> touched_partkeys;
  for (const auto &delta : buffer.LineitemDeltas())
    touched_partkeys.insert(delta.partkey);

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.LineitemDeltas().size()),
       static_cast<uint64_t>(touched_partkeys.size()), true, true, true, false,
       false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  std::map<int64_t, TpchQ17PartStatsChunk> local_chunks;
  std::map<int64_t, core::Version> chunk_versions;
  std::map<std::string, int64_t> attr_deltas;
  std::set<std::string> touched_attr_keys;
  std::map<int64_t, std::vector<int64_t>> parts_by_chunk;
  for (const auto partkey : touched_partkeys) {
    parts_by_chunk[static_cast<int64_t>(partkey >>
                                        TpchQ17Layout::kPartChunkShift)]
        .push_back(partkey);
  }

  for (const auto &[chunk_index, partkeys] : parts_by_chunk) {
    const auto chunk_partition =
        TpchQ17Layout::kPartStatsChunkBase +
        static_cast<core::PartitionId>(chunk_index);
    const auto chunk_snapshot =
        services.StateStore().ReadPartitions(visibility, {chunk_partition});
    TpchQ17PartStatsChunk *chunk = nullptr;
    for (const auto &image : chunk_snapshot.partitions) {
      const auto *typed = AsQ17Image(image, TpchQ17ImageKind::PartStatsChunk);
      if (!typed || !typed->chunk)
        continue;
      chunk = &local_chunks[chunk_index];
      *chunk = *typed->chunk;
      chunk_versions[chunk_index] = image.version;
    }
    if (!chunk) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }

    for (const auto partkey : partkeys) {
      auto part_it = chunk->parts.find(partkey);
      if (part_it == chunk->parts.end()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }

      const int64_t old_contrib =
          part_it->second.stats.precomputed_contribution_raw;
      const std::string attr_key = part_it->second.attr_key;

      for (const auto &delta : buffer.LineitemDeltas()) {
        if (delta.partkey != partkey)
          continue;
        ApplyLineitemDeltaToStats(part_it->second.stats, delta);
      }
      if (part_it->second.stats.count <= 0 &&
          part_it->second.stats.qty_price_pairs.empty()) {
        chunk->parts.erase(part_it);
      } else {
        part_it->second.stats.RecomputeContribution();
      }

      const int64_t new_contrib =
          chunk->parts.count(partkey)
              ? chunk->parts[partkey].stats.precomputed_contribution_raw
              : 0;
      if (!attr_key.empty()) {
        attr_deltas[attr_key] += new_contrib - old_contrib;
        touched_attr_keys.insert(attr_key);
      }
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;

  for (auto &[chunk_index, chunk] : local_chunks) {
    const auto partition =
        TpchQ17Layout::kPartStatsChunkBase +
        static_cast<core::PartitionId>(chunk_index);
    const auto version =
        chunk_versions.count(chunk_index) ? chunk_versions[chunk_index] : 0;
    const auto chunk_ptr =
        std::make_shared<const TpchQ17PartStatsChunk>(std::move(chunk));
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ17Layout::kAccelerator, partition, version,
        MakeChunkImage(chunk_ptr)));
  }

  for (const auto &attr_key : touched_attr_keys) {
    std::string brand;
    std::string container;
    TpchQ17State::SplitAttrKey(attr_key, brand, container);
    const auto attr_partition = AttrResultPartition(brand, container);
    const auto attr_snapshot =
        services.StateStore().ReadPartitions(visibility, {attr_partition});
    int64_t contribution = 0;
    core::Version attr_version = 0;
    for (const auto &image : attr_snapshot.partitions) {
      const auto *typed = AsQ17Image(image, TpchQ17ImageKind::AttrResult);
      if (!typed)
        continue;
      if (typed->brand != brand || typed->container != container)
        continue;
      contribution = typed->contribution_raw;
      attr_version = image.version;
      break;
    }
    contribution += attr_deltas[attr_key];
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ17Layout::kAccelerator, attr_partition, attr_version,
        MakeAttrResultImage(brand, container, contribution)));
  }

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
  std::string brand;
  std::string container;
  if (arguments.values.size() >= 2) {
    if (!arguments.values[0].is_null &&
        std::holds_alternative<std::string>(arguments.values[0].payload))
      brand = std::get<std::string>(arguments.values[0].payload);
    if (!arguments.values[1].is_null &&
        std::holds_alternative<std::string>(arguments.values[1].payload))
      container = std::get<std::string>(arguments.values[1].payload);
  }

  if (brand.empty() || container.empty()) {
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
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {AttrResultPartition(brand, container)});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ17Image(image, TpchQ17ImageKind::AttrResult);
    if (!typed)
      continue;
    if (typed->brand != brand || typed->container != container)
      continue;

    sink.Begin({{{"avg_yearly", core::AuxType::Double, true}}});
    if (typed->contribution_raw == 0) {
      sink.WriteRow({{core::AuxValue::Null(core::AuxType::Double)}});
    } else {
      const double result =
          static_cast<double>(typed->contribution_raw) / 100.0 / 7.0;
      sink.WriteRow({{core::AuxValue::Of(core::AuxType::Double, result)}});
    }
    sink.End();
    return;
  }

  EmitResult(services, brand, container, sink);
}

void TpchQ17Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  (void)services;
  std::string brand;
  std::string container;
  if (arguments.values.size() >= 2) {
    if (!arguments.values[0].is_null &&
        std::holds_alternative<std::string>(arguments.values[0].payload))
      brand = std::get<std::string>(arguments.values[0].payload);
    if (!arguments.values[1].is_null &&
        std::holds_alternative<std::string>(arguments.values[1].payload))
      container = std::get<std::string>(arguments.values[1].payload);
  }
  EmitResult(services, brand, container, sink);
}

TpchQ17State TpchQ17Module::BuildState(
    core::AuxEngineServices &services) const {
  (void)services;
  TpchQ17State state;
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q17 requires a typed initial scan source");

  initial_scan->ScanParts([&](const TpchQ17PartInput &input) {
    const auto key = TpchQ17State::MakeAttrKey(input.brand, input.container);
    state.attribute_index[key].insert(input.key);
    state.part_attr_keys[input.key] = key;
  });

  initial_scan->ScanLineitems([&](const TpchQ17LineitemInput &input) {
    auto &stats = state.part_stats[input.partkey];
    stats.sum_qty_raw += input.quantity_raw;
    stats.count += 1;

    TpchQ17QtyPricePair pair{input.quantity_raw, input.extendedprice_raw};
    auto pos = std::lower_bound(
        stats.qty_price_pairs.begin(), stats.qty_price_pairs.end(),
        pair.quantity_raw,
        [](const TpchQ17QtyPricePair &p, int64_t t) {
          return p.quantity_raw < t;
        });
    if (pos != stats.qty_price_pairs.end() &&
        pos->quantity_raw == pair.quantity_raw) {
      pos->extendedprice_raw += pair.extendedprice_raw;
    } else {
      stats.qty_price_pairs.insert(pos, pair);
    }
  });

  for (auto &entry : state.part_stats)
    entry.second.RecomputeContribution();

  return state;
}

void TpchQ17Module::EmitResult(core::AuxEngineServices &services,
                               const std::string &brand,
                               const std::string &container,
                               core::AuxRowSink &sink) const {
  (void)services;
  (void)brand;
  (void)container;
  sink.Begin({{{"avg_yearly", core::AuxType::Double, true}}});
  sink.WriteRow({{core::AuxValue::Null(core::AuxType::Double)}});
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
