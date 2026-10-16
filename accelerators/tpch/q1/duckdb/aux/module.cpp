#include "tpch/q1/duckdb/aux/module.hpp"
#include "tpch/q1/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include "duckdb/common/types/hugeint.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include "tpch/q1/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

using Q1DayPartition = ::accelerator_codegen::Q1DayPartition;
using Q1GroupKey = ::accelerator_codegen::Q1GroupKey;

constexpr uint32_t kVersion = 1;
constexpr uint32_t kDayMagic = 0x44315151; // Q1D
constexpr int32_t kEpoch = 719468;

constexpr int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

constexpr int32_t kQ1BaseDays = DaysFromCivil(1998, 12, 1);
// TPC-H lineitem shipdate lower bound (1992-01-02).
constexpr int32_t kTpchMinLineitemShipdateDays = DaysFromCivil(1992, 1, 2);

// Dyadic interval tree over the shipdate day index. Implicit heap numbering:
// node 1 is the root, node n has children 2n and 2n+1, leaves are
// kDomainSize + day_index. One partition per node (partition id = node id,
// < 2^17, well inside the store's int32 key space). Days outside the domain
// (before 1900 / after 2079) are clamped into the first / last leaf.
constexpr uint32_t kLeafBits = 16;
constexpr uint32_t kDomainSize = 1u << kLeafBits;
constexpr int32_t kDomainBase = DaysFromCivil(1900, 1, 1);

uint32_t LeafNode(int32_t day) {
  int64_t idx = static_cast<int64_t>(day) - kDomainBase;
  if (idx < 0)
    idx = 0;
  if (idx >= static_cast<int64_t>(kDomainSize))
    idx = kDomainSize - 1;
  return kDomainSize + static_cast<uint32_t>(idx);
}

// Canonical aligned-node cover of the days [kDomainBase, cutoff_days]; fills
// `nodes` (capacity 2 * kLeafBits) and returns the count.
uint32_t CoverPrefix(int32_t cutoff_days, uint32_t *nodes) {
  uint32_t count = 0;
  const int64_t last = static_cast<int64_t>(cutoff_days) - kDomainBase;
  if (last < 0)
    return 0;
  uint64_t l = kDomainSize;
  uint64_t r = kDomainSize + std::min<int64_t>(last, kDomainSize - 1) + 1;
  while (l < r) {
    if (l & 1)
      nodes[count++] = static_cast<uint32_t>(l++);
    if (r & 1)
      nodes[count++] = static_cast<uint32_t>(--r);
    l >>= 1;
    r >>= 1;
  }
  return count;
}

void WriteQ1Acc(ByteWriter &writer, ::accelerator_codegen::Q1AccT value) {
#if defined(__SIZEOF_INT128__)
  writer.U64(static_cast<uint64_t>(value));
  writer.U64(static_cast<uint64_t>(value >> 64));
#else
  writer.I64(value);
#endif
}

bool ReadQ1Acc(ByteReader &reader, ::accelerator_codegen::Q1AccT &value) {
#if defined(__SIZEOF_INT128__)
  uint64_t lo = 0;
  uint64_t hi = 0;
  if (!reader.U64(lo) || !reader.U64(hi)) {
    return false;
  }
  value = static_cast<::accelerator_codegen::Q1AccT>(hi) << 64;
  value |= static_cast<::accelerator_codegen::Q1AccT>(lo);
  return true;
#else
  return reader.I64(value);
#endif
}

// A one-character string cell (no copy).
bool CharCell(const core::AuxRow &row, std::size_t index, char &out) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto *text = std::get_if<std::string>(&row.values[index].payload);
  if (!text || text->size() != 1)
    return false;
  out = (*text)[0];
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

bool DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  days = static_cast<int32_t>(value);
  return true;
}

