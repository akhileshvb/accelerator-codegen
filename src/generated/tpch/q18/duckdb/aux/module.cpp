#include "tpch/q18/duckdb/aux/module.hpp"
#include "tpch/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kQuantitySumsMagic = 0x51313851; // Q18Q
constexpr uint32_t kVersionV1 = 1;
constexpr uint32_t kVersionV2 = 2;
constexpr uint32_t kVersionV3 = 3;

// ── Cell extraction helpers ─────────────────────────────────────────────────
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

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &quantity_raw) {
  if (!IntCell(row, 0, orderkey))
    return false;
  int64_t raw = 0;
  int32_t scale = 0;
  if (!RawDecimalCell(row, 1, raw, scale))
    return false;
  if (scale != 2)
    return false;
  quantity_raw = raw;
  return true;
}

std::shared_ptr<const TpchQ18StateImage>
MakeStateImage(TpchQ18ImageKind kind,
               const std::shared_ptr<const TpchQ18State> &state) {
  auto image = std::make_shared<TpchQ18StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const TpchQ18StateImage *AsQ18Image(const core::StateImage &image,
                                    TpchQ18ImageKind kind) {
  const auto *payload = image.As<TpchQ18StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool BetterOutputRow(const TpchQ18OutputRow &lhs, const TpchQ18OutputRow &rhs) {
  if (lhs.totalprice_raw != rhs.totalprice_raw)
    return lhs.totalprice_raw > rhs.totalprice_raw;
  return lhs.orderdate_days < rhs.orderdate_days;
}

void CollectSparseQuantities(
    const TpchQ18State &state,
    std::vector<std::pair<int64_t, int64_t>> &entries) {
  entries.clear();
  entries.reserve(state.quantity_overflow.size());
  for (const auto &entry : state.quantity_overflow) {
    if (entry.second != 0)
      entries.emplace_back(entry.first, entry.second);
  }
  std::sort(entries.begin(), entries.end());
}

void RebuildActiveOrderkeys(TpchQ18State &state) {
  state.active_orderkeys.clear();
  state.active_orderkeys.reserve(state.survivors.size());
  for (const auto &[orderkey, _] : state.survivors)
    state.active_orderkeys.push_back(orderkey);
}

void UpdateActiveOrderkey(TpchQ18State &state, int64_t orderkey) {
  const bool should_be_active = state.HasOrderMeta(orderkey);
  auto &keys = state.active_orderkeys;
  auto it = std::find(keys.begin(), keys.end(), orderkey);
  if (should_be_active) {
    if (it == keys.end())
      keys.push_back(orderkey);
    return;
  }
  if (it != keys.end()) {
    *it = keys.back();
    keys.pop_back();
  }
}

// Per-order quantity sums from the tables. The state holds only orders above
// kMinPersistQuantityRaw, so a lower threshold needs these.
std::unordered_map<int64_t, int64_t>
ScanOrderQuantitySums(TpchQ18InitialScan *initial_scan) {
  std::unordered_map<int64_t, int64_t> sums;
  if (!initial_scan->ScanOrderQuantityAggregates(
          [&](const TpchQ18OrderQuantityAggregate &agg) {
            sums[agg.orderkey] += agg.quantity_sum_raw;
          })) {
    initial_scan->ScanLineitems([&](const TpchQ18LineitemInput &input) {
      sums[input.orderkey] += input.quantity_raw;
    });
  }
  return sums;
}

void EmitResultScanOrders(const TpchQ18State &state, int64_t threshold_raw,
                          TpchQ18InitialScan *initial_scan,
                          core::AuxRowSink &sink) {
  const auto table_sums =
      threshold_raw < TpchQ18State::kMinPersistQuantityRaw
          ? ScanOrderQuantitySums(initial_scan)
          : std::unordered_map<int64_t, int64_t>{};
  const auto sum_of = [&](int64_t orderkey) -> int64_t {
    if (threshold_raw >= TpchQ18State::kMinPersistQuantityRaw)
      return state.OrderQuantitySum(orderkey);
    const auto it = table_sums.find(orderkey);
    return it == table_sums.end() ? 0 : it->second;
  };
  struct OrderMeta {
    int64_t custkey;
    int32_t orderdate_days;
    int64_t totalprice_raw;
  };
  std::unordered_map<int64_t, OrderMeta> orders;
  std::unordered_set<int64_t> needed_custkeys;

  initial_scan->ScanOrders([&](const TpchQ18OrderInput &input) {
    if (input.key < 0)
      return;
    const int64_t sum = sum_of(input.key);
    if (sum <= threshold_raw)
      return;
    orders.emplace(input.key,
                   OrderMeta{input.custkey, input.orderdate_days,
                             input.totalprice_raw});
    needed_custkeys.insert(input.custkey);
  });

  if (orders.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  std::unordered_map<int64_t, std::string> customer_names;
  initial_scan->ScanCustomers([&](const TpchQ18CustomerInput &input) {
    if (needed_custkeys.find(input.key) == needed_custkeys.end())
      return;
    customer_names.emplace(input.key, input.name);
  });

  std::vector<TpchQ18OutputRow> output_rows;
  output_rows.reserve(orders.size());
  for (const auto &[orderkey, meta] : orders) {
    const auto name_it = customer_names.find(meta.custkey);
    if (name_it == customer_names.end())
      continue;
    TpchQ18OutputRow row;
    row.name = name_it->second;
    row.custkey = meta.custkey;
    row.orderkey = orderkey;
    row.orderdate_days = meta.orderdate_days;
    row.totalprice_raw = meta.totalprice_raw;
    row.quantity_sum_raw = sum_of(orderkey);
    output_rows.push_back(std::move(row));
  }

  std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(6);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.custkey));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.orderkey));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Date, core::AuxDate{row.orderdate_days}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           core::ScaledDecimal{row.totalprice_raw, 2}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           core::ScaledDecimal{row.quantity_sum_raw, 2}));
    sink.WriteRow(out);
  }
  sink.End();
}

