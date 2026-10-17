#include "tpch/q6/duckdb/aux/module.hpp"
#include "tpch/q6/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include "duckdb/common/types/hugeint.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include "tpch/q6/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

using Q6AccT = ::accelerator_codegen::Q6AccT;
using Q6RevenueMatrix = ::accelerator_codegen::Q6RevenueMatrix;

// Month matrices per read slice (~4 us: two loads each).
constexpr std::size_t kEvalGrain = 4096;
// Month images per slice (~5 us: a matrix copy plus the deltas).
constexpr std::size_t kPartitionGrain = 16;
constexpr uint32_t kVersion = 3; // cells are 2D prefix sums
constexpr uint32_t kMatrixMagic = 0x51364D58; // Q6MX
// Months in the query's shipdate window.
constexpr int32_t kWindowMonths = 12;

void WriteQ6Acc(ByteWriter &writer, Q6AccT value) {
#if defined(__SIZEOF_INT128__)
  writer.U32(static_cast<uint32_t>(value));
  writer.U32(static_cast<uint32_t>(value >> 32));
  writer.U32(static_cast<uint32_t>(value >> 64));
  writer.U32(static_cast<uint32_t>(value >> 96));
#else
  writer.U32(static_cast<uint32_t>(value.lower));
  writer.U32(static_cast<uint32_t>(value.lower >> 32));
  writer.U32(static_cast<uint32_t>(value.upper));
  writer.U32(static_cast<uint32_t>(value.upper >> 32));
#endif
}

bool ReadQ6Acc(ByteReader &reader, Q6AccT &value) {
#if defined(__SIZEOF_INT128__)
  uint32_t w0 = 0;
  uint32_t w1 = 0;
  uint32_t w2 = 0;
  uint32_t w3 = 0;
  if (!reader.U32(w0) || !reader.U32(w1) || !reader.U32(w2) || !reader.U32(w3)) {
    return false;
  }
  value = static_cast<Q6AccT>(w0) | (static_cast<Q6AccT>(w1) << 32) |
          (static_cast<Q6AccT>(w2) << 64) | (static_cast<Q6AccT>(w3) << 96);
  return true;
#else
  uint32_t lo_lo = 0;
  uint32_t lo_hi = 0;
  uint32_t hi_lo = 0;
  uint32_t hi_hi = 0;
  if (!reader.U32(lo_lo) || !reader.U32(lo_hi) || !reader.U32(hi_lo) ||
      !reader.U32(hi_hi)) {
    return false;
  }
  value.lower = static_cast<uint64_t>(lo_lo) | (static_cast<uint64_t>(lo_hi) << 32);
  value.upper = static_cast<int64_t>(static_cast<uint64_t>(hi_lo) |
                                     (static_cast<uint64_t>(hi_hi) << 32));
  return true;
#endif
}

int32_t MonthKeyOf(int32_t days) {
  return static_cast<int32_t>(core::sql_date::MonthIndex(days));
}

int32_t StartOfMonth(int32_t days) {
  return static_cast<int32_t>(core::sql_date::MonthStart(days));
}

int32_t AddMonths(int32_t days, int32_t months) {
  return static_cast<int32_t>(core::sql_date::AddMonths(days, months));
}

// A fresh, zeroed image to fill in place before it is published as const.
std::shared_ptr<TpchQ6StateImage> NewStateImage(TpchQ6PartitionKind kind,
                                                int32_t key) {
  auto image = std::make_shared<TpchQ6StateImage>();
  image->kind = kind;
  image->key = key;
  return image;
}

TpchQ6PartitionKind KindOf(core::PartitionId partition) {
  return core::TpchQ6Layout::IsMonthPartition(partition)
             ? TpchQ6PartitionKind::Month
             : TpchQ6PartitionKind::Day;
}