int64_t ScaleToRaw(int64_t raw, int32_t scale, int32_t target_scale) {
  if (scale == target_scale)
    return raw;
  if (scale < target_scale) {
    int64_t factor = 1;
    for (int32_t i = scale; i < target_scale; ++i)
      factor *= 10;
    return raw * factor;
  }
  int64_t factor = 1;
  for (int32_t i = target_scale; i < scale; ++i)
    factor *= 10;
  return raw / factor;
}


// For each requested partition the matching image of `found` (or nullptr).
// Stores return found partitions in request order, but the interface does not
// promise it: the in-order walk is the fast path, anything else is matched by
// sorted lookup.
void MatchSnapshot(const std::vector<core::PartitionId> &requested,
                   const std::vector<core::StateImage> &found,
                   std::vector<const core::StateImage *> &out) {
  out.assign(requested.size(), nullptr);
  std::size_t at = 0;
  for (std::size_t slot = 0; slot < requested.size(); ++slot) {
    if (at < found.size() && found[at].partition == requested[slot])
      out[slot] = &found[at++];
  }
  if (at == found.size())
    return;
  std::vector<const core::StateImage *> sorted;
  sorted.reserve(found.size());
  for (const auto &image : found)
    sorted.push_back(&image);
  std::sort(sorted.begin(), sorted.end(),
            [](const core::StateImage *l, const core::StateImage *r) {
              return l->partition < r->partition;
            });
  for (std::size_t slot = 0; slot < requested.size(); ++slot) {
    const auto it = std::lower_bound(
        sorted.begin(), sorted.end(), requested[slot],
        [](const core::StateImage *image, core::PartitionId id) {
          return image->partition < id;
        });
    out[slot] = it != sorted.end() && (*it)->partition == requested[slot]
                    ? *it
                    : nullptr;
  }
}

const TpchQ1StateImage *AsQ1Image(const core::StateImage &image) {
  return image.As<TpchQ1StateImage>();
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ1LineitemDelta &delta,
                  int8_t sign) {
  int32_t shipdate_days = 0;
  int64_t quantity_raw = 0, extended_raw = 0, discount_raw = 0, tax_raw = 0;
  int32_t quantity_scale = 0, extended_scale = 0, discount_scale = 0,
          tax_scale = 0;
  char returnflag = 0, linestatus = 0;
  if (!DateCell(row, 0, shipdate_days) ||
      !RawDecimalCell(row, 1, quantity_raw, quantity_scale) ||
      !RawDecimalCell(row, 2, extended_raw, extended_scale) ||
      !RawDecimalCell(row, 3, discount_raw, discount_scale) ||
      !RawDecimalCell(row, 4, tax_raw, tax_scale) ||
      !CharCell(row, 5, returnflag) || !CharCell(row, 6, linestatus))
    return false;
  delta.shipdate_days = shipdate_days;
  delta.group_key = ::accelerator_codegen::PackQ1GroupKey(returnflag, linestatus);
  delta.quantity_raw = ScaleToRaw(quantity_raw, quantity_scale, 2);
  delta.extended_price_raw = ScaleToRaw(extended_raw, extended_scale, 2);
  delta.discount_raw = ScaleToRaw(discount_raw, discount_scale, 2);
  delta.tax_raw = ScaleToRaw(tax_raw, tax_scale, 2);
  delta.sign = sign;
  return true;
}

void ApplyLineitemInput(const TpchQ1LineitemInput &input,
                        Q1DayPartition &day) {
  const auto group_key = ::accelerator_codegen::PackQ1GroupKey(
      input.returnflag, input.linestatus);
  auto *group = day.FindOrAdd(group_key);
  if (!group)
    throw std::logic_error("TPC-H Q1: more (returnflag, linestatus) groups than slots");
  ::accelerator_codegen::ApplyQ1RowContribution(
      *group, ScaleToRaw(input.quantity_raw, input.quantity_scale, 2),
      ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2),
      ScaleToRaw(input.discount_raw, input.discount_scale, 2),
      ScaleToRaw(input.tax_raw, input.tax_scale, 2), 1);
}

core::ScaledDecimal128 WideDecimal(const duckdb::hugeint_t &value, int32_t scale) {
  const auto wide = (static_cast<__int128>(value.upper) << 64) | value.lower;
  return {wide, scale};
}

} // namespace

