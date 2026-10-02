#include "tpch/q17/duckdb/aux/module.hpp"
#include "tpch/q17/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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
    stats.prefix_valid = false;
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
  stats.prefix_valid = false;
}

void UpsertAttrContribution(TpchQ17AttrParts &parts, int64_t partkey,
                              int64_t contribution_raw) {
  for (auto &entry : parts.contributions) {
    if (entry.partkey != partkey)
      continue;
    if (contribution_raw == 0) {
      entry = parts.contributions.back();
      parts.contributions.pop_back();
    } else {
      entry.precomputed_contribution_raw = contribution_raw;
    }
    return;
  }
  if (contribution_raw != 0)
    parts.contributions.push_back({partkey, contribution_raw});
}

int64_t SumAttrContributions(const TpchQ17AttrParts &parts) {
  int64_t total = 0;
  for (const auto &entry : parts.contributions)
    total += entry.precomputed_contribution_raw;
  return total;
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

int64_t QuantityThresholdRaw(int64_t sum_qty_raw, int32_t count) {
  if (count <= 0)
    return 0;
  const double avg = static_cast<double>(sum_qty_raw) / 100.0 /
                     static_cast<double>(count);
  return static_cast<int64_t>(std::ceil(0.2 * avg * 100.0));
}

void TpchQ17PartStats::RecomputeContribution() {
  if (count <= 0 || qty_price_pairs.empty()) {
    precomputed_contribution_raw = 0;
    return;
  }
  if (!prefix_valid || prefix_sums.size() != qty_price_pairs.size())
    BuildPrefixSums();
  precomputed_contribution_raw =
      SumExtendedpriceBelow(QuantityThresholdRaw(sum_qty_raw, count));
}

void StripTpchQ17PartStatsForPublish(TpchQ17PartStats &stats) {
  stats.qty_price_pairs.clear();
  stats.qty_price_pairs.shrink_to_fit();
  stats.prefix_sums.clear();
  stats.prefix_sums.shrink_to_fit();
  stats.prefix_valid = false;
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
  return {{config.part_table, {0, 1, 2}, {0, 3, 6}},
          {config.lineitem_table, {0, 1, 2}, {1, 4, 5}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ17Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ17TxnBuffer>();
}

void TpchQ17Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);

  std::map<int64_t, TpchQ17PartStatsChunk> chunks;
  std::unordered_map<std::string, TpchQ17AttrParts> attr_parts_by_key;

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
    if (!entry.attr_key.empty()) {
      auto &parts = attr_parts_by_key[entry.attr_key];
      TpchQ17State::SplitAttrKey(entry.attr_key, parts.brand, parts.container);
      parts.contributions.push_back(
          {partkey, stats.precomputed_contribution_raw});
    }
    StripTpchQ17PartStatsForPublish(entry.stats);
    chunk.parts.emplace(partkey, std::move(entry));
  }

  for (auto &[chunk_index, chunk] : chunks) {
    const auto chunk_ptr =
        std::make_shared<const TpchQ17PartStatsChunk>(std::move(chunk));
    const auto partition =
        TpchQ17Layout::kPartStatsChunkBase +
        static_cast<core::PartitionId>(chunk_index);
    builder.Write(core::StateImage::Make(
        TpchQ17Layout::kAccelerator, partition, 0, MakeChunkImage(chunk_ptr)));
  }

  for (auto &[attr_key, parts] : attr_parts_by_key) {
    (void)attr_key;
    std::sort(parts.contributions.begin(), parts.contributions.end(),
              [](const TpchQ17AttrContribution &lhs,
                 const TpchQ17AttrContribution &rhs) {
                return lhs.partkey < rhs.partkey;
              });
    const auto parts_ptr =
        std::make_shared<const TpchQ17AttrParts>(std::move(parts));
    builder.Write(core::StateImage::Make(
        TpchQ17Layout::kAccelerator,
        AttrPartsPartition(parts_ptr->brand, parts_ptr->container), 0,
        MakeAttrPartsImage(parts_ptr)));
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
        {0, 0, false, true, false, true, false,
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

  std::size_t touched_chunks = 0;
  {
    std::set<int64_t> chunks;
    for (const auto partkey : touched_partkeys)
      chunks.insert(static_cast<int64_t>(partkey >> TpchQ17Layout::kPartChunkShift));
    touched_chunks = chunks.size();
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.LineitemDeltas().size()),
       static_cast<uint64_t>(std::max<std::size_t>(1, touched_chunks)),
       true, true, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    if (!initial_scan) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    const auto visibility = services.CurrentVisibility();
    std::map<int64_t, TpchQ17PartStatsChunk> local_chunks;
    std::set<std::string> touched_attr_keys;
    std::map<std::string, TpchQ17AttrParts> local_attr_parts;
    std::map<int64_t, std::vector<int64_t>> parts_by_chunk;
    std::unordered_set<int64_t> rebuild_keys;
    for (const auto partkey : touched_partkeys) {
      parts_by_chunk[static_cast<int64_t>(partkey >>
                                          TpchQ17Layout::kPartChunkShift)]
          .push_back(partkey);
      rebuild_keys.insert(partkey);
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
      }
      if (!chunk) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      for (const auto partkey : partkeys) {
        auto part_it = chunk->parts.find(partkey);
        if (part_it == chunk->parts.end()) {
          core::FinishRebuildAll(outcome);
          return outcome;
        }
        if (!part_it->second.attr_key.empty())
          touched_attr_keys.insert(part_it->second.attr_key);
      }
    }
    auto rebuilt = RebuildPartStatsBatch(rebuild_keys);
    for (auto &[chunk_index, partkeys] : parts_by_chunk) {
      auto chunk_it = local_chunks.find(chunk_index);
      if (chunk_it == local_chunks.end())
        continue;
      for (const auto partkey : partkeys) {
        auto part_it = chunk_it->second.parts.find(partkey);
        if (part_it == chunk_it->second.parts.end())
          continue;
        auto rebuilt_it = rebuilt.find(partkey);
        if (rebuilt_it == rebuilt.end()) {
          chunk_it->second.parts.erase(part_it);
          continue;
        }
        part_it->second.stats = std::move(rebuilt_it->second);
        StripTpchQ17PartStatsForPublish(part_it->second.stats);
      }
    }
    for (const auto &attr_key : touched_attr_keys) {
      std::string brand;
      std::string container;
      TpchQ17State::SplitAttrKey(attr_key, brand, container);
      const auto attr_partition = AttrPartsPartition(brand, container);
      const auto attr_snapshot =
          services.StateStore().ReadPartitions(visibility, {attr_partition});
      TpchQ17AttrParts *parts = nullptr;
      for (const auto &image : attr_snapshot.partitions) {
        const auto *typed = AsQ17Image(image, TpchQ17ImageKind::AttrParts);
        if (!typed || !typed->attr_parts)
          continue;
        if (typed->attr_parts->brand != brand ||
            typed->attr_parts->container != container)
          continue;
        parts = &local_attr_parts[attr_key];
        *parts = *typed->attr_parts;
        break;
      }
      if (!parts) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
    }
    for (const auto &attr_key : touched_attr_keys) {
      auto &parts = local_attr_parts[attr_key];
      for (const auto partkey : touched_partkeys) {
        const auto chunk_index = static_cast<int64_t>(
            partkey >> TpchQ17Layout::kPartChunkShift);
        const auto chunk_it = local_chunks.find(chunk_index);
        if (chunk_it == local_chunks.end())
          continue;
        const auto part_it = chunk_it->second.parts.find(partkey);
        if (part_it == chunk_it->second.parts.end()) {
          UpsertAttrContribution(parts, partkey, 0);
          continue;
        }
        if (part_it->second.attr_key != attr_key)
          continue;
        UpsertAttrContribution(
            parts, partkey,
            part_it->second.stats.precomputed_contribution_raw);
      }
    }
    for (auto &[chunk_index, chunk] : local_chunks) {
      const auto partition =
          TpchQ17Layout::kPartStatsChunkBase +
          static_cast<core::PartitionId>(chunk_index);
      const auto chunk_ptr =
          std::make_shared<const TpchQ17PartStatsChunk>(std::move(chunk));
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ17Layout::kAccelerator, partition, 0, MakeChunkImage(chunk_ptr)));
    }
    for (const auto &attr_key : touched_attr_keys) {
      const auto &parts = local_attr_parts[attr_key];
      const auto parts_ptr = std::make_shared<const TpchQ17AttrParts>(parts);
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ17Layout::kAccelerator,
          AttrPartsPartition(parts.brand, parts.container), 0,
          MakeAttrPartsImage(parts_ptr)));
    }
    core::FinishRebuildPartition(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (!initial_scan) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  std::map<int64_t, TpchQ17PartStatsChunk> local_chunks;
  std::map<int64_t, core::Version> chunk_versions;
  std::set<std::string> touched_attr_keys;
  std::map<std::string, TpchQ17AttrParts> local_attr_parts;
  std::map<std::string, core::Version> attr_versions;
  std::map<int64_t, std::vector<int64_t>> parts_by_chunk;
  std::unordered_set<int64_t> need_pair_rebuild;
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

      const std::string attr_key = part_it->second.attr_key;
      if (!attr_key.empty())
        touched_attr_keys.insert(attr_key);

      if (part_it->second.stats.qty_price_pairs.empty())
        need_pair_rebuild.insert(partkey);
    }
  }

  // Price-only updates (same quantity, net count 0) can patch
  // precomputed_contribution_raw from the published avg threshold. A live
  // DuckDB scan during PreCommit already includes this txn, so rebuilding
  // pairs and then applying the same deltas double-counts — that is within
  // 1e-6 relative error for one +1.00 write but fails stacked adversarial
  // writes (3/7 ≈ 0.43 on avg_yearly).
  std::unordered_map<int64_t, int64_t> net_count;
  std::unordered_map<int64_t, int64_t> net_qty;
  std::map<std::pair<int64_t, int64_t>, int64_t> net_price_by_qty;
  for (const auto &delta : buffer.LineitemDeltas()) {
    net_count[delta.partkey] += delta.sign;
    net_qty[delta.partkey] += static_cast<int64_t>(delta.sign) * delta.quantity_raw;
    net_price_by_qty[{delta.partkey, delta.quantity_raw}] +=
        static_cast<int64_t>(delta.sign) * delta.extendedprice_raw;
  }
  bool price_only = true;
  for (const auto partkey : touched_partkeys) {
    if (net_count[partkey] != 0 || net_qty[partkey] != 0) {
      price_only = false;
      break;
    }
  }

  if (price_only) {
    for (auto &[chunk_index, partkeys] : parts_by_chunk) {
      auto chunk_it = local_chunks.find(chunk_index);
      if (chunk_it == local_chunks.end())
        continue;
      auto &chunk = chunk_it->second;
      for (const auto partkey : partkeys) {
        auto part_it = chunk.parts.find(partkey);
        if (part_it == chunk.parts.end())
          continue;
        auto &stats = part_it->second.stats;
        const int64_t threshold =
            QuantityThresholdRaw(stats.sum_qty_raw, stats.count);
        for (const auto &[key, net_price] : net_price_by_qty) {
          if (key.first != partkey || net_price == 0)
            continue;
          if (key.second < threshold)
            stats.precomputed_contribution_raw += net_price;
        }
        StripTpchQ17PartStatsForPublish(stats);
      }
    }
  } else {
    // Scan already reflects this transaction. Reload pairs and do not apply
    // the same deltas on top.
    if (!need_pair_rebuild.empty()) {
      auto rebuilt = RebuildPartStatsBatch(need_pair_rebuild);
      for (auto &[chunk_index, partkeys] : parts_by_chunk) {
        auto chunk_it = local_chunks.find(chunk_index);
        if (chunk_it == local_chunks.end())
          continue;
        for (const auto partkey : partkeys) {
          if (!need_pair_rebuild.count(partkey))
            continue;
          auto part_it = chunk_it->second.parts.find(partkey);
          if (part_it == chunk_it->second.parts.end())
            continue;
          auto rebuilt_it = rebuilt.find(partkey);
          if (rebuilt_it == rebuilt.end()) {
            outcome.action = core::MaintenanceAction::RebuildAll;
            outcome.invalidations = All();
            outcome.pending_rebuild = outcome.invalidations;
            return outcome;
          }
          part_it->second.stats = std::move(rebuilt_it->second);
          StripTpchQ17PartStatsForPublish(part_it->second.stats);
        }
      }
    } else {
      for (auto &[chunk_index, partkeys] : parts_by_chunk) {
        auto chunk_it = local_chunks.find(chunk_index);
        if (chunk_it == local_chunks.end())
          continue;
        auto &chunk = chunk_it->second;
        for (const auto partkey : partkeys) {
          auto part_it = chunk.parts.find(partkey);
          if (part_it == chunk.parts.end())
            continue;
          for (const auto &delta : buffer.LineitemDeltas()) {
            if (delta.partkey != partkey)
              continue;
            ApplyLineitemDeltaToStats(part_it->second.stats, delta);
          }
          if (part_it->second.stats.count <= 0 &&
              part_it->second.stats.qty_price_pairs.empty()) {
            chunk.parts.erase(part_it);
          } else {
            part_it->second.stats.RecomputeContribution();
            StripTpchQ17PartStatsForPublish(part_it->second.stats);
          }
        }
      }
    }
  }

  for (const auto &attr_key : touched_attr_keys) {
    std::string brand;
    std::string container;
    TpchQ17State::SplitAttrKey(attr_key, brand, container);
    const auto attr_partition = AttrPartsPartition(brand, container);
    const auto attr_snapshot =
        services.StateStore().ReadPartitions(visibility, {attr_partition});
    TpchQ17AttrParts *parts = nullptr;
    for (const auto &image : attr_snapshot.partitions) {
      const auto *typed = AsQ17Image(image, TpchQ17ImageKind::AttrParts);
      if (!typed || !typed->attr_parts)
        continue;
      if (typed->attr_parts->brand != brand ||
          typed->attr_parts->container != container)
        continue;
      parts = &local_attr_parts[attr_key];
      *parts = *typed->attr_parts;
      attr_versions[attr_key] = image.version;
      break;
    }
    if (!parts) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }

  for (const auto &attr_key : touched_attr_keys) {
    auto &parts = local_attr_parts[attr_key];
    for (const auto partkey : touched_partkeys) {
      const auto chunk_index = static_cast<int64_t>(partkey >>
                                                    TpchQ17Layout::kPartChunkShift);
      const auto chunk_it = local_chunks.find(chunk_index);
      if (chunk_it == local_chunks.end())
        continue;
      const auto part_it = chunk_it->second.parts.find(partkey);
      if (part_it == chunk_it->second.parts.end()) {
        UpsertAttrContribution(parts, partkey, 0);
        continue;
      }
      if (part_it->second.attr_key != attr_key)
        continue;
      UpsertAttrContribution(parts, partkey,
                             part_it->second.stats.precomputed_contribution_raw);
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
    const auto &parts = local_attr_parts[attr_key];
    const auto version =
        attr_versions.count(attr_key) ? attr_versions[attr_key] : 0;
    const auto parts_ptr = std::make_shared<const TpchQ17AttrParts>(parts);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ17Layout::kAccelerator,
        AttrPartsPartition(parts.brand, parts.container), version,
        MakeAttrPartsImage(parts_ptr)));
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
      visibility, {AttrPartsPartition(brand, container)});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ17Image(image, TpchQ17ImageKind::AttrParts);
    if (!typed || !typed->attr_parts)
      continue;
    if (typed->attr_parts->brand != brand ||
        typed->attr_parts->container != container)
      continue;

    const int64_t contribution_raw = SumAttrContributions(*typed->attr_parts);
    sink.Begin({{{"avg_yearly", core::AuxType::Double, true}}});
    if (contribution_raw == 0) {
      sink.WriteRow({{core::AuxValue::Null(core::AuxType::Double)}});
    } else {
      const double result =
          static_cast<double>(contribution_raw) / 100.0 / 7.0;
      sink.WriteRow({{core::AuxValue::Of(core::AuxType::Double, result)}});
    }
    sink.End();
    return;
  }

  // A valid state without this brand/container's partition: no such part.
  sink.Begin({{{"avg_yearly", core::AuxType::Double, true}}});
  sink.WriteRow({{core::AuxValue::Null(core::AuxType::Double)}});
  sink.End();
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

  const bool used_sql = initial_scan->ScanPartQuantityAggregates(
      [&](const TpchQ17PartQuantityAggregate &agg) {
        auto &stats = state.part_stats[agg.partkey];
        stats.sum_qty_raw += agg.quantity_raw * agg.row_count;
        stats.count += static_cast<int32_t>(agg.row_count);
        stats.qty_price_pairs.push_back(
            TpchQ17QtyPricePair{agg.quantity_raw, agg.extendedprice_sum_raw});
      });
  if (!used_sql) {
    std::unordered_map<int64_t, std::unordered_map<int64_t, int64_t>>
        qty_price_by_part;
    initial_scan->ScanLineitems([&](const TpchQ17LineitemInput &input) {
      auto &stats = state.part_stats[input.partkey];
      stats.sum_qty_raw += input.quantity_raw;
      stats.count += 1;
      qty_price_by_part[input.partkey][input.quantity_raw] +=
          input.extendedprice_raw;
    });
    for (auto &[partkey, qty_map] : qty_price_by_part) {
      auto &stats = state.part_stats[partkey];
      stats.qty_price_pairs.reserve(qty_map.size());
      for (const auto &[qty, price] : qty_map)
        stats.qty_price_pairs.push_back(TpchQ17QtyPricePair{qty, price});
    }
  }

  for (auto &entry : state.part_stats) {
    auto &pairs = entry.second.qty_price_pairs;
    std::sort(pairs.begin(), pairs.end(),
              [](const TpchQ17QtyPricePair &lhs,
                 const TpchQ17QtyPricePair &rhs) {
                return lhs.quantity_raw < rhs.quantity_raw;
              });
    entry.second.RecomputeContribution();
  }

  return state;
}