int32_t KeyOf(core::PartitionId partition) {
  return core::TpchQ6Layout::IsMonthPartition(partition)
             ? core::TpchQ6Layout::MonthKeyFromPartition(partition)
             : core::TpchQ6Layout::DaysFromPartition(partition);
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

const TpchQ6StateImage *AsQ6Image(const core::StateImage &image) {
  return image.As<TpchQ6StateImage>();
}

// A decimal column; a double cell is taken at scale 2.
bool DecimalOrDoubleCell(const core::AuxRow &row, std::size_t index, int64_t &raw,
                         int32_t &scale) {
  if (core::DecimalCell(row, index, raw, scale))
    return true;
  double value = 0.0;
  if (!core::DoubleCell(row, index, value))
    return false;
  raw = static_cast<int64_t>(std::llround(value * 100.0));
  scale = 2;
  return true;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ6LineitemDelta &delta,
                  int8_t sign) {
  int32_t shipdate_days = 0;
  int64_t discount_raw = 0, quantity_raw = 0, extended_raw = 0;
  int32_t discount_scale = 0, quantity_scale = 0, extended_scale = 0;
  if (!core::DateCell(row, 0, shipdate_days) ||
      !DecimalOrDoubleCell(row, 1, discount_raw, discount_scale) ||
      !DecimalOrDoubleCell(row, 2, quantity_raw, quantity_scale) ||
      !DecimalOrDoubleCell(row, 3, extended_raw, extended_scale))
    return false;
  discount_raw = core::ScaleToRaw(discount_raw, discount_scale, 2);
  quantity_raw = core::ScaleToRaw(quantity_raw, quantity_scale, 2);
  extended_raw = core::ScaleToRaw(extended_raw, extended_scale, 2);
  delta.out_of_domain =
      !::accelerator_codegen::Q6InDomain(discount_raw, quantity_raw);
  delta.shipdate_days = shipdate_days;
  delta.month_key = MonthKeyOf(shipdate_days);
  delta.discount_cent = static_cast<int8_t>(discount_raw);
  delta.quantity_scaled = quantity_raw;
  delta.contribution =
      ::accelerator_codegen::Q6RowContributionRaw(extended_raw, discount_raw);
  delta.sign = sign;
  return true;
}

// Removing an out-of-domain row changes nothing: its month stays declined
// until rebuilt.
void AddDelta(TpchQ6TxnBuffer &buffer, const TpchQ6LineitemDelta &delta) {
  if (!delta.out_of_domain || delta.sign > 0)
    buffer.AddDelta(delta);
}

int64_t HugeintLower(const duckdb::hugeint_t &value) {
  return static_cast<int64_t>(value.lower);
}

// Copy-on-write: a fresh image holding `base`'s matrix (none: a month without
// rows yet) plus the deltas order[begin, end).
std::shared_ptr<TpchQ6StateImage>
ApplyDeltas(const TpchQ6StateImage *base, core::PartitionId partition,
            const std::vector<TpchQ6LineitemDelta> &deltas,
            const std::vector<std::pair<core::PartitionId, uint32_t>> &order,
            std::size_t begin, std::size_t end) {
  auto image = NewStateImage(KindOf(partition), KeyOf(partition));
  if (base) {
    image->matrix = base->matrix;
    image->declined = base->declined; // its out-of-domain row is still there
  }
  // Accumulate the batch raw, prefix it once, add the whole block.
  Q6RevenueMatrix batch;
  const bool small = end - begin <= 3;
  for (std::size_t i = begin; i < end; ++i) {
    const auto &delta = deltas[order[i].second];
    if (delta.out_of_domain) {
      image->declined = true;
    } else if (small) {
      image->matrix.AddCell(delta.discount_cent, delta.quantity_scaled,
                            delta.contribution, delta.sign);
    } else {
      batch.AddRawCell(delta.discount_cent, delta.quantity_scaled,
                       delta.contribution, delta.sign);
    }
  }
  if (!small) {
    batch.PrefixInPlace();
    image->matrix.AddMatrix(batch);
  }
  return image;
}

using DeltaOrder = std::vector<std::pair<core::PartitionId, uint32_t>>;

// The images for the month partitions touched by `deltas` (`order` groups them
// by partition, `starts` delimits the groups) built copy-on-write from the
// published ones. False when a published image is foreign: the caller rebuilds.
bool ApplyToMonths(core::AuxEngineServices &services,
                   const std::vector<TpchQ6LineitemDelta> &deltas, const DeltaOrder &order,
                   const std::vector<core::PartitionId> &partitions,
                   const std::vector<std::size_t> &starts,
                   core::StagedMaintenanceOutcome &outcome) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), partitions);
  std::vector<const core::StateImage *> matched;
  MatchSnapshot(partitions, snapshot.partitions, matched);
  std::vector<const TpchQ6StateImage *> base(partitions.size());
  for (std::size_t p = 0; p < partitions.size(); ++p) {
    if (!matched[p])
      continue; // a month without rows has no partition yet
    const auto *typed = AsQ6Image(*matched[p]);
    if (matched[p]->partition != partitions[p] || !typed ||
        typed->kind != KindOf(partitions[p]) || typed->key != KeyOf(partitions[p]))
      return false;
    base[p] = typed;
  }
  // Partitions are independent: each slice builds its own images.
  std::vector<std::shared_ptr<TpchQ6StateImage>> images(partitions.size());
  core::ParallelEach(services, partitions.size(), kPartitionGrain, [&](std::size_t p) {
    images[p] = ApplyDeltas(base[p], partitions[p], deltas, order, starts[p], starts[p + 1]);
  });
  outcome.updates.reserve(partitions.size());
  for (std::size_t p = 0; p < partitions.size(); ++p)
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ6Layout::kAccelerator, partitions[p],
        matched[p] ? matched[p]->version : 0,
        std::shared_ptr<const TpchQ6StateImage>(std::move(images[p]))));
  outcome.action = core::MaintenanceAction::ApplyDelta;
  return true;
}

} // namespace

