#include "tpch/q18/duckdb/aux/module.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/q18/calibration_writes.hpp"

#include <algorithm>
#include <array>
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
// Deltas per slice (~200 us): netting one lineitem delta per order is ~50 ns.
constexpr std::size_t kDeltaGrain = 4096;

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &quantity_raw) {
  if (!core::IntCell(row, 0, orderkey))
    return false;
  int64_t raw = 0;
  int32_t scale = 0;
  if (!core::DecimalCell(row, 1, raw, scale))
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
  state.quantity_overflow.ForEach([&](int64_t orderkey, int64_t sum) {
    if (sum != 0)
      entries.emplace_back(orderkey, sum);
  });
  std::sort(entries.begin(), entries.end());
}

// Quantity sum per order key in hash shards: partial states of a parallel
// build fold shard by shard in parallel. A run of rows of one order (lineitem
// is clustered by order) is summed before it touches a shard.
struct OrderSums {
  static constexpr std::size_t kShards = 64;
  std::array<std::unordered_map<int64_t, int64_t>, kShards> shards;
  int64_t run_key = -1;
  int64_t run_sum = 0;

  static std::size_t ShardOf(int64_t orderkey) {
    return (static_cast<uint64_t>(orderkey) * 0x9E3779B97F4A7C15ULL) >> 58;
  }
  void Add(int64_t orderkey, int64_t quantity_raw) {
    if (orderkey != run_key) {
      Flush();
      run_key = orderkey;
    }
    run_sum += quantity_raw;
  }
  // Moves the open run into its shard; call before reading or folding.
  void Flush() {
    if (run_key >= 0)
      shards[ShardOf(run_key)][run_key] += run_sum;
    run_key = -1;
    run_sum = 0;
  }
  void FoldShard(std::size_t shard, const OrderSums &from) {
    for (const auto &[orderkey, sum] : from.shards[shard])
      shards[shard][orderkey] += sum;
  }
};