void AppendNonSurvivorRows(const TpchQ18State &state, int64_t threshold_raw,
                           TpchQ18InitialScan *initial_scan,
                           const std::unordered_set<int64_t> &skip_orderkeys,
                           std::vector<TpchQ18OutputRow> &output_rows) {
  if (threshold_raw >= TpchQ18State::kMinPersistQuantityRaw)
    return;
  const auto table_sums =
      threshold_raw < TpchQ18State::kMinPersistQuantityRaw
          ? ScanOrderQuantitySums(initial_scan)
          : std::unordered_map<int64_t, int64_t>{};
  const auto sum_of = [&](int64_t orderkey) -> int64_t {
    if (threshold_raw >= TpchQ18State::kMinPersistQuantityRaw)
      return state.OrderQuantitySum(orderkey);
    const auto it = table_sums.find(orderkey);
    return it == table_sums.end() ? 0 : it->second;
  };

  struct OrderMeta {
    int64_t custkey;
    int32_t orderdate_days;
    int64_t totalprice_raw;
  };
  std::unordered_map<int64_t, OrderMeta> orders;
  std::unordered_set<int64_t> needed_custkeys;

  initial_scan->ScanOrders([&](const TpchQ18OrderInput &input) {
    if (input.key < 0 || skip_orderkeys.find(input.key) != skip_orderkeys.end())
      return;
    const int64_t sum = sum_of(input.key);
    if (sum <= threshold_raw || sum > TpchQ18State::kMinPersistQuantityRaw)
      return;
    orders.emplace(input.key,
                   OrderMeta{input.custkey, input.orderdate_days,
                             input.totalprice_raw});
    needed_custkeys.insert(input.custkey);
  });
  if (orders.empty())
    return;

  std::unordered_map<int64_t, std::string> customer_names;
  for (const auto &[custkey, name] : state.customer_names) {
    if (needed_custkeys.find(custkey) != needed_custkeys.end())
      customer_names.emplace(custkey, name);
  }
  if (customer_names.size() < needed_custkeys.size()) {
    initial_scan->ScanCustomers([&](const TpchQ18CustomerInput &input) {
      if (needed_custkeys.find(input.key) == needed_custkeys.end())
        return;
      customer_names.emplace(input.key, input.name);
    });
  }

  for (const auto &[orderkey, meta] : orders) {
    const auto name_it = customer_names.find(meta.custkey);
    if (name_it == customer_names.end())
      continue;
    TpchQ18OutputRow row;
    row.name = name_it->second;
    row.custkey = meta.custkey;
    row.orderkey = orderkey;
    row.orderdate_days = meta.orderdate_days;
    row.totalprice_raw = meta.totalprice_raw;
    row.quantity_sum_raw = sum_of(orderkey);
    output_rows.push_back(std::move(row));
  }
}

} // namespace