std::vector<uint8_t> EncodeTpchQ6Matrix(const Q6RevenueMatrix &matrix) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kMatrixMagic, kVersion,
                         static_cast<uint64_t>(matrix.cells.size()));
  for (const auto &cell : matrix.cells) {
    WriteQ6Acc(writer, cell);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ6Matrix(const std::vector<uint8_t> &bytes,
                        Q6RevenueMatrix &matrix) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kMatrixMagic, kVersion, count) ||
      count != matrix.cells.size()) {
    return false;
  }
  for (auto &cell : matrix.cells) {
    if (!ReadQ6Acc(reader, cell)) {
      return false;
    }
  }
  return reader.FullyConsumed();
}

bool ParseTpchQ6QueryParams(const core::QueryArguments &arguments,
                            TpchQ6QueryParams &params) {
  if (arguments.values.size() < 3)
    return false;
  if (arguments.values[0].is_null || arguments.values[1].is_null ||
      arguments.values[2].is_null)
    return false;

  if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
    params.start_days =
        std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
  else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
    params.start_days =
        static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  else
    return false;

  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  if (std::holds_alternative<core::ScaledDecimal>(
          arguments.values[1].payload)) {
    const auto decimal =
        std::get<core::ScaledDecimal>(arguments.values[1].payload);
    discount_raw = decimal.value;
    discount_scale = decimal.scale;
  } else if (std::holds_alternative<double>(arguments.values[1].payload)) {
    const auto value = std::get<double>(arguments.values[1].payload);
    discount_raw = static_cast<int64_t>(std::llround(value * 100.0));
    discount_scale = 2;
  } else if (std::holds_alternative<int64_t>(arguments.values[1].payload)) {
    discount_raw = std::get<int64_t>(arguments.values[1].payload);
    discount_scale = 2;
  } else {
    return false;
  }
  discount_raw = core::ScaleToRaw(discount_raw, discount_scale, 2);
  params.discount_lower_raw = discount_raw - 1;
  params.discount_upper_raw = discount_raw + 1;

  int64_t quantity_raw = 0;
  int32_t quantity_scale = 2;
  if (std::holds_alternative<core::ScaledDecimal>(
          arguments.values[2].payload)) {
    const auto decimal =
        std::get<core::ScaledDecimal>(arguments.values[2].payload);
    quantity_raw = decimal.value;
    quantity_scale = decimal.scale;
  } else if (std::holds_alternative<double>(arguments.values[2].payload)) {
    const auto value = std::get<double>(arguments.values[2].payload);
    quantity_raw = static_cast<int64_t>(std::llround(value * 100.0));
    quantity_scale = 2;
  } else if (std::holds_alternative<int64_t>(arguments.values[2].payload)) {
    quantity_raw = std::get<int64_t>(arguments.values[2].payload);
    quantity_scale = 2;
  } else {
    return false;
  }
  params.quantity_upper_raw = core::ScaleToRaw(quantity_raw, quantity_scale, 2);
  params.end_days = AddMonths(params.start_days, kWindowMonths);
  return true;
}

core::ScaledDecimal TpchQ6RevenueOutputValue(Q6AccT raw) {
  const auto hugeint = ::accelerator_codegen::Q6RevenueFromAccumulator(raw);
  return core::ScaledDecimal{HugeintLower(hugeint),
                             ::accelerator_codegen::kQ6RevenueDecimalScale};
}

bool TpchQ6TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ6TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ6TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ6LineitemDelta> &TpchQ6TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ6TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ6TxnBuffer::AddDelta(TpchQ6LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}

core::AcceleratorId TpchQ6Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ6Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3}, {10, 6, 4, 5}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ6Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ6TxnBuffer>();
}

