#include "tpch/q18/aux/module.hpp"
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

void EnsureChunkSlots(TpchQ18State &state, int64_t orderkey) {
  if (orderkey < 0)
    return;
  const auto key = static_cast<std::size_t>(orderkey);
  const auto chunk_idx = key >> TpchQ18State::kChunkShift;
  const auto slot = key & TpchQ18State::kChunkMask;
  const auto need_chunks = chunk_idx + 1;
  if (state.quantity_chunks.size() < need_chunks) {
    state.quantity_chunks.resize(need_chunks);
    state.custkey_chunks.resize(need_chunks);
    state.orderdate_chunks.resize(need_chunks);
    state.totalprice_chunks.resize(need_chunks);
  }
  auto grow = [&](auto &chunks, auto fill) {
    auto &chunk = chunks[chunk_idx];
    if (slot >= chunk.size())
      chunk.resize(slot + 1, fill);
  };
  grow(state.quantity_chunks, int64_t{0});
  grow(state.custkey_chunks, int64_t{0});
  grow(state.orderdate_chunks, int32_t{0});
  grow(state.totalprice_chunks, int64_t{0});
}

void RebuildActiveOrderkeys(TpchQ18State &state) {
  state.active_orderkeys.clear();
  for (std::size_t chunk_idx = 0; chunk_idx < state.quantity_chunks.size();
       ++chunk_idx) {
    const auto &qchunk = state.quantity_chunks[chunk_idx];
    const int64_t base =
        static_cast<int64_t>(chunk_idx << TpchQ18State::kChunkShift);
    for (std::size_t slot = 0; slot < qchunk.size(); ++slot) {
      if (qchunk[slot] == 0)
        continue;
      const int64_t orderkey = base + static_cast<int64_t>(slot);
      if (!state.HasOrderMeta(orderkey))
        continue;
      state.active_orderkeys.push_back(orderkey);
    }
  }
}

bool StateHasJoinMetadata(const TpchQ18State &state) {
  for (const auto &chunk : state.custkey_chunks) {
    for (const int64_t custkey : chunk) {
      if (custkey != 0)
        return true;
    }
  }
  return false;
}

void EmitResultScanOrders(const TpchQ18State &state, int64_t threshold_raw,
                          TpchQ18InitialScan *initial_scan,
                          core::AuxRowSink &sink) {
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
    const int64_t sum = state.OrderQuantitySum(input.key);
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
    row.quantity_sum_raw = state.OrderQuantitySum(orderkey);
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

} // namespace

void TpchQ18State::AddOrderQuantity(int64_t orderkey, int64_t delta) {
  if (orderkey < 0 || delta == 0)
    return;
  EnsureChunkSlots(*this, orderkey);
  const auto key = static_cast<std::size_t>(orderkey);
  quantity_chunks[key >> kChunkShift][key & kChunkMask] += delta;
}

void TpchQ18State::SetOrderQuantitySum(int64_t orderkey, int64_t sum) {
  if (orderkey < 0)
    return;
  EnsureChunkSlots(*this, orderkey);
  const auto key = static_cast<std::size_t>(orderkey);
  quantity_chunks[key >> kChunkShift][key & kChunkMask] = sum;
}

int64_t TpchQ18State::OrderQuantitySum(int64_t orderkey) const {
  if (orderkey < 0)
    return 0;
  const auto key = static_cast<std::size_t>(orderkey);
  const auto chunk_idx = key >> kChunkShift;
  if (chunk_idx >= quantity_chunks.size())
    return 0;
  const auto &chunk = quantity_chunks[chunk_idx];
  const auto slot = key & kChunkMask;
  if (slot >= chunk.size())
    return 0;
  return chunk[slot];
}

void TpchQ18State::SetOrderMeta(int64_t orderkey, int64_t custkey,
                                int32_t orderdate_days, int64_t totalprice_raw) {
  if (orderkey < 0 || custkey <= 0)
    return;
  EnsureChunkSlots(*this, orderkey);
  const auto key = static_cast<std::size_t>(orderkey);
  custkey_chunks[key >> kChunkShift][key & kChunkMask] = custkey;
  orderdate_chunks[key >> kChunkShift][key & kChunkMask] = orderdate_days;
  totalprice_chunks[key >> kChunkShift][key & kChunkMask] = totalprice_raw;
}