void TpchQ18State::AddOrderQuantity(int64_t orderkey, int64_t delta) {
  if (orderkey < 0 || delta == 0)
    return;
  SetOrderQuantitySum(orderkey, OrderQuantitySum(orderkey) + delta);
}

void TpchQ18State::SetOrderQuantitySum(int64_t orderkey, int64_t sum) {
  if (orderkey < 0)
    return;
  if (sum <= 0) {
    quantity_overflow.erase(orderkey);
    return;
  }
  quantity_overflow[orderkey] = sum;
}

int64_t TpchQ18State::OrderQuantitySum(int64_t orderkey) const {
  const auto ov = quantity_overflow.find(orderkey);
  if (ov != quantity_overflow.end())
    return ov->second;
  const auto it = survivors.find(orderkey);
  return it == survivors.end() ? 0 : it->second.quantity_sum_raw;
}

void TpchQ18State::SetOrderMeta(int64_t orderkey, int64_t custkey,
                                  int32_t orderdate_days,
                                  int64_t totalprice_raw) {
  if (orderkey < 0 || custkey <= 0)
    return;
  survivors[orderkey] = TpchQ18SurvivorMeta{
      custkey, orderdate_days, totalprice_raw, OrderQuantitySum(orderkey)};
}

void TpchQ18State::RemoveSurvivor(int64_t orderkey) {
  survivors.erase(orderkey);
}

void TpchQ18State::SyncSurvivorQuantity(int64_t orderkey) {
  const auto it = survivors.find(orderkey);
  if (it != survivors.end())
    it->second.quantity_sum_raw = OrderQuantitySum(orderkey);
}

bool TpchQ18State::HasOrderMeta(int64_t orderkey) const {
  return survivors.find(orderkey) != survivors.end();
}

int64_t TpchQ18State::OrderCustkey(int64_t orderkey) const {
  const auto it = survivors.find(orderkey);
  return it == survivors.end() ? 0 : it->second.custkey;
}

int32_t TpchQ18State::OrderOrderdate(int64_t orderkey) const {
  const auto it = survivors.find(orderkey);
  return it == survivors.end() ? 0 : it->second.orderdate_days;
}

int64_t TpchQ18State::OrderTotalprice(int64_t orderkey) const {
  const auto it = survivors.find(orderkey);
  return it == survivors.end() ? 0 : it->second.totalprice_raw;
}