void TpchQ6Module::ApplyLineitemInput(
    const TpchQ6LineitemInput &input,
    std::unordered_map<core::PartitionId, Q6RevenueMatrix> &matrices,
    std::set<core::PartitionId> &declined) const {
  const auto discount_raw =
      core::ScaleToRaw(input.discount_raw, input.discount_scale, 2);
  const auto quantity_raw =
      core::ScaleToRaw(input.quantity_raw, input.quantity_scale, 2);
  const auto extended_raw =
      core::ScaleToRaw(input.extended_price_raw, input.extended_price_scale, 2);
  const auto month_partition = core::TpchQ6Layout::MonthPartition(
      MonthKeyOf(input.shipdate_days));
  auto &matrix = matrices[month_partition];
  if (input.null_cell ||
      !::accelerator_codegen::Q6InDomain(discount_raw, quantity_raw)) {
    declined.insert(month_partition);
    return;
  }
  const auto contribution =
      ::accelerator_codegen::Q6RowContributionRaw(extended_raw, discount_raw);
  matrix.AddRawCell(static_cast<int8_t>(discount_raw), quantity_raw,
                    contribution, 1);
}

TpchQ6Module::MonthMatrices
TpchQ6Module::BuildMatrices(core::AuxEngineServices &services) const {
  return core::PartitionedBuild<MonthMatrices>(
      services, [] { return MonthMatrices{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ6LineitemInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ6LineitemInput>{input});
                                    });
      },
      [&](MonthMatrices &partial, const TpchQ6LineitemInput &input) {
        ApplyLineitemInput(input, partial.matrices, partial.declined);
      },
      [](MonthMatrices &into, MonthMatrices &&from) {
        for (const auto &[partition, matrix] : from.matrices)
          into.matrices[partition].AddMatrix(matrix);
        into.declined.merge(from.declined);
      });
}

void TpchQ6Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q6 requires a typed initial scan source");
  auto [matrices, declined] = BuildMatrices(services);
  for (auto &entry : matrices) {
    auto image = NewStateImage(KindOf(entry.first), KeyOf(entry.first));
    image->matrix = entry.second;
    image->matrix.PrefixInPlace();
    image->declined = declined.count(entry.first) != 0;
    builder.Write(core::StateImage::Make(
        core::TpchQ6Layout::kAccelerator, entry.first, 0,
        std::shared_ptr<const TpchQ6StateImage>(std::move(image))));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ6Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ6Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ6TxnBuffer &>(base_buffer);
  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ6LineitemDelta delta;
    if (!ReadLineitem(row, delta, sign)) {
      buffer.MarkRebuild();
      return;
    }
    AddDelta(buffer, delta);
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
      TpchQ6LineitemDelta old_delta, new_delta;
      if (!ReadLineitem(mutation.old_rows[i], old_delta, -1) ||
          !ReadLineitem(mutation.new_rows[i], new_delta, 1)) {
        buffer.MarkRebuild();
        return;
      }
      AddDelta(buffer, old_delta);
      AddDelta(buffer, new_delta);
    }
    break;
  }
}

// Rescans the given month partitions and publishes their images.
void TpchQ6Module::RebuildMonths(const std::vector<core::PartitionId> &partitions,
                                 core::StagedMaintenanceOutcome &outcome) const {
  std::set<int32_t> month_keys;
  for (const auto partition : partitions) {
    if (core::TpchQ6Layout::IsMonthPartition(partition))
      month_keys.insert(core::TpchQ6Layout::MonthKeyFromPartition(partition));
  }
  std::unordered_map<core::PartitionId, Q6RevenueMatrix> rebuilt;
  std::set<core::PartitionId> declined;
  if (initial_scan) {
    initial_scan->ScanLineitemsForMonths(month_keys, [&](const TpchQ6LineitemInput &input) {
      ApplyLineitemInput(input, rebuilt, declined);
    });
  }
  for (const auto partition : partitions) {
    auto image = NewStateImage(KindOf(partition), KeyOf(partition));
    const auto it = rebuilt.find(partition);
    if (it != rebuilt.end()) {
      image->matrix = it->second;
      image->matrix.PrefixInPlace();
    }
    image->declined = declined.count(partition) != 0;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ6Layout::kAccelerator, partition, 0,
        std::shared_ptr<const TpchQ6StateImage>(std::move(image))));
  }
  core::FinishRebuildPartition(outcome);
}

