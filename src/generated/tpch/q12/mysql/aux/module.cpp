#include "tpch/q12/mysql/aux/module.hpp"
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

constexpr uint32_t kVersion = 2; // v2: priority image carries order_line_index
constexpr uint32_t kPriorityMagic = 0x50313251; // Q12P
constexpr uint32_t kDayMagic = 0x44313251;     // Q12D
constexpr int32_t kEpoch = 719468;

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

int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

void CivilFromDays(int32_t days, int32_t &year, unsigned &month, unsigned &day) {
  int32_t z = days + kEpoch;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int32_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
}

std::shared_ptr<const TpchQ12StateImage>
MakeStateImage(TpchQ12ImageKind kind, int32_t receipt_day,
               const std::shared_ptr<const TpchQ12State> &state) {
  auto image = std::make_shared<TpchQ12StateImage>();
  image->kind = kind;
  image->receipt_day = receipt_day;
  image->state = state;
  return image;
}

const TpchQ12StateImage *AsQ12Image(const core::StateImage &image,
                                    TpchQ12ImageKind kind) {
  const auto *payload = image.As<TpchQ12StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

void ApplyContribution(TpchQ12DayShipmodeStats &stats,
                       TpchQ12PriorityClass priority, int64_t sign) {
  switch (priority) {
  case TpchQ12PriorityClass::High:
    stats.high_line_count += sign;
    break;
  case TpchQ12PriorityClass::Low:
    stats.low_line_count += sign;
    break;
  default:
    break;
  }
}

void CleanupDayMode(TpchQ12State &state, int32_t receipt_day,
                    const std::string &shipmode) {
  auto day_it = state.day_partitions.find(receipt_day);
  if (day_it == state.day_partitions.end())
    return;
  auto mode_it = day_it->second.find(shipmode);
  if (mode_it == day_it->second.end())
    return;
  if (mode_it->second.high_line_count == 0 &&
      mode_it->second.low_line_count == 0)
    day_it->second.erase(mode_it);
  if (day_it->second.empty())
    state.day_partitions.erase(day_it);
}

void AdjustLineIndex(TpchQ12State &state, int64_t orderkey, int32_t receipt_day,
                     const std::string &shipmode, int64_t sign) {
  auto &by_day = state.order_line_index[orderkey];
  auto &by_mode = by_day[receipt_day];
  auto &count = by_mode[shipmode];
  count += sign;
  if (count == 0) {
    by_mode.erase(shipmode);
    if (by_mode.empty())
      by_day.erase(receipt_day);
    if (by_day.empty())
      state.order_line_index.erase(orderkey);
  } else if (count < 0) {
    // Defensive: index should never go negative under correct maintenance.
    by_mode.erase(shipmode);
    if (by_mode.empty())
      by_day.erase(receipt_day);
    if (by_day.empty())
      state.order_line_index.erase(orderkey);
  }
}

void CollectIndexedDays(const TpchQ12State &state, int64_t orderkey,
                        std::set<int32_t> &days) {
  const auto it = state.order_line_index.find(orderkey);
  if (it == state.order_line_index.end())
    return;
  for (const auto &day_entry : it->second)
    days.insert(day_entry.first);
}

// Move day_partition contributions for orderkey from old → new priority.
void ReclassifyOrderPriority(TpchQ12State &state, int64_t orderkey,
                            TpchQ12PriorityClass new_priority) {
  const auto old_priority = state.PriorityOf(orderkey);
  if (old_priority == new_priority) {
    state.SetPriority(orderkey, new_priority);
    return;
  }
  const auto index_it = state.order_line_index.find(orderkey);
  if (index_it != state.order_line_index.end()) {
    for (const auto &day_entry : index_it->second) {
      const int32_t day = day_entry.first;
      for (const auto &mode_entry : day_entry.second) {
        const auto &shipmode = mode_entry.first;
        const int64_t count = mode_entry.second;
        if (count == 0)
          continue;
        auto &stats = state.day_partitions[day][shipmode];
        ApplyContribution(stats, old_priority, -count);
        ApplyContribution(stats, new_priority, count);
        CleanupDayMode(state, day, shipmode);
      }
    }
  }
  state.SetPriority(orderkey, new_priority);
}

// Deletes first, then inserts — order updates net to the new priority.
void ApplyOrderDeltas(TpchQ12State &state,
                      const std::vector<TpchQ12OrderDelta> &deltas) {
  for (const auto &delta : deltas) {
    if (delta.sign < 0)
      ReclassifyOrderPriority(state, delta.orderkey,
                              TpchQ12PriorityClass::Absent);
  }
  for (const auto &delta : deltas) {
    if (delta.sign > 0)
      ReclassifyOrderPriority(state, delta.orderkey, delta.priority);
  }
}

// Qualifying lineitem ±1: always maintain reverse index; contribute iff priority
// is High/Low (Absent lines stay indexed for a later order insert).
void ApplyLineitemDelta(TpchQ12State &state, int64_t orderkey,
                        int32_t receipt_day, const std::string &shipmode,
                        int8_t sign) {
  AdjustLineIndex(state, orderkey, receipt_day, shipmode, sign);
  const auto priority = state.PriorityOf(orderkey);
  if (priority == TpchQ12PriorityClass::Absent ||
      priority == TpchQ12PriorityClass::Neither)
    return;
  auto &stats = state.day_partitions[receipt_day][shipmode];
  ApplyContribution(stats, priority, sign);
  CleanupDayMode(state, receipt_day, shipmode);
}

void StripIndexDays(TpchQ12State &state, const std::set<int32_t> &days) {
  for (auto order_it = state.order_line_index.begin();
       order_it != state.order_line_index.end();) {
    for (const auto day : days)
      order_it->second.erase(day);
    if (order_it->second.empty())
      order_it = state.order_line_index.erase(order_it);
    else
      ++order_it;
  }
}

bool ReadLineitem(const core::AuxRow &row, TpchQ12LineitemInput &out) {
  int64_t orderkey = 0;
  if (!IntCell(row, 0, orderkey) || !StringCell(row, 1, out.shipmode) ||
      !DateCell(row, 2, out.commitdate_days) ||
      !DateCell(row, 3, out.receiptdate_days) ||
      !DateCell(row, 4, out.shipdate_days))
    return false;
  out.orderkey = orderkey;
  return true;
}

// Matches ScanOrders: o_orderkey, o_orderpriority
bool ReadOrder(const core::AuxRow &row, int64_t &orderkey,
               std::string &orderpriority) {
  return IntCell(row, 0, orderkey) && StringCell(row, 1, orderpriority);
}

} // namespace