std::vector<uint8_t> EncodeTpchQ1Day(const Q1DayPartition &partition) {
  std::vector<Q1GroupKey> keys(partition.keys, partition.keys + partition.n);
  std::sort(keys.begin(), keys.end());

  ByteWriter writer;
  core::WriteImageHeader(writer, kDayMagic, kVersion, static_cast<uint64_t>(keys.size()));
  for (const auto key : keys) {
    const auto &stats = partition.stats[partition.Find(key)];
    writer.U16(key);
    writer.I64(stats.count);
    writer.I64(stats.sum_qty);
    writer.I64(stats.sum_base_price);
    WriteQ1Acc(writer, stats.sum_disc_price);
    WriteQ1Acc(writer, stats.sum_charge);
    WriteQ1Acc(writer, stats.sum_discount);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ1Day(const std::vector<uint8_t> &bytes, Q1DayPartition &partition) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kDayMagic, kVersion, count))
    return false;
  partition = Q1DayPartition{};
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t key = 0;
    ::accelerator_codegen::Q1DayGroupStats stats;
    if (!reader.U16(key) || !reader.I64(stats.count) ||
        !reader.I64(stats.sum_qty) || !reader.I64(stats.sum_base_price) ||
        !ReadQ1Acc(reader, stats.sum_disc_price) || !ReadQ1Acc(reader, stats.sum_charge) ||
        !ReadQ1Acc(reader, stats.sum_discount))
      return false;
    auto *slot = partition.FindOrAdd(key);
    if (!slot)
      return false;
    *slot = stats;
  }
  return reader.offset == bytes.size();
}

int32_t TpchQ1CutoffDays(int32_t day_offset) {
  return kQ1BaseDays - day_offset;
}

bool TpchQ1TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ1TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ1TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ1LineitemDelta> &TpchQ1TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ1TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ1TxnBuffer::AddDelta(TpchQ1LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ1Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ1Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3, 4, 5, 6}, {10, 4, 5, 6, 7, 8, 9}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ1Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ1TxnBuffer>();
}