std::vector<uint8_t> EncodeTpchQ18OrderQuantitySums(const TpchQ18State &state) {
  std::vector<int64_t> survivor_keys;
  survivor_keys.reserve(state.survivors.size());
  for (const auto &[orderkey, _] : state.survivors)
    survivor_keys.push_back(orderkey);
  std::sort(survivor_keys.begin(), survivor_keys.end());

  std::vector<std::pair<int64_t, int64_t>> qty_entries;
  CollectSparseQuantities(state, qty_entries);

  std::vector<std::pair<int64_t, std::string>> names;
  names.reserve(state.customer_names.size());
  for (const auto &[custkey, name] : state.customer_names)
    names.emplace_back(custkey, name);
  std::sort(names.begin(), names.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  ByteWriter writer;
  core::WriteImageHeader(writer, kQuantitySumsMagic, kVersionV3,
                         static_cast<uint32_t>(survivor_keys.size()));
  for (const int64_t orderkey : survivor_keys) {
    const auto &meta = state.survivors.at(orderkey);
    writer.I64(orderkey);
    writer.I64(meta.custkey);
    writer.I32(meta.orderdate_days);
    writer.I64(meta.totalprice_raw);
  }
  writer.U32(static_cast<uint32_t>(qty_entries.size()));
  for (const auto &[orderkey, sum] : qty_entries) {
    writer.I64(orderkey);
    writer.I64(sum);
  }
  writer.U32(static_cast<uint32_t>(names.size()));
  for (const auto &[custkey, name] : names) {
    writer.I64(custkey);
    writer.String(name);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ18OrderQuantitySums(const std::vector<uint8_t> &bytes,
                                    TpchQ18State &state) {
  auto decode_v3 = [&](ByteReader &reader, uint32_t survivor_count) -> bool {
    state.quantity_overflow.clear();
    state.survivors.clear();
    state.active_orderkeys.clear();
    state.customer_names.clear();

    for (uint32_t i = 0; i < survivor_count; ++i) {
      int64_t orderkey = 0, custkey = 0, totalprice = 0;
      int32_t orderdate = 0;
      if (!reader.I64(orderkey) || !reader.I64(custkey) || !reader.I32(orderdate) ||
          !reader.I64(totalprice))
        return false;
      if (orderkey < 0 || custkey <= 0)
        return false;
      state.survivors[orderkey] =
          TpchQ18SurvivorMeta{custkey, orderdate, totalprice, 0};
    }

    uint32_t qty_count = 0;
    if (!reader.U32(qty_count))
      return false;
    for (uint32_t i = 0; i < qty_count; ++i) {
      int64_t key = 0, sum = 0;
      if (!reader.I64(key) || !reader.I64(sum))
        return false;
      if (key < 0)
        return false;
      state.SetOrderQuantitySum(key, sum);
      state.SyncSurvivorQuantity(key);
    }

    uint32_t name_count = 0;
    if (!reader.U32(name_count))
      return false;
    for (uint32_t i = 0; i < name_count; ++i) {
      int64_t custkey = 0;
      std::string name;
      if (!reader.I64(custkey) || !reader.String(name))
        return false;
      if (custkey <= 0)
        return false;
      state.customer_names.emplace(custkey, std::move(name));
    }

    RebuildActiveOrderkeys(state);
    return reader.FullyConsumed();
  };

  auto decode_v2 = [&](ByteReader &reader, uint32_t count) -> bool {
    state.quantity_overflow.clear();
    state.survivors.clear();
    state.active_orderkeys.clear();
    state.customer_names.clear();

    for (uint32_t i = 0; i < count; ++i) {
      int64_t key = 0, sum = 0, custkey = 0, totalprice = 0;
      int32_t orderdate = 0;
      if (!reader.I64(key) || !reader.I64(sum) || !reader.I64(custkey) ||
          !reader.I32(orderdate) || !reader.I64(totalprice))
        return false;
      if (key < 0 || custkey <= 0)
        return false;
      state.SetOrderQuantitySum(key, sum);
      if (sum > TpchQ18State::kMinPersistQuantityRaw)
        state.SetOrderMeta(key, custkey, orderdate, totalprice);
    }
    RebuildActiveOrderkeys(state);
    return reader.FullyConsumed();
  };

  auto decode_v1 = [&](ByteReader &reader, uint32_t count) -> bool {
    state.quantity_overflow.clear();
    state.survivors.clear();
    state.active_orderkeys.clear();
    state.customer_names.clear();

    for (uint32_t i = 0; i < count; ++i) {
      int64_t key = 0, sum = 0;
      if (!reader.I64(key) || !reader.I64(sum))
        return false;
      if (key < 0)
        return false;
      state.SetOrderQuantitySum(key, sum);
    }
    return reader.FullyConsumed();
  };

  ByteReader reader_v3{bytes};
  uint32_t count = 0;
  if (core::ReadImageHeader(reader_v3, kQuantitySumsMagic, kVersionV3, count))
    return decode_v3(reader_v3, count);

  ByteReader reader_v2{bytes};
  if (core::ReadImageHeader(reader_v2, kQuantitySumsMagic, kVersionV2, count))
    return decode_v2(reader_v2, count);

  ByteReader reader_v1{bytes};
  if (!core::ReadImageHeader(reader_v1, kQuantitySumsMagic, kVersionV1, count))
    return false;
  return decode_v1(reader_v1, count);
}

bool TpchQ18TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ18TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ18TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ18LineitemDelta> &TpchQ18TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ18TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ18TxnBuffer::AddDelta(TpchQ18LineitemDelta delta) {
  pending = true;
  if (delta.quantity_raw != 0)
    deltas.push_back(delta);
}

core::AcceleratorId TpchQ18Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ18Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1}, {0, 4}},
          {config.orders_table, {0, 1, 2, 3}, {0, 1, 4, 3}},
          {config.customer_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ18Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ18TxnBuffer>();
}

void TpchQ18Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ18State>(state);
  builder.Write(core::StateImage::Make(
      TpchQ18Layout::kAccelerator, TpchQ18Layout::kOrderQuantitySums, 0,
      MakeStateImage(TpchQ18ImageKind::OrderQuantitySums, typed_state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ18Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.customer_table)
    return All();
  return {};
}

void TpchQ18Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ18TxnBuffer &>(base_buffer);
  // Customer mutations are rare (RF does not touch customer). Orders Insert /
  // Delete are NoOp here: survivor meta is filled via LookupOrder when a
  // lineitem delta first crosses the persist threshold.
  if (mutation.table == config.customer_table) {
    buffer.MarkRebuild();
    return;
  }
  if (mutation.table == config.orders_table)
    return;
  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0, quantity_raw = 0;
    if (!ReadLineitem(row, orderkey, quantity_raw)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddDelta({orderkey, quantity_raw, sign});
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
TpchQ18Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ18TxnBuffer &>(base_buffer);
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

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
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

  core::Version existing_version = 0;
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ18Layout::kOrderQuantitySums});
  bool found = false;
  std::shared_ptr<TpchQ18State> new_state;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ18Layout::kOrderQuantitySums) {
      const auto *typed = AsQ18Image(image, TpchQ18ImageKind::OrderQuantitySums);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      new_state = std::make_shared<TpchQ18State>(*typed->state);
      existing_version = image.version;
      found = true;
      break;
    }
  }
  if (!found) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  std::unordered_map<int64_t, int64_t> net_by_order;
  for (const auto &delta : buffer.Deltas()) {
    if (delta.orderkey < 0)
      continue;
    net_by_order[delta.orderkey] +=
        static_cast<int64_t>(delta.sign) * delta.quantity_raw;
  }

  for (const auto &[orderkey, net] : net_by_order) {
    if (net == 0)
      continue;
    int64_t after = 0;
    if (initial_scan && initial_scan->LookupOrderQuantitySum(orderkey, after)) {
      // Scan includes this transaction — do not add `net` again.
    } else if (new_state->HasOrderMeta(orderkey)) {
      after = new_state->OrderQuantitySum(orderkey) + net;
    } else {
      // The state holds survivors only: this order's previous sum is
      // unknown here, so rebuild rather than guess it from 0.
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    const int64_t before = after - net;
    const bool was_survivor = before > TpchQ18State::kMinPersistQuantityRaw;
    const bool will_be_survivor = after > TpchQ18State::kMinPersistQuantityRaw;

    new_state->SetOrderQuantitySum(orderkey, after);

    if (was_survivor && !will_be_survivor) {
      new_state->RemoveSurvivor(orderkey);
      new_state->quantity_overflow.erase(orderkey);
      UpdateActiveOrderkey(*new_state, orderkey);
      continue;
    }

    if (!will_be_survivor) {
      new_state->quantity_overflow.erase(orderkey);
      continue;
    }

    if (!new_state->HasOrderMeta(orderkey)) {
      TpchQ18OrderInput order;
      if (!initial_scan || !initial_scan->LookupOrder(orderkey, order) ||
          order.custkey <= 0) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      new_state->SetOrderMeta(order.key, order.custkey, order.orderdate_days,
                              order.totalprice_raw);
      if (new_state->customer_names.find(order.custkey) ==
          new_state->customer_names.end()) {
        initial_scan->ScanCustomers([&](const TpchQ18CustomerInput &input) {
          if (input.key == order.custkey)
            new_state->customer_names.emplace(input.key, input.name);
        });
      }
      UpdateActiveOrderkey(*new_state, orderkey);
    } else {
      new_state->SyncSurvivorQuantity(orderkey);
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ18State>(std::move(new_state));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ18Layout::kAccelerator, TpchQ18Layout::kOrderQuantitySums,
      existing_version,
      MakeStateImage(TpchQ18ImageKind::OrderQuantitySums, typed_state)));
  return outcome;
}