int32_t TpchQ12DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return DaysFromCivil(year, month, day);
}

int32_t TpchQ12AddMonths(int32_t days_since_epoch, int32_t months) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  const int total = year * 12 + static_cast<int>(month) - 1 + months;
  const int new_year = total / 12;
  const unsigned new_month = static_cast<unsigned>(total % 12 + 1);
  return DaysFromCivil(new_year, new_month, day);
}

TpchQ12PriorityClass TpchQ12ClassifyPriority(const std::string &priority) {
  if (priority == "1-URGENT" || priority == "2-HIGH")
    return TpchQ12PriorityClass::High;
  return TpchQ12PriorityClass::Low;
}

bool TpchQ12LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days,
                              int32_t shipdate_days) {
  return commitdate_days < receiptdate_days && shipdate_days < commitdate_days;
}

std::vector<uint8_t> EncodeTpchQ12OrderPriorities(const TpchQ12State &state) {
  ByteWriter writer;
  std::vector<int64_t> keys;
  for (std::size_t chunk_idx = 0; chunk_idx < state.order_priorities.chunks.size();
       ++chunk_idx) {
    const auto &chunk = state.order_priorities.chunks[chunk_idx];
    const int64_t base =
        static_cast<int64_t>(chunk_idx << core::DenseIdArray<uint8_t>::kShift);
    for (std::size_t slot = 0; slot < chunk.size(); ++slot) {
      if (chunk[slot] == 0)
        continue;
      keys.push_back(base + static_cast<int64_t>(slot));
    }
  }
  std::sort(keys.begin(), keys.end());
  core::WriteImageHeader(writer, kPriorityMagic, kVersion,
                         static_cast<uint32_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.U8(static_cast<uint8_t>(state.PriorityOf(key)));
  }

  // Reverse index: orderkey → (day, shipmode, count)*
  std::vector<int64_t> index_keys;
  index_keys.reserve(state.order_line_index.size());
  for (const auto &entry : state.order_line_index)
    index_keys.push_back(entry.first);
  std::sort(index_keys.begin(), index_keys.end());
  writer.U32(static_cast<uint32_t>(index_keys.size()));
  for (const auto orderkey : index_keys) {
    const auto &by_day = state.order_line_index.at(orderkey);
    uint32_t cell_count = 0;
    for (const auto &day_entry : by_day)
      cell_count += static_cast<uint32_t>(day_entry.second.size());
    writer.I64(orderkey);
    writer.U32(cell_count);
    for (const auto &day_entry : by_day) {
      for (const auto &mode_entry : day_entry.second) {
        writer.I32(day_entry.first);
        writer.String(mode_entry.first);
        writer.I64(mode_entry.second);
      }
    }
  }
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ12Day(int32_t receipt_day,
                                      const TpchQ12State &state) {
  const auto it = state.day_partitions.find(receipt_day);
  std::vector<std::string> shipmodes;
  if (it != state.day_partitions.end()) {
    for (const auto &entry : it->second) {
      if (entry.second.high_line_count != 0 || entry.second.low_line_count != 0)
        shipmodes.push_back(entry.first);
    }
    std::sort(shipmodes.begin(), shipmodes.end());
  }
  ByteWriter writer;
  core::WriteImageHeader(writer, kDayMagic, kVersion, static_cast<uint32_t>(shipmodes.size()));
  for (const auto &shipmode : shipmodes) {
    const auto &stats = it->second.at(shipmode);
    writer.String(shipmode);
    writer.I64(stats.high_line_count);
    writer.I64(stats.low_line_count);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ12OrderPriorities(const std::vector<uint8_t> &bytes,
                                    TpchQ12State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kPriorityMagic, kVersion, count))
    return false;
  state.order_priorities.Clear();
  state.order_line_index.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint8_t priority = 0;
    if (!reader.I64(key) || !reader.U8(priority))
      return false;
    state.SetPriority(key, static_cast<TpchQ12PriorityClass>(priority));
  }
  uint32_t index_orders = 0;
  if (!reader.U32(index_orders))
    return false;
  for (uint32_t i = 0; i < index_orders; ++i) {
    int64_t orderkey = 0;
    uint32_t cell_count = 0;
    if (!reader.I64(orderkey) || !reader.U32(cell_count))
      return false;
    for (uint32_t c = 0; c < cell_count; ++c) {
      int32_t day = 0;
      std::string shipmode;
      int64_t n = 0;
      if (!reader.I32(day) || !reader.String(shipmode) || !reader.I64(n))
        return false;
      if (n != 0)
        state.order_line_index[orderkey][day][std::move(shipmode)] = n;
    }
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ12Day(const std::vector<uint8_t> &bytes, int32_t receipt_day,
                      TpchQ12State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kDayMagic, kVersion, count))
    return false;
  auto &day = state.day_partitions[receipt_day];
  day.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string shipmode;
    TpchQ12DayShipmodeStats stats;
    if (!reader.String(shipmode) || !reader.I64(stats.high_line_count) ||
        !reader.I64(stats.low_line_count))
      return false;
    day.emplace(std::move(shipmode), stats);
  }
  return reader.offset == bytes.size();
}