void TpchQ1Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto days = BuildState(services);
  // Fold the per-day aggregates into every dyadic ancestor node.
  std::map<uint32_t, TpchQ1StateImage> nodes;
  for (auto &entry : days) {
    for (uint32_t node = LeafNode(entry.first); node >= 1; node >>= 1) {
      auto &target = nodes[node].partition;
      for (uint32_t g = 0; g < entry.second.n; ++g) {
        auto *stats = target.FindOrAdd(entry.second.keys[g]);
        if (!stats)
          throw std::logic_error(
              "TPC-H Q1: more (returnflag, linestatus) groups than slots");
        const auto &add = entry.second.stats[g];
        stats->count += add.count;
        stats->sum_qty += add.sum_qty;
        stats->sum_base_price += add.sum_base_price;
        stats->sum_disc_price += add.sum_disc_price;
        stats->sum_charge += add.sum_charge;
        stats->sum_discount += add.sum_discount;
      }
    }
  }
  days.clear();
  for (auto &entry : nodes) {
    if (entry.second.partition.Empty())
      continue;
    auto image = std::make_shared<TpchQ1StateImage>(std::move(entry.second));
    image->shipdate_days = static_cast<int32_t>(entry.first);
    builder.Write(core::StateImage::Make(
        core::TpchQ1Layout::kAccelerator,
        static_cast<core::PartitionId>(entry.first), 0,
        std::shared_ptr<const TpchQ1StateImage>(std::move(image))));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ1Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

namespace {

// Tree nodes per slice below which a commit's node images are built inline.
constexpr std::size_t kNodeGrain = 512;

// A fresh image of `node` holding the stored one plus the deltas order[range]
// (copy-on-write: published images are never mutated). Null if the stored
// image is foreign or a group table overflows.
std::shared_ptr<TpchQ1StateImage>
ApplyToNode(core::PartitionId node, const core::StateImage *stored,
            const std::vector<TpchQ1LineitemDelta> &deltas,
            const std::vector<std::pair<uint32_t, uint32_t>> &order,
            std::pair<uint32_t, uint32_t> range) {
  auto image = std::make_shared<TpchQ1StateImage>();
  image->shipdate_days = static_cast<int32_t>(node);
  if (stored) {
    const auto *typed = AsQ1Image(*stored);
    if (!typed || typed->shipdate_days != image->shipdate_days)
      return nullptr;
    image->partition = typed->partition;
  }
  auto &part = image->partition;
  for (uint32_t i = range.first; i < range.second; ++i) {
    const auto &delta = deltas[order[i].second];
    auto *group = part.FindOrAdd(delta.group_key);
    if (!group)
      return nullptr;
    ::accelerator_codegen::ApplyQ1RowContribution(
        *group, delta.quantity_raw, delta.extended_price_raw,
        delta.discount_raw, delta.tax_raw, delta.sign);
    if (group->count == 0)
      part.EraseAt(static_cast<uint32_t>(group - part.stats));
  }
  return image;
}

} // namespace

void TpchQ1Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ1TxnBuffer &>(base_buffer);
  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ1LineitemDelta delta;
    if (!ReadLineitem(row, delta, sign)) {
      buffer.MarkRebuild();
      return;
    }
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
      TpchQ1LineitemDelta old_delta, new_delta;
      if (!ReadLineitem(mutation.old_rows[i], old_delta, -1) ||
          !ReadLineitem(mutation.new_rows[i], new_delta, 1)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddDelta(old_delta);
      buffer.AddDelta(new_delta);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ1Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ1TxnBuffer &>(base_buffer);
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
  if (buffer.Deltas().empty())
    return outcome;

  // RF-sized batches are always applied incrementally; the cost model is
  // only consulted for bulk changes (a partition rebuild is not possible for
  // aggregate nodes, so it maps to RebuildAll). A forced action applies to
  // every batch.
  if (core::ForcedMaintenanceAction() || buffer.Deltas().size() > 256) {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
         false, false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      outcome.action =
          decision.action == core::MaintenanceAction::RebuildPartition
              ? core::MaintenanceAction::RebuildAll
              : decision.action;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
  }

  // Every delta updates its leaf and all ancestors (copy-on-write: each
  // touched node is copied into a fresh image; published images are never
  // mutated). Deltas are visited in leaf order, so the deltas of one node are a
  // contiguous run of `order` and the nodes of a level are found by a
  // forward-only walk (no maps, no searches).
  const auto &deltas = buffer.Deltas();
  constexpr int kLevels = 17; // leaf level 0 .. root level 16
  std::vector<std::pair<uint32_t, uint32_t>> order; // (leaf node, delta index)
  order.reserve(deltas.size());
  for (uint32_t i = 0; i < deltas.size(); ++i)
    order.emplace_back(LeafNode(deltas[i].shipdate_days), i);
  std::sort(order.begin(), order.end());

  std::vector<core::PartitionId> partitions; // touched nodes, level by level
  std::vector<std::pair<uint32_t, uint32_t>> runs; // per node: range in `order`
  partitions.reserve(order.size() * 4 + kLevels);
  runs.reserve(partitions.capacity());
  for (int level = 0; level < kLevels; ++level) {
    uint32_t last = 0;
    for (uint32_t i = 0; i < order.size(); ++i) {
      const uint32_t node = order[i].first >> level;
      if (node != last) {
        if (last != 0)
          runs.back().second = i;
        partitions.push_back(static_cast<core::PartitionId>(node));
        runs.emplace_back(i, 0);
        last = node;
      }
    }
    runs.back().second = static_cast<uint32_t>(order.size());
  }

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  std::vector<const core::StateImage *> matched;
  MatchSnapshot(partitions, snapshot.partitions, matched);

  // Nodes are independent: each slice builds its own images.
  outcome.updates.resize(partitions.size());
  std::atomic<bool> unusable{false};
  core::ParallelSlices(
      services, partitions.size(), kNodeGrain,
      [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t slot = begin; slot < end; ++slot) {
          auto image = ApplyToNode(partitions[slot], matched[slot], deltas,
                                   order, runs[slot]);
          if (!image) {
            unusable = true;
            return;
          }
          outcome.updates[slot] = core::StateImage::Make(
              core::TpchQ1Layout::kAccelerator, partitions[slot],
              matched[slot] ? matched[slot]->version : 0,
              std::shared_ptr<const TpchQ1StateImage>(std::move(image)));
        }
      });
  if (unusable) {
    outcome.updates.clear();
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  return outcome;
}

bool TpchQ1Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ1Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  int32_t day_offset = 0;
  if (arguments.values.empty() || arguments.values[0].is_null)
    return;
  if (std::holds_alternative<int64_t>(arguments.values[0].payload))
    day_offset = static_cast<int32_t>(
        std::get<int64_t>(arguments.values[0].payload));
  else
    return;
  const auto cutoff_days = TpchQ1CutoffDays(day_offset);
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  // Decompose [min day, cutoff] into <= ~2*log2(domain) aligned dyadic nodes,
  // read them in one call, borrow the immutable images and merge into a flat
  // fixed-size accumulator (no maps, no per-read allocation besides the
  // reused partition list).
  thread_local std::vector<core::PartitionId> partitions;
  partitions.clear();
  {
    uint32_t nodes[2 * kLeafBits + 2];
    const uint32_t count = CoverPrefix(cutoff_days, nodes);
    for (uint32_t i = 0; i < count; ++i)
      partitions.push_back(static_cast<core::PartitionId>(nodes[i]));
  }
  ::accelerator_codegen::Q1MergedFlat merged;
  if (!partitions.empty()) {
    const auto snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), partitions);
    for (const auto &image : snapshot.partitions) {
      const auto node = static_cast<int32_t>(image.partition);
      const auto *typed = AsQ1Image(image);
      if (!typed || typed->shipdate_days != node ||
          !merged.AddPartition(typed->partition)) {
        ExecuteFallback(services, arguments, sink);
        return;
      }
    }
  }
  ::accelerator_codegen::Q1OutputRow rows[::accelerator_codegen::kQ1MaxGroups];
  const uint32_t row_count =
      ::accelerator_codegen::FinalizeQ1MergedFlat(merged, rows);
  EmitResult(rows, row_count, sink);
}