bool TpchQ18Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ18Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (arguments.values.empty() || arguments.values[0].is_null) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  int64_t threshold_raw = 0;
  if (std::holds_alternative<int64_t>(arguments.values[0].payload)) {
    threshold_raw = std::get<int64_t>(arguments.values[0].payload) * 100;
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

  std::shared_ptr<const TpchQ18State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, threshold_raw, services, sink);
}

void TpchQ18Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  int64_t threshold_raw = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      threshold_raw = std::get<int64_t>(arguments.values[0].payload) * 100;
  }
  EmitResult(BuildState(services), threshold_raw, services, sink);
}

TpchQ18State
TpchQ18Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ18State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");

  const bool used_survivors = initial_scan->ScanSurvivorOrders(
      TpchQ18State::kMinPersistQuantityRaw,
      [&](const TpchQ18SurvivorAggregate &agg) {
        if (agg.orderkey < 0 || agg.custkey <= 0 ||
            agg.quantity_sum_raw <= TpchQ18State::kMinPersistQuantityRaw)
          return;
        state.SetOrderQuantitySum(agg.orderkey, agg.quantity_sum_raw);
        state.SetOrderMeta(agg.orderkey, agg.custkey, agg.orderdate_days,
                           agg.totalprice_raw);
      });
  if (!used_survivors) {
    const bool used_sql = initial_scan->ScanOrderQuantityAggregates(
        [&](const TpchQ18OrderQuantityAggregate &agg) {
          if (agg.orderkey < 0 ||
              agg.quantity_sum_raw <= TpchQ18State::kMinPersistQuantityRaw)
            return;
          state.SetOrderQuantitySum(agg.orderkey, agg.quantity_sum_raw);
        });
    if (!used_sql) {
      initial_scan->ScanLineitems([&](const TpchQ18LineitemInput &input) {
        if (input.orderkey < 0)
          return;
        state.AddOrderQuantity(input.orderkey, input.quantity_raw);
      });
    }

    initial_scan->ScanOrders([&](const TpchQ18OrderInput &input) {
      if (input.key < 0)
        return;
      const int64_t sum = state.OrderQuantitySum(input.key);
      if (sum <= TpchQ18State::kMinPersistQuantityRaw)
        return;
      state.SetOrderMeta(input.key, input.custkey, input.orderdate_days,
                         input.totalprice_raw);
    });
  }

  // Published state is survivor-only. Drop every other quantity row even
  // when the SQL aggregate path ran — otherwise SF20 keeps O(orders).
  for (auto it = state.quantity_overflow.begin();
       it != state.quantity_overflow.end();) {
    if (state.survivors.find(it->first) == state.survivors.end())
      it = state.quantity_overflow.erase(it);
    else
      ++it;
  }

  std::unordered_set<int64_t> needed_custkeys;
  needed_custkeys.reserve(state.survivors.size());
  for (const auto &[_, meta] : state.survivors)
    needed_custkeys.insert(meta.custkey);

  initial_scan->ScanCustomers([&](const TpchQ18CustomerInput &input) {
    if (input.key <= 0)
      return;
    if (needed_custkeys.find(input.key) == needed_custkeys.end())
      return;
    state.customer_names.emplace(input.key, input.name);
  });

  RebuildActiveOrderkeys(state);
  return state;
}