bool FindTpchQ12OrderPriority(const std::vector<uint8_t> &bytes,
                              int64_t orderkey, TpchQ12PriorityClass &priority) {
  ByteReader header{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(header, kPriorityMagic, kVersion, count))
    return false;
  constexpr std::size_t kHeaderBytes = 12;
  constexpr std::size_t kRecordBytes = 9;
  const std::size_t priority_bytes =
      kHeaderBytes + static_cast<std::size_t>(count) * kRecordBytes;
  // v2 appends a reverse-index trailer after the fixed priority records.
  if (count > (std::numeric_limits<std::size_t>::max() - kHeaderBytes) /
                 kRecordBytes ||
      bytes.size() < priority_bytes)
    return false;
  std::size_t first = 0;
  std::size_t last = count;
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    ByteReader reader{bytes};
    reader.offset = kHeaderBytes + middle * kRecordBytes;
    int64_t key = 0;
    if (!reader.I64(key))
      return false;
    if (key < orderkey) {
      first = middle + 1;
      continue;
    }
    if (key > orderkey) {
      last = middle;
      continue;
    }
    uint8_t value = 0;
    if (!reader.U8(value))
      return false;
    priority = static_cast<TpchQ12PriorityClass>(value);
    return true;
  }
  return false;
}

bool TpchQ12TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ12TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
}
bool TpchQ12TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ12LineitemDelta> &TpchQ12TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ12OrderDelta> &TpchQ12TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ12TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
}
void TpchQ12TxnBuffer::AddDelta(TpchQ12LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ12TxnBuffer::AddOrderDelta(TpchQ12OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}

core::AcceleratorId TpchQ12Module::Id() const { return kId; }
std::vector<core::TableDependency> TpchQ12Module::Dependencies() const {
  return {{config.orders_table, {0, 1}, {0, 5}},
          {config.lineitem_table, {0, 1, 2, 3, 4}, {0, 14, 11, 12, 10}}};
}
std::unique_ptr<core::AuxTxnBuffer> TpchQ12Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ12TxnBuffer>();
}