core::StagedMaintenanceOutcome
TpchQ6Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ6TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.Deltas().empty())
    return outcome;

  // Deltas grouped by month partition: indices sorted by (partition, index).
  const auto &deltas = buffer.Deltas();
  DeltaOrder order;
  order.reserve(deltas.size());
  for (uint32_t i = 0; i < deltas.size(); ++i)
    order.emplace_back(core::TpchQ6Layout::MonthPartition(deltas[i].month_key), i);
  std::sort(order.begin(), order.end());
  const auto starts =
      core::RunStarts(order, [](const auto &entry) { return entry.first; });
  std::vector<core::PartitionId> partitions; // unique, ascending
  for (std::size_t r = 0; r + 1 < starts.size(); ++r)
    partitions.push_back(order[starts[r]].first);

  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalPartitioned(
          deltas.size(), partitions.size(), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    RebuildMonths(partitions, outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta ||
      !ApplyToMonths(services, deltas, order, partitions, starts, outcome)) {
    core::FinishRebuildAll(outcome, decision);
  }
  return outcome;
}

bool TpchQ6Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ6Module::CollectQueryPartitions(
    const TpchQ6QueryParams &params,
    std::vector<core::PartitionId> &partitions) const {
  // Partitions produced by the walk below are already unique (the cursor only
  // advances), so no set is needed; sort once for the store.
  partitions.clear();
  {
    // Fast path (every TPC-H instance): a window that starts on the first of a
    // month and ends exactly twelve months later is twelve whole months.
    if (params.start_days == StartOfMonth(params.start_days) &&
        params.end_days == AddMonths(params.start_days, kWindowMonths)) {
      const int32_t first = MonthKeyOf(params.start_days);
      for (int32_t i = 0; i < kWindowMonths; ++i)
        partitions.push_back(core::TpchQ6Layout::MonthPartition(first + i));
      return; // ascending, unique
    }
  }
  int32_t cursor = params.start_days;
  const int32_t end = params.end_days;
  while (cursor < end) {
    if (cursor == StartOfMonth(cursor)) {
      const int32_t month_end = AddMonths(cursor, 1);
      if (month_end <= end) {
        partitions.push_back(core::TpchQ6Layout::MonthPartition(
            MonthKeyOf(cursor)));
        cursor = month_end;
        continue;
      }
    }
    partitions.push_back(core::TpchQ6Layout::DayPartition(cursor));
    ++cursor;
  }
  std::sort(partitions.begin(), partitions.end());
}

void TpchQ6Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  TpchQ6QueryParams params;
  if (!ParseTpchQ6QueryParams(arguments, params))
    return;
  thread_local std::vector<core::PartitionId> partitions;
  CollectQueryPartitions(params, partitions);
  // Common case (every month present): validity and all partitions in one
  // linearized observation. Otherwise the general path below.
  auto snapshot = services.StateStore().ReadHitPartitions(
      services.CurrentVisibility(), {Id(), {}}, partitions);
  if (!snapshot.available) {
    if (!services.StateStore()
             .CheckValidity({Id(), {}}, services.CurrentVisibility())
             .IsValid()) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), partitions);
    if (snapshot.partitions.empty()) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
  }
  // The images borrowed from the snapshot are exactly the query's partitions.
  // Resolve the cells once, prefetch every matrix, then two loads each.
  const auto plan = ::accelerator_codegen::Q6RangePlan::Make(
      params.discount_lower_raw, params.discount_upper_raw,
      params.quantity_upper_raw);
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ6Image(image);
    if (!typed || typed->declined) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    if (!plan.empty)
      plan.Prefetch(typed->matrix);
  }
  Q6AccT revenue = static_cast<Q6AccT>(0);
  if (!plan.empty) {
    const auto &images = snapshot.partitions;
    core::ParallelReduce(
        services, images.size(), kEvalGrain, revenue,
        [&](Q6AccT &sum, std::size_t begin, std::size_t end) {
          for (std::size_t i = begin; i < end; ++i)
            sum += plan.Eval(AsQ6Image(images[i])->matrix);
        },
        [](Q6AccT &into, Q6AccT &&from) { into += from; });
  }
  EmitResult(revenue, sink);
}

void TpchQ6Module::EmitResult(Q6AccT revenue, core::AuxRowSink &sink) const {
  static const core::OutputSchema kSchema{
      {{"revenue", core::AuxType::ScaledDecimal, false}}};
  sink.Begin(kSchema);
  core::AuxRow row;
  row.values.resize(1);
  row.values[0].type = core::AuxType::ScaledDecimal;
  row.values[0].is_null = false;
  row.values[0].payload = TpchQ6RevenueOutputValue(revenue);
  sink.WriteRow(row);
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ6Module::CalibrationWrites() const {
  return TpchQ6CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