void TpchQ1Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  int32_t day_offset = 0;
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<int64_t>(arguments.values[0].payload))
    return;
  day_offset =
      static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  const auto rows =
      ComposeOutput(BuildState(services), TpchQ1CutoffDays(day_offset));
  EmitResult(rows.data(), static_cast<uint32_t>(rows.size()), sink);
}

std::unordered_map<int32_t, Q1DayPartition>
TpchQ1Module::BuildState(core::AuxEngineServices &services) const {
  return BuildStateForDays(services, {});
}

std::unordered_map<int32_t, Q1DayPartition>
TpchQ1Module::BuildStateForDays(core::AuxEngineServices &services,
                                const std::set<int32_t> &days) const {
  (void)services;
  std::unordered_map<int32_t, Q1DayPartition> state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q1 requires a typed initial scan source");

  auto consume = [&](const TpchQ1DayGroupAggregate &agg) {
    if (!days.empty() && days.find(agg.shipdate_days) == days.end())
      return;
    const auto group_key = ::accelerator_codegen::PackQ1GroupKey(
        agg.returnflag, agg.linestatus);
    auto *group = state[agg.shipdate_days].FindOrAdd(group_key);
    if (!group)
      throw std::logic_error(
          "TPC-H Q1: more (returnflag, linestatus) groups than slots");
    group->count += agg.count;
    group->sum_qty += agg.sum_qty_raw;
    group->sum_base_price += agg.sum_base_price_raw;
    group->sum_discount += agg.sum_discount_raw;
    group->sum_disc_price +=
        static_cast<::accelerator_codegen::Q1AccT>(agg.sum_disc_price_raw);
    group->sum_charge +=
        static_cast<::accelerator_codegen::Q1AccT>(agg.sum_charge_raw);
  };

  if (!days.empty() && initial_scan->ScanDayGroupAggregatesForDays(days, consume))
    return state;
  if (initial_scan->ScanDayGroupAggregates(consume))
    return state;

  initial_scan->ScanLineitems([&](const TpchQ1LineitemInput &input) {
    if (!days.empty() && days.find(input.shipdate_days) == days.end())
      return;
    ApplyLineitemInput(input, state[input.shipdate_days]);
  });
  return state;
}

