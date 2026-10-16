#include "tpch/q12/duckdb/aux/module.hpp"
#include "tpch/q12/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <stdexcept>
#include <utility>
#include "tpch/q12/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 2;
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

const TpchQ12StateImage *AsQ12Image(const core::StateImage &image,
                                    TpchQ12ImageKind kind) {
  const auto *payload = image.As<TpchQ12StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

// Below this many tree contributions a commit applies its levels inline.
constexpr std::size_t kLevelApplyMinContributions = 2048;

// Dyadic tree over receipt days, implicit heap numbering: node 1 is the root,
// leaves are kTreeN + (day - kTreeBase). Days outside [kTreeBase,
// kTreeBase + kTreeN) live in one overflow partition. All partition ids stay
// well below 2^31 and never collide with kOrderPriorities (0).
constexpr int kTreeBits = 18;
constexpr int64_t kTreeN = int64_t(1) << kTreeBits;
constexpr int32_t kTreeBase = -(1 << (kTreeBits - 1));
constexpr core::PartitionId kOverflowPartition =
    static_cast<core::PartitionId>(1) << (kTreeBits + 2);
static_assert(kOverflowPartition < (int64_t(1) << 31), "int32 partition keys");

bool InTree(int32_t day) {
  return day >= kTreeBase && static_cast<int64_t>(day) - kTreeBase < kTreeN;
}
int64_t LeafNode(int32_t day) {
  return kTreeN + (static_cast<int64_t>(day) - kTreeBase);
}

// Adds (shipmode, high, low) for `day` to the leaf-to-root path (or overflow)
// of a map of nodes (Build only; maintenance uses the flat level walk).
void AddToTree(std::map<core::PartitionId, TpchQ12Node> &nodes, int32_t day,
               const std::string &shipmode, int64_t high, int64_t low) {
  const int slot = TpchQ12ModeSlot(shipmode);
  if (!InTree(day)) {
    nodes[kOverflowPartition].overflow_days[day].Add(slot, shipmode, high, low);
    return;
  }
  for (int64_t n = LeafNode(day); n >= 1; n >>= 1)
    nodes[static_cast<core::PartitionId>(n)].modes.Add(slot, shipmode, high,
                                                        low);
}

// A fresh node image (the node is stored inline in the image).
std::shared_ptr<TpchQ12StateImage> NewNodeImage() {
  auto image = std::make_shared<TpchQ12StateImage>();
  image->kind = TpchQ12ImageKind::Node;
  return image;
}

const TpchQ12Node *AsNodeImage(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ12StateImage>();
  return payload && payload->kind == TpchQ12ImageKind::Node ? &payload->node
                                                            : nullptr;
}

// dst += src (dense slots, plus extra names).
void MergeModes(TpchQ12Modes &dst, const TpchQ12Modes &src) {
  for (int i = 0; i < kQ12ModeSlots; ++i) {
    dst.high[i] += src.high[i];
    dst.low[i] += src.low[i];
  }
  for (const auto &entry : src.extra)
    dst.Add(-1, entry.shipmode, entry.high_line_count, entry.low_line_count);
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

void ApplyContribution(TpchQ12DayShipmodeStats &stats,
                       TpchQ12PriorityClass priority, int8_t sign) {
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
  std::vector<int64_t> keys; // ascending (ForEachOrdered walks in key order)
  state.order_priorities.ForEachOrdered([&](int64_t key, uint8_t v) {
    if (v != 0)
      keys.push_back(key);
  });
  core::WriteImageHeader(writer, kPriorityMagic, kVersion, static_cast<uint64_t>(keys.size()));
  for (const auto key : keys) {
    writer.I64(key);
    writer.U8(static_cast<uint8_t>(state.PriorityOf(key)));
    writer.U16(state.line_counts.Get(key));
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
  core::WriteImageHeader(writer, kDayMagic, kVersion, static_cast<uint64_t>(shipmodes.size()));
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
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kPriorityMagic, kVersion, count))
    return false;
  state.order_priorities = decltype(state.order_priorities)();
  state.line_counts = decltype(state.line_counts)();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    uint8_t priority = 0;
    uint16_t count = 0;
    if (!reader.I64(key) || !reader.U8(priority) || !reader.U16(count))
      return false;
    state.SetPriority(key, static_cast<TpchQ12PriorityClass>(priority));
    state.line_counts.Set(key, count);
  }
  return reader.offset == bytes.size();
}