void TpchQ12Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ12State>(state);
  builder.Write(core::StateImage::Make(
      core::TpchQ12Layout::kAccelerator,
      core::TpchQ12Layout::kOrderPriorities, 0,
      MakeStateImage(TpchQ12ImageKind::OrderPriorities, 0, typed_state)));
  for (const auto &entry : state.day_partitions) {
    builder.Write(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, static_cast<core::PartitionId>(entry.first),
        0, MakeStateImage(TpchQ12ImageKind::Day, entry.first, typed_state)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ12Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ12Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ12TxnBuffer &>(base_buffer);
  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0;
      std::string priority;
      if (!ReadOrder(row, orderkey, priority)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta(
          {orderkey, TpchQ12ClassifyPriority(priority), sign});
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
    return;
  }
  if (mutation.table != config.lineitem_table)
    return;
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ12LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    if (!TpchQ12LineitemQualifies(input.commitdate_days, input.receiptdate_days,
                                  input.shipdate_days))
      return;
    TpchQ12LineitemDelta delta;
    delta.orderkey = input.orderkey;
    delta.receipt_day = input.receiptdate_days;
    delta.shipmode = input.shipmode;
    delta.sign = sign;
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
      add(mutation.old_rows[i], -1);
      add(mutation.new_rows[i], 1);
    }
    break;
  }
}