std::vector<::accelerator_codegen::Q1OutputRow> TpchQ1Module::ComposeOutput(
    const std::unordered_map<int32_t, Q1DayPartition> &days,
    int32_t cutoff_days) const {
  std::unordered_map<Q1GroupKey, ::accelerator_codegen::Q1MergedGroupStats>
      merged;
  for (const auto &entry : days) {
    if (entry.first > cutoff_days)
      continue;
    for (uint32_t g = 0; g < entry.second.n; ++g)
      ::accelerator_codegen::MergeQ1DayGroupStats(
          merged[entry.second.keys[g]], entry.second.stats[g]);
  }
  return ::accelerator_codegen::FinalizeQ1MergedGroups(merged);
}

void TpchQ1Module::EmitResult(const ::accelerator_codegen::Q1OutputRow *rows,
                              uint32_t row_count, core::AuxRowSink &sink) const {
  static const core::OutputSchema kSchema{
      {{"l_returnflag", core::AuxType::String, false},
       {"l_linestatus", core::AuxType::String, false},
       {"sum_qty", core::AuxType::ScaledDecimal128, false},
       {"sum_base_price", core::AuxType::ScaledDecimal128, false},
       {"sum_disc_price", core::AuxType::ScaledDecimal128, false},
       {"sum_charge", core::AuxType::ScaledDecimal128, false},
       {"avg_qty", core::AuxType::Double, false},
       {"avg_price", core::AuxType::Double, false},
       {"avg_disc", core::AuxType::Double, false},
       {"count_order", core::AuxType::Int64, false}}};
  sink.Begin(kSchema);
  // One reused row: the value vector is allocated once, per-row stores only
  // overwrite payloads (the one-character strings fit the small-string buffer).
  core::AuxRow row;
  row.values.resize(10);
  const core::AuxType types[10] = {
      core::AuxType::String,           core::AuxType::String,
      core::AuxType::ScaledDecimal128, core::AuxType::ScaledDecimal128,
      core::AuxType::ScaledDecimal128, core::AuxType::ScaledDecimal128,
      core::AuxType::Double,        core::AuxType::Double,
      core::AuxType::Double,        core::AuxType::Int64};
  for (int i = 0; i < 10; ++i) {
    row.values[i].type = types[i];
    row.values[i].is_null = false;
  }
  row.values[0].payload.emplace<std::string>();
  row.values[1].payload.emplace<std::string>();
  for (uint32_t i = 0; i < row_count; ++i) {
    const auto &r = rows[i];
    std::get<std::string>(row.values[0].payload)
        .assign(1, static_cast<char>(r.group_key >> 8));
    std::get<std::string>(row.values[1].payload)
        .assign(1, static_cast<char>(r.group_key & 0xFF));
    row.values[2].payload = WideDecimal(r.sum_qty, 2);
    row.values[3].payload = WideDecimal(r.sum_base_price, 2);
    row.values[4].payload = WideDecimal(r.sum_disc_price, 4);
    row.values[5].payload = WideDecimal(r.sum_charge, 6);
    row.values[6].payload = r.avg_qty;
    row.values[7].payload = r.avg_price;
    row.values[8].payload = r.avg_disc;
    row.values[9].payload = static_cast<int64_t>(r.count_order);
    sink.WriteRow(row);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ1Module::CalibrationWrites() const {
  return TpchQ1CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