bool TpchQ18State::HasOrderMeta(int64_t orderkey) const {
  if (orderkey < 0)
    return false;
  const auto key = static_cast<std::size_t>(orderkey);
  const auto chunk_idx = key >> kChunkShift;
  if (chunk_idx >= custkey_chunks.size())
    return false;
  const auto &chunk = custkey_chunks[chunk_idx];
  const auto slot = key & kChunkMask;
  if (slot >= chunk.size())
    return false;
  return chunk[slot] != 0;
}

int64_t TpchQ18State::OrderCustkey(int64_t orderkey) const {
  if (!HasOrderMeta(orderkey))
    return 0;
  const auto key = static_cast<std::size_t>(orderkey);
  return custkey_chunks[key >> kChunkShift][key & kChunkMask];
}

int32_t TpchQ18State::OrderOrderdate(int64_t orderkey) const {
  if (!HasOrderMeta(orderkey))
    return 0;
  const auto key = static_cast<std::size_t>(orderkey);
  return orderdate_chunks[key >> kChunkShift][key & kChunkMask];
}

int64_t TpchQ18State::OrderTotalprice(int64_t orderkey) const {
  if (!HasOrderMeta(orderkey))
    return 0;
  const auto key = static_cast<std::size_t>(orderkey);
  return totalprice_chunks[key >> kChunkShift][key & kChunkMask];
}