bool DecodeTpchQ12Day(const std::vector<uint8_t> &bytes, int32_t receipt_day,
                      TpchQ12State &state) {
  ByteReader reader{bytes};
  uint64_t count = 0;
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
  uint64_t count = 0;
  if (!core::ReadImageHeader(header, kPriorityMagic, kVersion, count))
    return false;
  constexpr std::size_t kHeaderBytes = 12;
  constexpr std::size_t kRecordBytes = 11;
  if (count > (std::numeric_limits<std::size_t>::max() - kHeaderBytes) /
                 kRecordBytes ||
      bytes.size() != kHeaderBytes +
                          static_cast<std::size_t>(count) * kRecordBytes)
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
  auto state_owned = BuildState(services);
  std::map<core::PartitionId, TpchQ12Node> nodes;
  for (const auto &day : state_owned.day_partitions)
    for (const auto &entry : day.second)
      AddToTree(nodes, day.first, entry.first, entry.second.high_line_count,
                entry.second.low_line_count);
  state_owned.day_partitions.clear();
  const auto typed_state =
      std::make_shared<const TpchQ12State>(std::move(state_owned));
  builder.Write(core::StateImage::Make(
      core::TpchQ12Layout::kAccelerator,
      core::TpchQ12Layout::kOrderPriorities, 0,
      MakeStateImage(TpchQ12ImageKind::OrderPriorities, 0, typed_state)));
  for (auto &entry : nodes) {
    auto image = NewNodeImage();
    image->node = std::move(entry.second);
    builder.Write(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, entry.first, 0,
        std::shared_ptr<const TpchQ12StateImage>(std::move(image))));
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

  const auto visibility = services.CurrentVisibility();
  const auto priority_snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ12Layout::kOrderPriorities});
  // Working copy: shares all pages with the published image (cheap page-table
  // copy); only pages actually written are cloned. day_partitions is left
  // empty so the new priority image does not carry any day data.
  std::shared_ptr<TpchQ12State> priority_state;
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
      priority_state = std::make_shared<TpchQ12State>();
      priority_state->order_priorities = typed->state->order_priorities;
      priority_state->line_counts = typed->state->line_counts;
      priority_version = image.version;
      break;
    }
  }
  if (!priority_state) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Order changes. A key with exactly one removal and one insertion is an
  // in-place update: same class (or untracked columns only) is a no-op; a class
  // change moves that order's lineitems between classes (looked up in the base
  // table below). Anything more tangled is a rebuild. Deltas are grouped by key
  // with one stable sort (per-key order is preserved).
  const auto &order_deltas = buffer.OrderDeltas();
  std::vector<uint32_t> by_key(order_deltas.size());
  for (uint32_t i = 0; i < by_key.size(); ++i)
    by_key[i] = i;
  std::stable_sort(by_key.begin(), by_key.end(), [&](uint32_t l, uint32_t r) {
    return order_deltas[l].orderkey < order_deltas[r].orderkey;
  });
  std::vector<int64_t> removed_orders; // ascending
  struct Changed {
    int64_t key;
    TpchQ12PriorityClass from;
    TpchQ12PriorityClass to;
  };
  std::vector<Changed> changed_orders; // ascending by key
  auto find_changed = [&](int64_t key) -> const Changed * {
    if (changed_orders.empty())
      return nullptr;
    const auto it = std::lower_bound(
        changed_orders.begin(), changed_orders.end(), key,
        [](const Changed &c, int64_t k) { return c.key < k; });
    return it != changed_orders.end() && it->key == key ? &*it : nullptr;
  };

  bool priorities_changed = false;
  for (std::size_t i = 0; i < by_key.size();) {
    std::size_t j = i;
    const int64_t key = order_deltas[by_key[i]].orderkey;
    const TpchQ12OrderDelta *ins = nullptr, *rem = nullptr;
    std::size_t ins_n = 0, rem_n = 0;
    for (; j < by_key.size() && order_deltas[by_key[j]].orderkey == key; ++j) {
      const auto &d = order_deltas[by_key[j]];
      if (d.sign > 0) {
        ins = &d;
        ++ins_n;
      } else {
        rem = &d;
        ++rem_n;
      }
    }
    if (ins_n == 0) {
      removed_orders.push_back(key);
    } else if (rem_n == 0) {
      // Inserted order: the class of its last inserted row wins.
      priority_state->SetPriority(key, ins->priority);
      priorities_changed = true;
    } else {
      if (ins_n != 1 || rem_n != 1) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      if (ins->priority != rem->priority) {
        if (priority_state->PriorityOf(key) != rem->priority) {
          core::FinishRebuildAll(outcome);
          return outcome;
        }
        changed_orders.push_back({key, rem->priority, ins->priority});
      }
    }
    i = j;
  }

  // Class-changing updates: the order's current qualifying lineitems S.
  // Published state holds (S - inserted + deleted by this txn) under the old
  // class; the final state needs S under the new class. The per-delta term
  // (+-1 under the old class) is added in the node-delta loop below.
  std::vector<std::tuple<int64_t, int32_t, std::string>> changed_lines;
  if (!changed_orders.empty()) {
    std::vector<int64_t> keys;
    for (const auto &entry : changed_orders)
      keys.push_back(entry.key);
    if (!initial_scan ||
        !initial_scan->LookupQualifyingLineitems(
            keys, [&](int64_t key, int32_t day, const std::string &mode) {
              changed_lines.emplace_back(key, day, mode);
            })) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    std::map<int64_t, std::size_t> line_n;
    for (const auto &line : changed_lines)
      ++line_n[std::get<0>(line)];
    for (const auto &entry : changed_orders) {
      const auto n = line_n[entry.key];
      if (n > std::numeric_limits<uint16_t>::max()) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      priority_state->SetPriority(entry.key, entry.to);
      priority_state->line_counts.Set(entry.key, static_cast<uint16_t>(n));
    }
    priorities_changed = true;
  }

  // Track per-order qualifying-lineitem counts so an order removal can be
  // applied as a delta when none of its lineitems remain counted.
  for (const auto &delta : buffer.Deltas()) {
    if (find_changed(delta.orderkey))
      continue; // count set from the base-table lookup
    const auto priority = priority_state->PriorityOf(delta.orderkey);
    if (priority == TpchQ12PriorityClass::Absent) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    if (priority == TpchQ12PriorityClass::Neither)
      continue;
    const int64_t count =
        static_cast<int64_t>(priority_state->line_counts.Get(delta.orderkey)) +
        delta.sign;
    if (count < 0 || count > std::numeric_limits<uint16_t>::max()) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    priority_state->line_counts.Set(delta.orderkey,
                                    static_cast<uint16_t>(count));
    priorities_changed = true;
  }
  for (const auto key : removed_orders) {
    if (priority_state->line_counts.Get(key) != 0) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
  }
  const auto total_deltas =
      buffer.Deltas().size() + buffer.OrderDeltas().size() +
      changed_lines.size();

  std::size_t touched_days = 0;
  {
    std::vector<int32_t> days;
    days.reserve(buffer.Deltas().size());
    for (const auto &delta : buffer.Deltas())
      days.push_back(delta.receipt_day);
    std::sort(days.begin(), days.end());
    touched_days = static_cast<std::size_t>(
        std::unique(days.begin(), days.end()) - days.begin());
  }

  // Always apply as a delta: the lineitem deltas move leaf-to-root paths of the
  // dyadic tree. (No partition rebuild: it would need a diff against the old
  // leaf to fix the ancestors.)
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_deltas),
       static_cast<uint64_t>(std::max<std::size_t>(
           1, touched_days + (priorities_changed ? 1 : 0))),
       true, false, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta &&
      decision.action != core::MaintenanceAction::RebuildPartition) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (!buffer.Deltas().empty() || !changed_lines.empty()) {
    // Net contribution of every line to the tree. Lines in the tree domain
    // are keyed by their leaf node; the rest go to the overflow partition.
    struct Contribution {
      int64_t leaf;
      int32_t day;
      int slot; // dense mode slot, -1: look up by name
      const std::string *mode;
      int64_t high;
      int64_t low;
    };
    std::vector<Contribution> tree, overflow;
    tree.reserve(buffer.Deltas().size() + changed_lines.size());
    auto add = [&](int32_t day, const std::string &mode,
                   TpchQ12PriorityClass cls, int64_t amount) {
      if (cls != TpchQ12PriorityClass::High && cls != TpchQ12PriorityClass::Low)
        return;
      Contribution c{InTree(day) ? LeafNode(day) : 0, day,
                     TpchQ12ModeSlot(mode), &mode,
                     cls == TpchQ12PriorityClass::High ? amount : 0,
                     cls == TpchQ12PriorityClass::Low ? amount : 0};
      (InTree(day) ? tree : overflow).push_back(c);
    };
    for (const auto &line : changed_lines) {
      const auto *cls = find_changed(std::get<0>(line));
      add(std::get<1>(line), std::get<2>(line), cls->from, -1);
      add(std::get<1>(line), std::get<2>(line), cls->to, +1);
    }
    for (const auto &delta : buffer.Deltas()) {
      if (const auto *changed = find_changed(delta.orderkey)) {
        add(delta.receipt_day, delta.shipmode, changed->from, delta.sign);
        continue;
      }
      add(delta.receipt_day, delta.shipmode,
          priority_state->PriorityOf(delta.orderkey), delta.sign);
    }
    std::sort(tree.begin(), tree.end(),
              [](const Contribution &l, const Contribution &r) {
                return l.leaf < r.leaf;
              });

    // Ancestors per level (leaf level 0 .. root level kTreeBits): because the
    // contributions are visited in leaf order, each level is a sorted run and
    // each contribution finds its node with a forward-only cursor.
    constexpr int kLevels = kTreeBits + 1;
    std::vector<core::PartitionId> partitions;
    partitions.reserve(tree.size() * 4 + kLevels + 1);
    uint32_t level_begin[kLevels];
    for (int level = 0; level < kLevels; ++level) {
      level_begin[level] = static_cast<uint32_t>(partitions.size());
      int64_t last = 0;
      for (const auto &c : tree) {
        const int64_t node = c.leaf >> level;
        if (node != last) {
          partitions.push_back(static_cast<core::PartitionId>(node));
          last = node;
        }
      }
    }
    const std::size_t tree_slots = partitions.size();
    if (!overflow.empty())
      partitions.push_back(kOverflowPartition);

    std::vector<std::shared_ptr<TpchQ12StateImage>> work(partitions.size());
    std::vector<core::Version> versions(partitions.size(), 0);
    if (!partitions.empty()) {
      const auto snapshot =
          services.StateStore().ReadPartitions(visibility, partitions);
      std::vector<const core::StateImage *> matched;
      MatchSnapshot(partitions, snapshot.partitions, matched);
      for (std::size_t slot = 0; slot < partitions.size(); ++slot) {
        auto image = NewNodeImage();
        if (matched[slot]) {
          const auto *node = AsNodeImage(*matched[slot]);
          if (!node) {
            outcome.action = core::MaintenanceAction::RebuildAll;
            outcome.invalidations = All();
            outcome.pending_rebuild = outcome.invalidations;
            return outcome;
          }
          image->node = *node;
          versions[slot] = matched[slot]->version;
        }
        work[slot] = std::move(image);
      }
    }
    // Levels own disjoint partitions, so each is applied by one slice.
    const std::size_t level_grain =
        tree.size() < kLevelApplyMinContributions ? kLevels : 1;
    core::ParallelSlices(
        services, kLevels, level_grain,
        [&](std::size_t, std::size_t first, std::size_t last) {
          for (std::size_t level = first; level < last; ++level) {
            uint32_t cursor = level_begin[level];
            for (const auto &c : tree) {
              if (c.leaf >> level != static_cast<int64_t>(partitions[cursor]))
                ++cursor;
              work[cursor]->node.modes.Add(c.slot, *c.mode, c.high, c.low);
            }
          }
        });
    if (!overflow.empty()) {
      auto &days = work[tree_slots]->node.overflow_days;
      for (const auto &c : overflow)
        days[c.day].Add(c.slot, *c.mode, c.high, c.low);
      for (auto it = days.begin(); it != days.end();)
        it = it->second.Empty() ? days.erase(it) : std::next(it);
    }
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.reserve(partitions.size() + 1);
    for (std::size_t slot = 0; slot < partitions.size(); ++slot)
      outcome.updates.push_back(core::StateImage::Make(
          core::TpchQ12Layout::kAccelerator, partitions[slot], versions[slot],
          std::shared_ptr<const TpchQ12StateImage>(std::move(work[slot]))));
  }

  for (const auto key : removed_orders) {
    priority_state->SetPriority(key, TpchQ12PriorityClass::Absent);
    priorities_changed = true;
  }

  if (priorities_changed) {
    outcome.action = core::MaintenanceAction::ApplyDelta;
    const auto typed_priorities =
        std::static_pointer_cast<const TpchQ12State>(std::move(priority_state));
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ12Layout::kAccelerator, core::TpchQ12Layout::kOrderPriorities,
        priority_version,
        MakeStateImage(TpchQ12ImageKind::OrderPriorities, 0, typed_priorities)));
  }
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
  static const std::string kEmpty;
  const std::string *shipmode1 = &kEmpty, *shipmode2 = &kEmpty;
  int32_t start = 0;
  if (arguments.values.size() < 3)
    return ExecuteFallback(services, arguments, sink);
  if (!arguments.values[0].is_null)
    if (const auto *text =
            std::get_if<std::string>(&arguments.values[0].payload))
      shipmode1 = text;
  if (!arguments.values[1].is_null)
    if (const auto *text =
            std::get_if<std::string>(&arguments.values[1].payload))
      shipmode2 = text;
  if (!arguments.values[2].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[2].payload))
      start =
          std::get<core::AuxDate>(arguments.values[2].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[2].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[2].payload));
  }
  const auto end = TpchQ12AddMonths(start, 12);
  // Decompose the window into aligned tree nodes (bottom-up segment tree).
  thread_local std::vector<core::PartitionId> partitions;
  partitions.clear();
  const int64_t lo = std::max<int64_t>(start, kTreeBase);
  const int64_t hi = std::min<int64_t>(end, int64_t(kTreeBase) + kTreeN);
  if (lo < hi) {
    int64_t l = lo - kTreeBase + kTreeN, r = hi - kTreeBase + kTreeN;
    while (l < r) {
      if (l & 1)
        partitions.push_back(static_cast<core::PartitionId>(l++));
      if (r & 1)
        partitions.push_back(static_cast<core::PartitionId>(--r));
      l >>= 1;
      r >>= 1;
    }
  }
  const bool outside = start < kTreeBase || end > int64_t(kTreeBase) + kTreeN;
  if (outside)
    partitions.push_back(kOverflowPartition);
  // Common case (every node present): validity and all partitions in one
  // linearized observation. Empty nodes are absent, so otherwise the general
  // path: validity, then whatever exists.
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
  }
  // At most two nodes per level plus the overflow sum.
  constexpr std::size_t kMaxParts = 2 * (kTreeBits + 1) + 2;
  const TpchQ12Modes *parts[kMaxParts];
  std::size_t part_count = 0;
  TpchQ12Modes overflow_sum;
  for (const auto &image : snapshot.partitions) {
    const auto *node = AsNodeImage(image);
    if (!node || part_count >= kMaxParts - 1) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    if (image.partition == kOverflowPartition) {
      for (const auto &day : node->overflow_days) {
        if (day.first < start || day.first >= end)
          continue;
        MergeModes(overflow_sum, day.second);
      }
    } else {
      parts[part_count++] = &node->modes;
    }
  }
  if (outside)
    parts[part_count++] = &overflow_sum;
  TpchQ12FinalizeAndEmit(parts, part_count, *shipmode1, *shipmode2, sink);
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
  const auto state = BuildState(services);
  const auto end = TpchQ12AddMonths(start, 12);
  TpchQ12Modes window;
  for (const auto &day : state.day_partitions) {
    if (day.first < start || day.first >= end)
      continue;
    for (const auto &entry : day.second)
      window.Add(TpchQ12ModeSlot(entry.first), entry.first,
                 entry.second.high_line_count, entry.second.low_line_count);
  }
  const TpchQ12Modes *parts[1] = {&window};
  TpchQ12FinalizeAndEmit(parts, 1, shipmode1, shipmode2, sink);
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
    const auto priority = state.PriorityOf(input.orderkey);
    if (priority == TpchQ12PriorityClass::Absent)
      return;
    if (priority == TpchQ12PriorityClass::Neither)
      return;
    if (!TpchQ12LineitemQualifies(input.commitdate_days, input.receiptdate_days,
                                  input.shipdate_days))
      return;
    auto &stats = state.day_partitions[input.receiptdate_days][input.shipmode];
    ApplyContribution(stats, priority, 1);
    const auto count = state.line_counts.Get(input.orderkey);
    if (count != std::numeric_limits<uint16_t>::max())
      state.line_counts.Set(input.orderkey, static_cast<uint16_t>(count + 1));
  });
  return state;
}

std::vector<core::CalibrationWrite> TpchQ12Module::CalibrationWrites() const {
  return TpchQ12CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