bool TpchQ18Module::LoadState(core::AuxEngineServices &services,
                              std::shared_ptr<const TpchQ18State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ18Layout::kOrderQuantitySums});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ18Layout::kOrderQuantitySums) {
      const auto *typed = AsQ18Image(image, TpchQ18ImageKind::OrderQuantitySums);
      if (!typed)
        return false;
      state = typed->state;
      return true;
    }
  }
  return false;
}

void TpchQ18Module::EmitResult(const TpchQ18State &state,
                               int64_t threshold_raw,
                               core::AuxEngineServices &services,
                               core::AuxRowSink &sink) const {
  (void)services;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");

  // The state holds every order above kMinPersistQuantityRaw: with no
  // survivors there is no qualifying order (no SQL at query time).
  if (state.survivors.empty() &&
      threshold_raw >= TpchQ18State::kMinPersistQuantityRaw) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  if (state.survivors.empty()) {
    EmitResultScanOrders(state, threshold_raw, initial_scan, sink);
    return;
  }

  std::vector<TpchQ18OutputRow> output_rows;
  output_rows.reserve(state.survivors.size());

  std::unordered_set<int64_t> emitted_orderkeys;
  emitted_orderkeys.reserve(state.survivors.size());

  auto consider = [&](int64_t orderkey, const TpchQ18SurvivorMeta &meta) {
    const int64_t sum = state.OrderQuantitySum(orderkey);
    if (sum <= threshold_raw)
      return;
    const auto name_it = state.customer_names.find(meta.custkey);
    if (name_it == state.customer_names.end())
      return;
    emitted_orderkeys.insert(orderkey);
    TpchQ18OutputRow row;
    row.name = name_it->second;
    row.custkey = meta.custkey;
    row.orderkey = orderkey;
    row.orderdate_days = meta.orderdate_days;
    row.totalprice_raw = meta.totalprice_raw;
    row.quantity_sum_raw = sum;
    output_rows.push_back(std::move(row));
  };

  if (!state.active_orderkeys.empty()) {
    for (const int64_t orderkey : state.active_orderkeys) {
      const auto it = state.survivors.find(orderkey);
      if (it == state.survivors.end())
        continue;
      consider(orderkey, it->second);
    }
  } else {
    for (const auto &[orderkey, meta] : state.survivors)
      consider(orderkey, meta);
  }

  if (output_rows.empty() && state.customer_names.empty()) {
    std::unordered_set<int64_t> needed_custkeys;
    for (const auto &[orderkey, meta] : state.survivors) {
      const int64_t sum = state.OrderQuantitySum(orderkey);
      if (sum <= threshold_raw)
        continue;
      if (meta.custkey > 0)
        needed_custkeys.insert(meta.custkey);
    }
    if (!needed_custkeys.empty()) {
      std::unordered_map<int64_t, std::string> customer_names;
      initial_scan->ScanCustomers([&](const TpchQ18CustomerInput &input) {
        if (needed_custkeys.find(input.key) == needed_custkeys.end())
          return;
        customer_names.emplace(input.key, input.name);
      });
      for (const auto &[orderkey, meta] : state.survivors) {
        const int64_t sum = state.OrderQuantitySum(orderkey);
        if (sum <= threshold_raw)
          continue;
        const auto name_it = customer_names.find(meta.custkey);
        if (name_it == customer_names.end())
          continue;
        emitted_orderkeys.insert(orderkey);
        TpchQ18OutputRow row;
        row.name = name_it->second;
        row.custkey = meta.custkey;
        row.orderkey = orderkey;
        row.orderdate_days = meta.orderdate_days;
        row.totalprice_raw = meta.totalprice_raw;
        row.quantity_sum_raw = sum;
        output_rows.push_back(std::move(row));
      }
    }
  }

  AppendNonSurvivorRows(state, threshold_raw, initial_scan, emitted_orderkeys,
                        output_rows);

  if (output_rows.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(6);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.custkey));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.orderkey));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Date, core::AuxDate{row.orderdate_days}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           core::ScaledDecimal{row.totalprice_raw, 2}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           core::ScaledDecimal{row.quantity_sum_raw, 2}));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ18Module::CalibrationWrites() const {
  return TpchCalibrationWrites(18);
}

} // namespace accelerator_codegen::aux_state::query