TpchQ17PartStats TpchQ17Module::RebuildPartStatsFromScan(int64_t partkey) const {
  auto batch = RebuildPartStatsBatch({partkey});
  auto it = batch.find(partkey);
  if (it == batch.end())
    return {};
  return std::move(it->second);
}

std::unordered_map<int64_t, TpchQ17PartStats> TpchQ17Module::RebuildPartStatsBatch(
    const std::unordered_set<int64_t> &partkeys) const {
  std::unordered_map<int64_t, TpchQ17PartStats> out;
  if (!initial_scan || partkeys.empty())
    return out;

  std::unordered_map<int64_t, std::unordered_map<int64_t, int64_t>> qty_price_by_part;
  bool used_sql = false;
  if (partkeys.size() == 1) {
    const int64_t partkey = *partkeys.begin();
    used_sql = initial_scan->ScanPartQuantityAggregatesForPart(
        partkey, [&](const TpchQ17PartQuantityAggregate &agg) {
          if (agg.partkey != partkey)
            return;
          auto &stats = out[agg.partkey];
          stats.sum_qty_raw += agg.quantity_raw * agg.row_count;
          stats.count += static_cast<int32_t>(agg.row_count);
          qty_price_by_part[agg.partkey][agg.quantity_raw] +=
              agg.extendedprice_sum_raw;
        });
  }
  if (!used_sql) {
    used_sql = initial_scan->ScanPartQuantityAggregatesForParts(
        partkeys, [&](const TpchQ17PartQuantityAggregate &agg) {
          if (!partkeys.count(agg.partkey))
            return;
          auto &stats = out[agg.partkey];
          stats.sum_qty_raw += agg.quantity_raw * agg.row_count;
          stats.count += static_cast<int32_t>(agg.row_count);
          qty_price_by_part[agg.partkey][agg.quantity_raw] +=
              agg.extendedprice_sum_raw;
        });
  }
  if (!used_sql) {
    used_sql = initial_scan->ScanPartQuantityAggregates(
        [&](const TpchQ17PartQuantityAggregate &agg) {
          if (!partkeys.count(agg.partkey))
            return;
          auto &stats = out[agg.partkey];
          stats.sum_qty_raw += agg.quantity_raw * agg.row_count;
          stats.count += static_cast<int32_t>(agg.row_count);
          qty_price_by_part[agg.partkey][agg.quantity_raw] +=
              agg.extendedprice_sum_raw;
        });
  }
  if (!used_sql) {
    initial_scan->ScanLineitems([&](const TpchQ17LineitemInput &input) {
      if (!partkeys.count(input.partkey))
        return;
      auto &stats = out[input.partkey];
      stats.sum_qty_raw += input.quantity_raw;
      stats.count += 1;
      qty_price_by_part[input.partkey][input.quantity_raw] +=
          input.extendedprice_raw;
    });
  }

  for (auto &[partkey, qty_map] : qty_price_by_part) {
    auto &stats = out[partkey];
    stats.qty_price_pairs.clear();
    stats.qty_price_pairs.reserve(qty_map.size());
    for (const auto &[qty, price] : qty_map)
      stats.qty_price_pairs.push_back(TpchQ17QtyPricePair{qty, price});
    std::sort(stats.qty_price_pairs.begin(), stats.qty_price_pairs.end(),
              [](const TpchQ17QtyPricePair &lhs, const TpchQ17QtyPricePair &rhs) {
                return lhs.quantity_raw < rhs.quantity_raw;
              });
    stats.RecomputeContribution();
  }
  return out;
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