std::vector<uint8_t> EncodeTpchQ18OrderQuantitySums(const TpchQ18State &state) {
  ByteWriter writer;
  std::vector<int64_t> keys;
  keys.reserve(state.active_orderkeys.size());
  for (const int64_t orderkey : state.active_orderkeys) {
    if (state.OrderQuantitySum(orderkey) != 0 && state.HasOrderMeta(orderkey))
      keys.push_back(orderkey);
  }
  if (keys.empty()) {
    for (std::size_t chunk_idx = 0; chunk_idx < state.quantity_chunks.size();
         ++chunk_idx) {
      const auto &qchunk = state.quantity_chunks[chunk_idx];
      const int64_t base =
          static_cast<int64_t>(chunk_idx << TpchQ18State::kChunkShift);
      for (std::size_t slot = 0; slot < qchunk.size(); ++slot) {
        if (qchunk[slot] == 0)
          continue;
        const int64_t orderkey = base + static_cast<int64_t>(slot);
        if (!state.HasOrderMeta(orderkey))
          continue;
        keys.push_back(orderkey);
      }
    }
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

  core::WriteImageHeader(writer, kQuantitySumsMagic, kVersionV2,
                         static_cast<uint32_t>(keys.size()));
  for (const int64_t orderkey : keys) {
    writer.I64(orderkey);
    writer.I64(state.OrderQuantitySum(orderkey));
    writer.I64(state.OrderCustkey(orderkey));
    writer.I32(state.OrderOrderdate(orderkey));
    writer.I64(state.OrderTotalprice(orderkey));
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ18OrderQuantitySums(const std::vector<uint8_t> &bytes,
                                    TpchQ18State &state) {
  auto decode_body = [&](ByteReader &reader, uint32_t version, uint32_t count) -> bool {
    state.quantity_chunks.clear();
    state.custkey_chunks.clear();
    state.orderdate_chunks.clear();
    state.totalprice_chunks.clear();
    state.active_orderkeys.clear();

    if (version == kVersionV1) {
      for (uint32_t i = 0; i < count; ++i) {
        int64_t key = 0, sum = 0;
        if (!reader.I64(key) || !reader.I64(sum))
          return false;
        if (key < 0)
          return false;
        state.SetOrderQuantitySum(key, sum);
      }
      RebuildActiveOrderkeys(state);
      return reader.offset == bytes.size();
    }

    for (uint32_t i = 0; i < count; ++i) {
      int64_t key = 0, sum = 0, custkey = 0, totalprice = 0;
      int32_t orderdate = 0;
      if (!reader.I64(key) || !reader.I64(sum) || !reader.I64(custkey) ||
          !reader.I32(orderdate) || !reader.I64(totalprice))
        return false;
      if (key < 0 || custkey <= 0)
        return false;
      state.SetOrderQuantitySum(key, sum);
      state.SetOrderMeta(key, custkey, orderdate, totalprice);
    }
    RebuildActiveOrderkeys(state);
    return reader.offset == bytes.size();
  };

  ByteReader reader_v2{bytes};
  uint32_t count = 0;
  if (core::ReadImageHeader(reader_v2, kQuantitySumsMagic, kVersionV2, count))
    return decode_body(reader_v2, kVersionV2, count);

  ByteReader reader_v1{bytes};
  if (!core::ReadImageHeader(reader_v1, kQuantitySumsMagic, kVersionV1, count))
    return false;
  return decode_body(reader_v1, kVersionV1, count);
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
  return {{config.lineitem_table, {0, 1}},
          {config.orders_table, {0, 1, 2, 3}}};
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
  if (mutation.table == config.lineitem_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  if (mutation.table == config.orders_table)
    return All();
  return {};
}

void TpchQ18Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ18TxnBuffer &>(base_buffer);
  if (mutation.table == config.orders_table) {
    buffer.MarkRebuild();
    return;
  }
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
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, true, true,
       false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
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

  for (const auto &delta : buffer.Deltas()) {
    if (delta.orderkey < 0)
      continue;
    const int64_t signed_qty =
        static_cast<int64_t>(delta.sign) * delta.quantity_raw;
    const int64_t before = new_state->OrderQuantitySum(delta.orderkey);
    if (before == 0 && signed_qty > 0 && !new_state->HasOrderMeta(delta.orderkey)) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    new_state->AddOrderQuantity(delta.orderkey, signed_qty);
  }
  RebuildActiveOrderkeys(*new_state);

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

  initial_scan->ScanLineitems([&](const TpchQ18LineitemInput &input) {
    if (input.orderkey < 0)
      return;
    state.AddOrderQuantity(input.orderkey, input.quantity_raw);
  });
  initial_scan->ScanOrders([&](const TpchQ18OrderInput &input) {
    if (input.key < 0)
      return;
    state.SetOrderMeta(input.key, input.custkey, input.orderdate_days,
                       input.totalprice_raw);
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
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");

  if (!StateHasJoinMetadata(state)) {
    EmitResultScanOrders(state, threshold_raw, initial_scan, sink);
    return;
  }

  std::unordered_set<int64_t> needed_custkeys;
  std::vector<TpchQ18OutputRow> output_rows;
  output_rows.reserve(state.active_orderkeys.size() / 4 + 8);

  for (const int64_t orderkey : state.active_orderkeys) {
    const int64_t sum = state.OrderQuantitySum(orderkey);
    if (sum <= threshold_raw)
      continue;
    const int64_t custkey = state.OrderCustkey(orderkey);
    if (custkey <= 0)
      continue;
    TpchQ18OutputRow row;
    row.custkey = custkey;
    row.orderkey = orderkey;
    row.orderdate_days = state.OrderOrderdate(orderkey);
    row.totalprice_raw = state.OrderTotalprice(orderkey);
    row.quantity_sum_raw = sum;
    needed_custkeys.insert(custkey);
    output_rows.push_back(std::move(row));
  }

  if (output_rows.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  std::unordered_map<int64_t, std::string> customer_names;
  customer_names.reserve(needed_custkeys.size());
  initial_scan->ScanCustomers([&](const TpchQ18CustomerInput &input) {
    if (needed_custkeys.find(input.key) == needed_custkeys.end())
      return;
    customer_names.emplace(input.key, input.name);
  });

  std::vector<TpchQ18OutputRow> filtered;
  filtered.reserve(output_rows.size());
  for (auto &row : output_rows) {
    const auto name_it = customer_names.find(row.custkey);
    if (name_it == customer_names.end())
      continue;
    row.name = name_it->second;
    filtered.push_back(std::move(row));
  }
  output_rows = std::move(filtered);

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

} // namespace accelerator_codegen::aux_state::query