core::StagedMaintenanceOutcome
TpchQ12Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ12TxnBuffer &>(base_buffer);
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
  if (buffer.Deltas().empty() && buffer.OrderDeltas().empty())
    return outcome;

  const auto total_deltas =
      buffer.Deltas().size() + buffer.OrderDeltas().size();

  const auto visibility = services.CurrentVisibility();
  const auto priority_snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ12Layout::kOrderPriorities});
  std::shared_ptr<TpchQ12State> working;
  core::Version priority_version = 0;
  for (const auto &image : priority_snapshot.partitions) {
    if (image.partition == core::TpchQ12Layout::kOrderPriorities) {
      const auto *typed = AsQ12Image(image, TpchQ12ImageKind::OrderPriorities);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      working = std::make_shared<TpchQ12State>(*typed->state);
      priority_version = image.version;
      break;
    }
  }
  if (!working) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  std::set<int32_t> touched_days;
  for (const auto &delta : buffer.Deltas())
    touched_days.insert(delta.receipt_day);
  for (const auto &delta : buffer.OrderDeltas())
    CollectIndexedDays(*working, delta.orderkey, touched_days);

  const bool can_rebuild_days = !touched_days.empty() && initial_scan;
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas),
       static_cast<uint64_t>(
           std::max<std::size_t>(1, touched_days.size())),
       true, can_rebuild_days, true, false, false,
       core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    if (!can_rebuild_days) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    // Net order priority changes before rescanning touched days.
    ApplyOrderDeltas(*working, buffer.OrderDeltas());
    StripIndexDays(*working, touched_days);
    for (const auto day : touched_days)
      working->day_partitions.erase(day);

    initial_scan->ScanLineitemsForReceiptDays(
        touched_days, [&](const TpchQ12LineitemInput &input) {
          if (!TpchQ12LineitemQualifies(input.commitdate_days,
                                        input.receiptdate_days,
                                        input.shipdate_days))
            return;
          ApplyLineitemDelta(*working, input.orderkey, input.receiptdate_days,
                             input.shipmode, 1);
        });
    // Lineitem deltas in this txn are already reflected in the table scan when
    // initial_scan sees committed+pending rows; AccumulateMutation buffers are
    // for ApplyDelta. For RebuildPartition after Stage, rescan is source of
    // truth for touched days — do not double-apply buffer.Deltas().

    for (const auto day : touched_days) {
      TpchQ12State day_state;
      const auto day_it = working->day_partitions.find(day);
      if (day_it != working->day_partitions.end())
        day_state.day_partitions.emplace(day, day_it->second);
      const auto typed_state =
          std::make_shared<const TpchQ12State>(std::move(day_state));
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ12Layout::kAccelerator,
          static_cast<core::PartitionId>(day), 0,
          MakeStateImage(TpchQ12ImageKind::Day, day, typed_state)));
    }
    const auto typed_priorities =
        std::static_pointer_cast<const TpchQ12State>(std::move(working));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator,
        core::TpchQ12Layout::kOrderPriorities, 0,
        MakeStateImage(TpchQ12ImageKind::OrderPriorities, 0, typed_priorities)));
    core::FinishRebuildPartition(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // ----- ApplyDelta: load touched day partitions into working state -----
  std::map<int32_t, core::Version> versions;
  if (!touched_days.empty()) {
    std::vector<core::PartitionId> day_partitions;
    day_partitions.reserve(touched_days.size());
    for (const auto day : touched_days)
      day_partitions.push_back(static_cast<core::PartitionId>(day));

    const auto day_snapshot =
        services.StateStore().ReadPartitions(visibility, day_partitions);
    for (const auto &image : day_snapshot.partitions) {
      const auto day = static_cast<int32_t>(image.partition);
      if (day == static_cast<int32_t>(core::TpchQ12Layout::kOrderPriorities))
        continue;
      const auto *typed = AsQ12Image(image, TpchQ12ImageKind::Day);
      if (!typed || typed->receipt_day != day) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      const auto day_it = typed->state->day_partitions.find(day);
      if (day_it != typed->state->day_partitions.end())
        working->day_partitions[day] = day_it->second;
      else
        working->day_partitions.erase(day);
      versions[day] = image.version;
    }
    // Missing partitions are empty days (version 0); OK for first write to a day.
  }

  ApplyOrderDeltas(*working, buffer.OrderDeltas());
  for (const auto &delta : buffer.Deltas()) {
    ApplyLineitemDelta(*working, delta.orderkey, delta.receipt_day,
                       delta.shipmode, delta.sign);
  }

  // Order reclassify may touch additional days already collected; re-collect
  // after mutations in case lineitem deltas added new days.
  for (const auto &delta : buffer.Deltas())
    touched_days.insert(delta.receipt_day);
  for (const auto &delta : buffer.OrderDeltas())
    CollectIndexedDays(*working, delta.orderkey, touched_days);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  for (const auto day : touched_days) {
    const auto partition = static_cast<core::PartitionId>(day);
    const auto version = versions.count(day) ? versions[day] : 0;
    TpchQ12State day_state;
    const auto day_it = working->day_partitions.find(day);
    if (day_it != working->day_partitions.end())
      day_state.day_partitions.emplace(day, day_it->second);
    const auto typed_state = std::make_shared<const TpchQ12State>(day_state);
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, partition, version,
        MakeStateImage(TpchQ12ImageKind::Day, day, typed_state)));
  }
  const auto typed_priorities =
      std::static_pointer_cast<const TpchQ12State>(std::move(working));
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ12Layout::kAccelerator, core::TpchQ12Layout::kOrderPriorities,
      priority_version,
      MakeStateImage(TpchQ12ImageKind::OrderPriorities, 0, typed_priorities)));
  return outcome;
}