// Per-order quantity sums from the tables. The state holds only orders above
// kMinPersistQuantityRaw, so a lower threshold needs these.
std::unordered_map<int64_t, int64_t>
ScanOrderQuantitySums(TpchQ18InitialScan *initial_scan) {
  std::unordered_map<int64_t, int64_t> sums;
  if (!initial_scan->ScanOrderQuantityAggregates(
          [&](const TpchQ18OrderQuantityAggregate &agg) {
            sums[agg.orderkey] += agg.quantity_sum_raw;
          })) {
    initial_scan->ScanLineitemsSerial([&](const TpchQ18LineitemInput &input) {
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

  initial_scan->ScanOrdersSerial([&](const TpchQ18OrderInput &input) {
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
  initial_scan->ScanCustomersSerial([&](const TpchQ18CustomerInput &input) {
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

  initial_scan->ScanOrdersSerial([&](const TpchQ18OrderInput &input) {
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
  state.customer_names.ForEach([&](int64_t custkey, const std::string &name) {
    if (needed_custkeys.find(custkey) != needed_custkeys.end())
      customer_names.emplace(custkey, name);
  });
  if (customer_names.size() < needed_custkeys.size()) {
    initial_scan->ScanCustomersSerial([&](const TpchQ18CustomerInput &input) {
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
    quantity_overflow.Erase(orderkey);
    return;
  }
  quantity_overflow.Upsert(orderkey, sum);
}

int64_t TpchQ18State::OrderQuantitySum(int64_t orderkey) const {
  if (const int64_t *ov = quantity_overflow.Find(orderkey))
    return *ov;
  const auto *meta = survivors.Find(orderkey);
  return meta ? meta->quantity_sum_raw : 0;
}

void TpchQ18State::SetOrderMeta(int64_t orderkey, int64_t custkey,
                                  int32_t orderdate_days,
                                  int64_t totalprice_raw) {
  if (orderkey < 0 || custkey <= 0)
    return;
  survivors.Upsert(orderkey,
                   TpchQ18SurvivorMeta{custkey, orderdate_days, totalprice_raw,
                                       OrderQuantitySum(orderkey)});
}

void TpchQ18State::RemoveSurvivor(int64_t orderkey) {
  survivors.Erase(orderkey);
}

void TpchQ18State::SyncSurvivorQuantity(int64_t orderkey) {
  if (survivors.Contains(orderkey))
    survivors[orderkey].quantity_sum_raw = OrderQuantitySum(orderkey);
}

bool TpchQ18State::HasOrderMeta(int64_t orderkey) const {
  return survivors.Contains(orderkey);
}

int64_t TpchQ18State::OrderCustkey(int64_t orderkey) const {
  const auto *meta = survivors.Find(orderkey);
  return meta ? meta->custkey : 0;
}

int32_t TpchQ18State::OrderOrderdate(int64_t orderkey) const {
  const auto *meta = survivors.Find(orderkey);
  return meta ? meta->orderdate_days : 0;
}

int64_t TpchQ18State::OrderTotalprice(int64_t orderkey) const {
  const auto *meta = survivors.Find(orderkey);
  return meta ? meta->totalprice_raw : 0;
}

std::vector<uint8_t> EncodeTpchQ18OrderQuantitySums(const TpchQ18State &state) {
  std::vector<int64_t> survivor_keys;
  survivor_keys.reserve(state.survivors.size());
  state.survivors.ForEach([&](int64_t orderkey, const TpchQ18SurvivorMeta &) {
    survivor_keys.push_back(orderkey);
  });
  std::sort(survivor_keys.begin(), survivor_keys.end());

  std::vector<std::pair<int64_t, int64_t>> qty_entries;
  CollectSparseQuantities(state, qty_entries);

  std::vector<std::pair<int64_t, std::string>> names;
  names.reserve(state.customer_names.size());
  state.customer_names.ForEach([&](int64_t custkey, const std::string &name) {
    names.emplace_back(custkey, name);
  });
  std::sort(names.begin(), names.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  ByteWriter writer;
  core::WriteImageHeader(writer, kQuantitySumsMagic, kVersionV3,
                         static_cast<uint64_t>(survivor_keys.size()));
  for (const int64_t orderkey : survivor_keys) {
    const auto &meta = *state.survivors.Find(orderkey);
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
    state = TpchQ18State{};

    for (uint32_t i = 0; i < survivor_count; ++i) {
      int64_t orderkey = 0, custkey = 0, totalprice = 0;
      int32_t orderdate = 0;
      if (!reader.I64(orderkey) || !reader.I64(custkey) || !reader.I32(orderdate) ||
          !reader.I64(totalprice))
        return false;
      if (orderkey < 0 || custkey <= 0)
        return false;
      state.survivors.Upsert(
          orderkey, TpchQ18SurvivorMeta{custkey, orderdate, totalprice, 0});
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
      if (!state.customer_names.Contains(custkey))
        state.customer_names.Upsert(custkey, std::move(name));
    }

    return reader.FullyConsumed();
  };

  auto decode_v2 = [&](ByteReader &reader, uint64_t count) -> bool {
    state = TpchQ18State{};

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
    return reader.FullyConsumed();
  };

  auto decode_v1 = [&](ByteReader &reader, uint64_t count) -> bool {
    state = TpchQ18State{};

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
  uint64_t count = 0;
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
  inserted_orders.clear();
  deleted_orders.clear();
  customer_names.clear();
}
void TpchQ18TxnBuffer::AddCustomerName(int64_t custkey, std::string name) {
  pending = true;
  customer_names.emplace_back(custkey, std::move(name));
}
const std::vector<std::pair<int64_t, std::string>> &
TpchQ18TxnBuffer::CustomerNames() const {
  return customer_names;
}
void TpchQ18TxnBuffer::NoteOrderInserted(int64_t orderkey) {
  pending = true;
  inserted_orders.insert(orderkey);
}
void TpchQ18TxnBuffer::NoteOrderDeleted(int64_t orderkey) {
  pending = true;
  deleted_orders.insert(orderkey);
}
const std::unordered_set<int64_t> &TpchQ18TxnBuffer::InsertedOrders() const {
  return inserted_orders;
}
const std::unordered_set<int64_t> &TpchQ18TxnBuffer::DeletedOrders() const {
  return deleted_orders;
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
    if (mutation.kind != core::MutationKind::Update ||
        mutation.old_rows.size() != mutation.new_rows.size()) {
      buffer.MarkRebuild();
      return;
    }
    // Q18 reads c_custkey and c_name only; capture also delivers updates that
    // changed no captured column.
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      int64_t ok = 0, nk = 0;
      std::string on, nn;
      if (!core::IntCell(mutation.old_rows[i], 0, ok) ||
          !core::StringCell(mutation.old_rows[i], 1, on) ||
          !core::IntCell(mutation.new_rows[i], 0, nk) ||
          !core::StringCell(mutation.new_rows[i], 1, nn) || ok != nk) {
        buffer.MarkRebuild();
        return;
      }
      if (on != nn)
        buffer.AddCustomerName(nk, std::move(nn));
    }
    return;
  }
  if (mutation.table == config.orders_table) {
    // Remember which order keys are created / removed in this transaction so
    // maintenance can derive "previous sum = 0" without a SQL point query.
    int64_t key = 0;
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        if (core::IntCell(row, 0, key))
          buffer.NoteOrderInserted(key);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        if (core::IntCell(row, 0, key))
          buffer.NoteOrderDeleted(key);
      break;
    case core::MutationKind::Update:
      // The survivors' meta (customer, date, price) copies these columns.
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        const auto &before = mutation.old_rows[i].values;
        const auto &after = mutation.new_rows[i].values;
        const bool same =
            before.size() == after.size() &&
            std::equal(before.begin(), before.end(), after.begin(),
                       [](const core::AuxValue &x, const core::AuxValue &y) {
                         return x.is_null == y.is_null && (x.is_null || x.payload == y.payload);
                       });
        if (!same) {
          buffer.MarkRebuild();
          return;
        }
      }
      break;
    }
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
    core::FinishRebuildAll(outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                                        services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.Deltas().empty() && buffer.CustomerNames().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
      buffer.Deltas().size() + buffer.CustomerNames().size(),
      services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  core::Version existing_version = 0;
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ18Layout::kOrderQuantitySums});
  bool found = false;
  std::shared_ptr<const TpchQ18State> base_state;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ18Layout::kOrderQuantitySums) {
      const auto *typed = AsQ18Image(image, TpchQ18ImageKind::OrderQuantitySums);
      if (!typed) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      base_state = typed->state;
      existing_version = image.version;
      found = true;
      break;
    }
  }
  if (!found) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  std::unordered_map<int64_t, int64_t> net_by_order;
  const auto &deltas = buffer.Deltas();
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, net_by_order,
      [&](std::unordered_map<int64_t, int64_t> &net, std::size_t first,
          std::size_t last) {
        for (std::size_t i = first; i < last; ++i) {
          if (deltas[i].orderkey < 0)
            continue;
          net[deltas[i].orderkey] +=
              static_cast<int64_t>(deltas[i].sign) * deltas[i].quantity_raw;
        }
      },
      [](std::unordered_map<int64_t, int64_t> &into,
         std::unordered_map<int64_t, int64_t> &&from) {
        for (const auto &[orderkey, net] : from)
          into[orderkey] += net;
      });

  // The published image is never mutated: clone lazily, only when a survivor
  // actually changes (RF chunks on small orders leave the state untouched).
  std::shared_ptr<TpchQ18State> new_state;
  const auto mut = [&]() -> TpchQ18State & {
    if (!new_state)
      new_state = std::make_shared<TpchQ18State>(*base_state);
    return *new_state;
  };

  // Customer renames: refresh the cached names of survivors' customers.
  for (const auto &[custkey, name] : buffer.CustomerNames()) {
    const std::string *cached = base_state->customer_names.Find(custkey);
    if (cached && *cached != name)
      mut().customer_names.Upsert(custkey, name);
  }

  for (const auto &[orderkey, net] : net_by_order) {
    if (net == 0)
      continue;
    const bool is_survivor = base_state->HasOrderMeta(orderkey);
    int64_t after = 0;
    if (is_survivor) {
      after = base_state->OrderQuantitySum(orderkey) + net;
    } else if (net < 0) {
      // The state holds every order above the persist threshold, so this
      // order's previous sum was <= threshold and only shrinks.
      continue;
    } else if (buffer.InsertedOrders().count(orderkey) &&
               !buffer.DeletedOrders().count(orderkey)) {
      // Order created in this transaction: previous sum is 0.
      after = net;
    } else if (initial_scan &&
               initial_scan->LookupOrderQuantitySum(orderkey, after)) {
      // Scan includes this transaction: do not add `net` again.
    } else {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    const bool will_be_survivor = after > TpchQ18State::kMinPersistQuantityRaw;

    if (is_survivor && !will_be_survivor) {
      auto &st = mut();
      st.RemoveSurvivor(orderkey);
      st.quantity_overflow.Erase(orderkey);
      continue;
    }
    if (!will_be_survivor)
      continue;

    auto &st = mut();
    st.SetOrderQuantitySum(orderkey, after);
    if (!is_survivor) {
      TpchQ18OrderInput order;
      if (!initial_scan || !initial_scan->LookupOrder(orderkey, order) ||
          order.custkey <= 0) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      st.SetOrderMeta(order.key, order.custkey, order.orderdate_days,
                      order.totalprice_raw);
      if (!st.customer_names.Contains(order.custkey)) {
        std::string name;
        if (initial_scan->LookupCustomerName(order.custkey, name))
          st.customer_names.Upsert(order.custkey, std::move(name));
        else
          initial_scan->ScanCustomersSerial([&](const TpchQ18CustomerInput &input) {
            if (input.key == order.custkey)
              st.customer_names.Upsert(input.key, input.name);
          });
      }
    } else {
      st.SyncSurvivorQuantity(orderkey);
    }
  }

  if (!new_state)
    return outcome;

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

  EmitResult(*state, threshold_raw, sink);
}

TpchQ18State
TpchQ18Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ18State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");

  // Quantity sum per order, hash-sharded so the partials fold shard by shard
  // in parallel.
  auto sums = core::PartitionedBuild<OrderSums>(
      services, [] { return OrderSums{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(
            services, parts, [&](std::size_t p, const TpchQ18LineitemInput &input) {
              sink(p, core::SingleRowBatch<TpchQ18LineitemInput>{input});
            });
      },
      [](OrderSums &partial, const TpchQ18LineitemInput &input) {
        if (input.orderkey >= 0)
          partial.Add(input.orderkey, input.quantity_raw);
      },
      [&](OrderSums &into, OrderSums &&from) {
        into.Flush();
        from.Flush();
        core::ParallelSlices(services, OrderSums::kShards, 1,
                             [&](std::size_t, std::size_t first, std::size_t last) {
                               for (std::size_t s = first; s < last; ++s)
                                 into.FoldShard(s, from);
                             });
      });
  sums.Flush();
  using Sums = std::vector<std::pair<int64_t, int64_t>>;
  Sums above_sums;
  core::ParallelReduce(
      services, OrderSums::kShards, 1, above_sums,
      [&](Sums &found, std::size_t first, std::size_t last) {
        for (std::size_t s = first; s < last; ++s)
          for (const auto &entry : sums.shards[s])
            if (entry.second > TpchQ18State::kMinPersistQuantityRaw)
              found.push_back(entry);
      },
      [](Sums &into, Sums &&from) { into.insert(into.end(), from.begin(), from.end()); });
  const std::unordered_map<int64_t, int64_t> above(above_sums.begin(), above_sums.end());

  // Survivors: orders above the threshold, with their order row joined in.
  using Survivors = std::vector<std::pair<TpchQ18OrderInput, int64_t>>;
  const auto survivors = core::PartitionedBuild<Survivors>(
      services, [] { return Survivors{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(services, parts,
                                            [&](std::size_t p, const TpchQ18OrderInput &input) {
                                              sink(p, core::SingleRowBatch<TpchQ18OrderInput>{input});
                                            });
      },
      [&](Survivors &partial, const TpchQ18OrderInput &input) {
        const auto it = above.find(input.key);
        if (it != above.end() && input.custkey > 0)
          partial.emplace_back(input, it->second);
      },
      [](Survivors &into, Survivors &&from) {
        into.insert(into.end(), from.begin(), from.end());
      });
  std::unordered_set<int64_t> needed_custkeys;
  for (const auto &[order, sum] : survivors) {
    state.SetOrderQuantitySum(order.key, sum);
    state.SetOrderMeta(order.key, order.custkey, order.orderdate_days,
                       order.totalprice_raw);
    needed_custkeys.insert(order.custkey);
  }

  using Names = std::vector<std::pair<int64_t, std::string>>;
  const auto names = core::PartitionedBuild<Names>(
      services, [] { return Names{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanCustomers(
            services, parts, [&](std::size_t p, const TpchQ18CustomerInput &input) {
              sink(p, core::SingleRowBatch<TpchQ18CustomerInput>{input});
            });
      },
      [&](Names &partial, const TpchQ18CustomerInput &input) {
        if (needed_custkeys.count(input.key))
          partial.emplace_back(input.key, input.name);
      },
      [](Names &into, Names &&from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()),
                    std::make_move_iterator(from.end()));
      });
  for (const auto &[custkey, name] : names)
    if (!state.customer_names.Contains(custkey))
      state.customer_names.Upsert(custkey, name);
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
                               core::AuxRowSink &sink) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");

  // The state holds every order above kMinPersistQuantityRaw: with no
  // survivors there is no qualifying order (no SQL at query time).
  const bool no_survivors = state.survivors.size() == 0;
  if (no_survivors &&
      threshold_raw >= TpchQ18State::kMinPersistQuantityRaw) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  if (no_survivors) {
    EmitResultScanOrders(state, threshold_raw, initial_scan, sink);
    return;
  }

  std::vector<TpchQ18OutputRow> output_rows;
  std::unordered_set<int64_t> emitted_orderkeys;

  state.survivors.ForEach([&](int64_t orderkey, const TpchQ18SurvivorMeta &meta) {
    // meta.quantity_sum_raw is kept in sync with the sum on every write.
    if (meta.quantity_sum_raw <= threshold_raw)
      return;
    const std::string *name = state.customer_names.Find(meta.custkey);
    if (!name)
      return;
    emitted_orderkeys.insert(orderkey);
    TpchQ18OutputRow row;
    row.name = *name;
    row.custkey = meta.custkey;
    row.orderkey = orderkey;
    row.orderdate_days = meta.orderdate_days;
    row.totalprice_raw = meta.totalprice_raw;
    row.quantity_sum_raw = meta.quantity_sum_raw;
    output_rows.push_back(std::move(row));
  });

  if (output_rows.empty() && state.customer_names.size() == 0) {
    std::unordered_set<int64_t> needed_custkeys;
    state.survivors.ForEach([&](int64_t orderkey, const TpchQ18SurvivorMeta &meta) {
      const int64_t sum = state.OrderQuantitySum(orderkey);
      if (sum <= threshold_raw)
        return;
      if (meta.custkey > 0)
        needed_custkeys.insert(meta.custkey);
    });
    if (!needed_custkeys.empty()) {
      std::unordered_map<int64_t, std::string> customer_names;
      initial_scan->ScanCustomersSerial([&](const TpchQ18CustomerInput &input) {
        if (needed_custkeys.find(input.key) == needed_custkeys.end())
          return;
        customer_names.emplace(input.key, input.name);
      });
      state.survivors.ForEach([&](int64_t orderkey, const TpchQ18SurvivorMeta &meta) {
        const int64_t sum = state.OrderQuantitySum(orderkey);
        if (sum <= threshold_raw)
          return;
        const auto name_it = customer_names.find(meta.custkey);
        if (name_it == customer_names.end())
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
      });
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
  return TpchQ18CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