bool TpchQ12Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ12Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  std::string shipmode1, shipmode2;
  int32_t start = 0;
  if (arguments.values.size() < 3)
    return ExecuteFallback(services, arguments, sink);
  if (!arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    shipmode1 = std::get<std::string>(arguments.values[0].payload);
  if (!arguments.values[1].is_null &&
      std::holds_alternative<std::string>(arguments.values[1].payload))
    shipmode2 = std::get<std::string>(arguments.values[1].payload);
  if (!arguments.values[2].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[2].payload))
      start =
          std::get<core::AuxDate>(arguments.values[2].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[2].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[2].payload));
  }
  const auto end = TpchQ12AddMonths(start, 12);
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  TpchQ12State state;
  if (!LoadState(services, state, start, end)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(state, start, end, shipmode1, shipmode2, sink);
}

void TpchQ12Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  std::string shipmode1, shipmode2;
  int32_t start = 0;
  if (arguments.values.size() >= 1 && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    shipmode1 = std::get<std::string>(arguments.values[0].payload);
  if (arguments.values.size() >= 2 && !arguments.values[1].is_null &&
      std::holds_alternative<std::string>(arguments.values[1].payload))
    shipmode2 = std::get<std::string>(arguments.values[1].payload);
  if (arguments.values.size() >= 3 && !arguments.values[2].is_null &&
      std::holds_alternative<core::AuxDate>(arguments.values[2].payload))
    start =
        std::get<core::AuxDate>(arguments.values[2].payload).days_since_epoch;
  EmitResult(BuildState(services), start, TpchQ12AddMonths(start, 12), shipmode1,
             shipmode2, sink);
}

TpchQ12State TpchQ12Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ12State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q12 requires a typed initial scan source");
  initial_scan->ScanOrders([&](const TpchQ12OrderInput &input) {
    state.SetPriority(input.key, TpchQ12ClassifyPriority(input.orderpriority));
  });
  initial_scan->ScanLineitems([&](const TpchQ12LineitemInput &input) {
    if (!TpchQ12LineitemQualifies(input.commitdate_days, input.receiptdate_days,
                                  input.shipdate_days))
      return;
    // Index every qualifying lineitem; contribute when the order is present.
    ApplyLineitemDelta(state, input.orderkey, input.receiptdate_days,
                       input.shipmode, 1);
  });
  return state;
}

bool TpchQ12Module::LoadState(core::AuxEngineServices &services,
                              TpchQ12State &state, int32_t start_days,
                              int32_t end_days) const {
  std::vector<core::PartitionId> partitions;
  for (int32_t day = start_days; day < end_days; ++day)
    partitions.push_back(static_cast<core::PartitionId>(day));
  const auto snapshot =
      services.StateStore().ReadPartitions(services.CurrentVisibility(),
                                          partitions);
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ12Layout::kOrderPriorities)
      continue;
    const auto receipt_day = static_cast<int32_t>(image.partition);
    const auto *typed = AsQ12Image(image, TpchQ12ImageKind::Day);
    if (!typed || typed->receipt_day != receipt_day)
      return false;
    const auto day_it = typed->state->day_partitions.find(receipt_day);
    if (day_it != typed->state->day_partitions.end())
      state.day_partitions[receipt_day] = day_it->second;
  }
  return true;
}

void TpchQ12Module::EmitResult(const TpchQ12State &state, int32_t start_days,
                               int32_t end_days, const std::string &shipmode1,
                               const std::string &shipmode2,
                               core::AuxRowSink &sink) const {
  TpchQ12FinalizeAndEmit(state, start_days, end_days, shipmode1, shipmode2,
                         sink);
}

} // namespace accelerator_codegen::aux_state::query
