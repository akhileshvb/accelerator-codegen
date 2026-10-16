#include "tpch/q7/duckdb/aux/module.hpp"
#include "tpch/q7/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include "tpch/q7/layout.hpp"


namespace accelerator_codegen::aux_state::query {

namespace {

using core::ByteReader;
using core::ByteWriter;

// Per-partition sums of the lineitem scan.
struct LineitemPartial {
  std::vector<int64_t> revenue_1995, revenue_1996; // supplier nation x customer nation
  std::unordered_map<int64_t, int64_t> supp_orphans, cust_orphans;
  size_t lineitem_count, lineitem_valid;
};

// Shards per slice when sorting a built index.
constexpr std::size_t kSortShardGrain = 1024;

// Touched mid arrays per slice when publishing a side index.
constexpr std::size_t kFinishGrain = 16;

bool CustomProfileEnabled() {
  const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
  return v != nullptr && v[0] != '\0';
}

struct CustomTimer {
  std::chrono::steady_clock::time_point start;
  const char *phase;
  explicit CustomTimer(const char *p)
      : start(std::chrono::steady_clock::now()), phase(p) {}
  void Emit(size_t rows, size_t groups) {
    auto end = std::chrono::steady_clock::now();
    const double us =
        std::chrono::duration<double, std::micro>(end - start).count();
    std::fprintf(stderr,
                 "ACCEL_CUSTOM_PROFILE {\"phase\":\"%s\",\"us\":%.1f,"
                 "\"rows\":%zu,\"groups\":%zu}\n",
                 phase, us, rows, groups);
  }
};

constexpr uint32_t kVersion = 1;
constexpr uint32_t kSideVersion = 6;
constexpr uint32_t kRevenueVersion = 4;
constexpr uint32_t kSideMagic = 0x53303751;
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

int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

void CivilFromDays(int32_t days, int32_t &year, unsigned &month,
                   unsigned &day) {
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

int32_t YearFromDays(int32_t days) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days, year, month, day);
  return year;
}

std::shared_ptr<const TpchQ7StateImage>
MakeStateImage(TpchQ7ImageKind kind,
               const std::shared_ptr<const TpchQ7State> &state) {
  auto image = std::make_shared<TpchQ7StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const TpchQ7StateImage *AsQ7Image(const core::StateImage &image,
                                  TpchQ7ImageKind kind) {
  const auto *payload = image.As<TpchQ7StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

constexpr core::PartitionId kRevenuePartition = 1;

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, int64_t &custkey) {
  return IntCell(row, 0, orderkey) && IntCell(row, 1, custkey);
}

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &suppkey, int32_t &shipdate_days,
                  int64_t &extended_raw, int32_t &extended_scale,
                  int64_t &discount_raw, int32_t &discount_scale) {
  // Mutation / scan layout: orderkey, suppkey, extendedprice, discount, shipdate
  return IntCell(row, 0, orderkey) && IntCell(row, 1, suppkey) &&
         RawDecimalCell(row, 2, extended_raw, extended_scale) &&
         RawDecimalCell(row, 3, discount_raw, discount_scale) &&
         DateCell(row, 4, shipdate_days);
}

std::vector<uint8_t> EncodeTpchQ7Side(const TpchQ7State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kSideMagic, kSideVersion,
                         static_cast<uint64_t>(state.nation_names.size() +
                                               state.supplier_nations.size() +
                                               state.customer_nations.size() +
                                               state.order_custkeys.size()));
  writer.U32(static_cast<uint32_t>(state.nation_names.size()));
  for (const auto &[nk, name] : state.nation_names) {
    writer.I32(nk);
    writer.String(name);
  }
  const auto write_nc = [&](const TpchQ7Map<TpchQ7NationCount> &m) {
    writer.U32(static_cast<uint32_t>(m.size()));
    m.ForEach([&](const TpchQ7Map<TpchQ7NationCount>::Element &e) {
      writer.I64(e.key);
      writer.I32(e.value.nation);
      writer.I32(e.value.window_count);
    });
  };
  write_nc(state.supplier_nations);
  write_nc(state.customer_nations);
  writer.U64(state.order_custkeys.size());
  state.order_custkeys.ForEach(
      [&](const TpchQ7Map<TpchQ7OrderCust>::Element &e) {
        writer.I64(e.key);
        writer.I64(e.value.custkey);
      });
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ7Revenue(const TpchQ7State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kSideMagic, kRevenueVersion,
                         static_cast<uint64_t>(state.revenue_1995.size() +
                                               state.revenue_1996.size()));
  writer.U32(static_cast<uint32_t>(state.num_nation_slots));
  writer.U32(static_cast<uint32_t>(state.revenue_1995.size()));
  for (const auto v : state.revenue_1995)
    writer.I64(v);
  writer.U32(static_cast<uint32_t>(state.revenue_1996.size()));
  for (const auto v : state.revenue_1996)
    writer.I64(v);
  return std::move(writer.bytes);
}

bool DecodeTpchQ7Side(const std::vector<uint8_t> &bytes, TpchQ7State &state) {
  ByteReader reader{bytes};
  uint64_t total_count = 0;
  // Older layouts (v1-v4) carried no per-key window counts / full order map and
  // cannot be maintained incrementally: reject so the engine rebuilds.
  if (!core::ReadImageHeader(reader, kSideMagic, kSideVersion, total_count))
    return false;
  state.nation_names.clear();
  state.num_nation_slots = kTpchQ7MinNationSlots;

  uint32_t count = 0;
  if (!reader.U32(count))
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    int32_t nk = 0;
    std::string name;
    if (!reader.I32(nk) || !reader.String(name))
      return false;
    state.nation_names.emplace(nk, std::move(name));
  }
  const auto read_nc = [&](TpchQ7Map<TpchQ7NationCount> &out) {
    uint32_t n = 0;
    if (!reader.U32(n))
      return false;
    TpchQ7Map<TpchQ7NationCount>::Builder builder(n);
    for (uint32_t i = 0; i < n; ++i) {
      int64_t k = 0;
      TpchQ7NationCount v;
      if (!reader.I64(k) || !reader.I32(v.nation) ||
          !reader.I32(v.window_count))
        return false;
      builder.Add({k, v});
    }
    builder.Sort();
    out = builder.Finish();
    return true;
  };
  if (!read_nc(state.supplier_nations) || !read_nc(state.customer_nations))
    return false;
  uint64_t order_count = 0;
  if (!reader.U64(order_count))
    return false;
  TpchQ7Map<TpchQ7OrderCust>::Builder orders(order_count);
  for (uint64_t i = 0; i < order_count; ++i) {
    int64_t ok = 0;
    TpchQ7OrderCust v;
    if (!reader.I64(ok) || !reader.I64(v.custkey))
      return false;
    orders.Add({ok, v});
  }
  orders.Sort();
  state.order_custkeys = orders.Finish();
  for (const auto &[nk, name] : state.nation_names)
    state.num_nation_slots = std::max(state.num_nation_slots, nk + 1);
  const auto widen = [&](const TpchQ7NationCount &v) {
    state.num_nation_slots = std::max(state.num_nation_slots, v.nation + 1);
  };
  state.supplier_nations.ForEach(
      [&](const TpchQ7Map<TpchQ7NationCount>::Element &e) { widen(e.value); });
  state.customer_nations.ForEach(
      [&](const TpchQ7Map<TpchQ7NationCount>::Element &e) { widen(e.value); });
  return reader.offset == bytes.size();
}

bool DecodeTpchQ7Revenue(const std::vector<uint8_t> &bytes, TpchQ7State &state) {
  ByteReader reader{bytes};
  uint64_t total_count = 0;
  if (!core::ReadImageHeader(reader, kSideMagic, 3, total_count)) {
    reader.offset = 0;
    if (!core::ReadImageHeader(reader, kSideMagic, kRevenueVersion, total_count))
      return false;
  }
  state.revenue_1995.clear();
  state.revenue_1996.clear();

  uint32_t count = 0;
  if (!reader.U32(count))
    return false;
  state.num_nation_slots = count == 0
      ? kTpchQ7MinNationSlots : static_cast<int32_t>(count);

  if (!reader.U32(count))
    return false;
  state.revenue_1995.resize(count, 0);
  for (uint32_t i = 0; i < count; ++i) {
    int64_t v = 0;
    if (!reader.I64(v))
      return false;
    state.revenue_1995[i] = v;
  }

  if (!reader.U32(count))
    return false;
  state.revenue_1996.resize(count, 0);
  for (uint32_t i = 0; i < count; ++i) {
    int64_t v = 0;
    if (!reader.I64(v))
      return false;
    state.revenue_1996[i] = v;
  }

  return reader.offset == bytes.size();
}

} // namespace

int32_t TpchQ7DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return DaysFromCivil(year, month, day);
}

int32_t TpchQ7MonthKeyFromDays(int32_t days_since_epoch) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  return year * 100 + static_cast<int32_t>(month);
}

int64_t TpchQ7RevenueRaw(int64_t extended_price_raw, int32_t /*extended_scale*/,
                         int64_t discount_raw, int32_t /*discount_scale*/) {
  return extended_price_raw * (100 - discount_raw);
}

bool TpchQ7TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ7TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  ops.clear();
}
bool TpchQ7TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ7Op> &TpchQ7TxnBuffer::Ops() const { return ops; }
void TpchQ7TxnBuffer::MarkRebuild() {
  requires_rebuild = true;
  pending = true;
}
void TpchQ7TxnBuffer::AddOp(const TpchQ7Op &op) {
  pending = true;
  ops.push_back(op);
}

core::AcceleratorId TpchQ7Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ7Module::Dependencies() const {
  return {{config.nation_table, {0, 1}, {0, 1}},
          {config.supplier_table, {0, 1}, {0, 3}},
          {config.customer_table, {0, 1}, {0, 3}},
          {config.orders_table, {0, 1}, {0, 1}},
          {config.lineitem_table, {0, 1, 2, 3, 4}, {0, 2, 5, 6, 10}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ7Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ7TxnBuffer>();
}

void TpchQ7Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto state = BuildState(services);

  auto side_state = std::make_shared<TpchQ7State>();
  side_state->nation_names = std::move(state.nation_names);
  side_state->supplier_nations = std::move(state.supplier_nations);
  side_state->customer_nations = std::move(state.customer_nations);
  side_state->order_custkeys = std::move(state.order_custkeys);
  side_state->num_nation_slots = state.num_nation_slots;

  builder.Write(core::StateImage::Make(
      core::TpchQ7Layout::kAccelerator,
      core::TpchQ7Layout::kSidePartition, 0,
      MakeStateImage(TpchQ7ImageKind::Side,
                     std::shared_ptr<const TpchQ7State>(side_state))));

  auto revenue_state = std::make_shared<TpchQ7State>();
  revenue_state->revenue_1995 = std::move(state.revenue_1995);
  revenue_state->revenue_1996 = std::move(state.revenue_1996);
  revenue_state->num_nation_slots = state.num_nation_slots;

  builder.Write(core::StateImage::Make(
      core::TpchQ7Layout::kAccelerator, kRevenuePartition, 0,
      MakeStateImage(TpchQ7ImageKind::Revenue,
                     std::shared_ptr<const TpchQ7State>(revenue_state))));
  builder.Finish();
}

core::InvalidationPlan
TpchQ7Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table) {
    // Q7 only needs incremental revenue maintenance for price/discount
    // lineitem writes (the shared harness probe shape). AccumulateMutation
    // resolves a revenue delta and StageMaintenance publishes the side image.
    // Any other lineitem-column change invalidates the single consolidated
    // side partition and rebuilds.
    bool price_or_discount_only = true;
    for (const auto column : mutation.updated_columns) {
      if (column != 2 && column != 3) {
        price_or_discount_only = false;
        break;
      }
    }
    if (price_or_discount_only)
      return {};
  }

  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.customer_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    plan.partitions.insert(core::TpchQ7Layout::kSidePartition);
    plan.partitions.insert(kRevenuePartition);
    return plan;
  }
  return {};
}

void TpchQ7Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  // Capture only: rows are logged in order and resolved against the published
  // state in one pass at StageMaintenance (no store reads per row here).
  auto &buffer = dynamic_cast<TpchQ7TxnBuffer &>(base_buffer);
  if (buffer.RequiresRebuild())
    return;

  if (mutation.table == config.nation_table) {
    // Nation rows feed the name map and nation-key slots; only a no-op update
    // (identical key and name) is ignored.
    if (mutation.kind == core::MutationKind::Update &&
        mutation.old_rows.size() == mutation.new_rows.size()) {
      bool same = true;
      for (std::size_t i = 0; i < mutation.old_rows.size() && same; ++i) {
        int64_t ok = 0, nk = 0;
        std::string on, nn;
        same = IntCell(mutation.old_rows[i], 0, ok) &&
               IntCell(mutation.new_rows[i], 0, nk) && ok == nk &&
               StringCell(mutation.old_rows[i], 1, on) &&
               StringCell(mutation.new_rows[i], 1, nn) && on == nn;
      }
      if (same)
        return;
    }
    buffer.MarkRebuild();
    return;
  }

  const bool is_supplier = mutation.table == config.supplier_table;
  const bool is_customer = mutation.table == config.customer_table;
  const bool is_order = mutation.table == config.orders_table;
  if (is_supplier || is_customer || is_order) {
    const auto type = is_supplier   ? TpchQ7Op::Supplier
                      : is_customer ? TpchQ7Op::Customer
                                    : TpchQ7Op::Order;
    const auto read = [&](const core::AuxRow &row, int64_t &key,
                          int64_t &val) {
      if (!IntCell(row, 0, key) || !IntCell(row, 1, val))
        return false;
      if (!is_order && (val < std::numeric_limits<int32_t>::min() ||
                        val > std::numeric_limits<int32_t>::max()))
        return false;
      return true;
    };
    TpchQ7Op op;
    op.type = type;
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        op.kind = 0;
        if (!read(row, op.k1, op.v1)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddOp(op);
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        op.kind = 1;
        if (!read(row, op.k0, op.v0)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddOp(op);
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        op.kind = 2;
        if (!read(mutation.old_rows[i], op.k0, op.v0) ||
            !read(mutation.new_rows[i], op.k1, op.v1)) {
          buffer.MarkRebuild();
          return;
        }
        // Updates that touch no tracked column are not state changes.
        if (op.k0 == op.k1 && op.v0 == op.v1)
          continue;
        buffer.AddOp(op);
      }
      break;
    }
    return;
  }
  if (mutation.table != config.lineitem_table)
    return;

  const auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0, suppkey = 0;
    int32_t shipdate_days = 0;
    int64_t extended_raw = 0, discount_raw = 0;
    int32_t extended_scale = 0, discount_scale = 0;
    // Full layout: orderkey, suppkey, extendedprice, discount, shipdate. A
    // short (orderkey, price, discount) shape cannot be resolved without a
    // per-lineitem index; rebuild instead of guessing.
    if (!ReadLineitem(row, orderkey, suppkey, shipdate_days, extended_raw,
                      extended_scale, discount_raw, discount_scale)) {
      buffer.MarkRebuild();
      return;
    }
    if (shipdate_days < DaysFromCivil(kTpchQ7MinYear, 1, 1) ||
        shipdate_days > DaysFromCivil(kTpchQ7MaxYear, 12, 31))
      return; // outside the fixed query window: no effect
    TpchQ7Op op;
    op.type = TpchQ7Op::Lineitem;
    op.kind = sign;
    op.k0 = orderkey;
    op.v0 = suppkey;
    op.k1 = shipdate_days;
    op.v1 = TpchQ7RevenueRaw(extended_raw, extended_scale, discount_raw,
                             discount_scale);
    buffer.AddOp(op);
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

namespace {

// Read-through writer over an immutable TpchQ7Map: changes go to a Txn (which
// copies only touched shards) and are published with Finish at the end.
template <class V> struct Overlay {
  typename TpchQ7Map<V>::Txn txn;
  explicit Overlay(const TpchQ7Map<V> &b) : txn(b) {}
  std::optional<V> Get(int64_t key) const {
    const auto *e = txn.Find(key);
    return e ? std::optional<V>(e->value) : std::nullopt;
  }
  void Set(int64_t key, const V &v) { txn.Upsert({key, v}); }
  void Erase(int64_t key) { txn.Erase(key); }
};

} // namespace

core::StagedMaintenanceOutcome
TpchQ7Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  core::StagedMaintenanceOutcome outcome;
  auto &buffer = dynamic_cast<TpchQ7TxnBuffer &>(base_buffer);
  const auto rebuild_all = [&]() {
    outcome.updates.clear();
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  };
  if (buffer.RequiresRebuild())
    return rebuild_all();
  if (!buffer.HasPendingChanges())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(std::max<std::size_t>(1, buffer.Ops().size())),
       1, true, false, true, false, false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action == core::MaintenanceAction::RebuildPartition
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility,
      {core::TpchQ7Layout::kSidePartition, kRevenuePartition});

  std::shared_ptr<const TpchQ7State> side_ptr, revenue_ptr;
  uint64_t side_version = 0, revenue_version = 0;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == kRevenuePartition) {
      const auto *typed = AsQ7Image(image, TpchQ7ImageKind::Revenue);
      if (typed) {
        revenue_ptr = typed->state;
        revenue_version = image.version;
      }
    } else if (image.partition == core::TpchQ7Layout::kSidePartition) {
      const auto *typed = AsQ7Image(image, TpchQ7ImageKind::Side);
      if (typed) {
        side_ptr = typed->state;
        side_version = image.version;
      }
    }
  }
  if (!side_ptr || !revenue_ptr)
    return rebuild_all();

  const TpchQ7State &side = *side_ptr;
  const TpchQ7State &base_rev = *revenue_ptr;
  const int32_t slots = base_rev.num_nation_slots > 0
                            ? base_rev.num_nation_slots
                            : kTpchQ7MinNationSlots;

  Overlay<TpchQ7NationCount> supp(side.supplier_nations);
  Overlay<TpchQ7NationCount> cust(side.customer_nations);
  Overlay<TpchQ7OrderCust> ords(side.order_custkeys);
  // Orders deleted in this transaction stay resolvable for lineitem deletes
  // that arrive after the order delete.
  std::unordered_map<int64_t, int64_t> deleted_orders;
  std::vector<int64_t> rev95 = base_rev.revenue_1995;
  std::vector<int64_t> rev96 = base_rev.revenue_1996;
  rev95.resize(static_cast<std::size_t>(slots) * slots, 0);
  rev96.resize(static_cast<std::size_t>(slots) * slots, 0);
  bool rebuild = false;
  bool revenue_changed = false;

  // Point SQL (one per distinct key, cached) is only exact when the DB state it
  // sees equals the pre-transaction state for every table it joins: so no
  // lineitem/order ops, and no ops on the opposite dimension, in this txn.
  bool has_li_order = false, has_supp = false, has_cust = false;
  for (const auto &op : buffer.Ops()) {
    has_li_order |= op.type == TpchQ7Op::Lineitem || op.type == TpchQ7Op::Order;
    has_supp |= op.type == TpchQ7Op::Supplier;
    has_cust |= op.type == TpchQ7Op::Customer;
  }
  struct Contribution {
    bool ok = false;
    std::vector<std::array<int64_t, 3>> rows; // other nk, year, revenue
  };
  std::unordered_map<int64_t, Contribution> supp_contrib, cust_contrib;
  const auto fetch = [&](bool is_supp, int64_t key) -> const Contribution * {
    if (is_supp ? (has_li_order || has_cust) : (has_li_order || has_supp)) {
      rebuild = true;
      return nullptr;
    }
    auto &cache = is_supp ? supp_contrib : cust_contrib;
    auto it = cache.find(key);
    if (it == cache.end()) {
      Contribution c;
      const auto cb = [&](int32_t nk, int32_t year, int64_t rev) {
        c.rows.push_back({nk, year, rev});
      };
      c.ok = initial_scan && (is_supp ? initial_scan->ScanSupplierContribution(key, cb)
                                      : initial_scan->ScanCustomerContribution(key, cb));
      it = cache.emplace(key, std::move(c)).first;
    }
    if (!it->second.ok) {
      rebuild = true;
      return nullptr;
    }
    return &it->second;
  };
  // Adds sign * contribution into the grid with this key on the given
  // nationkey side (supplier side when is_supp, else customer side).
  const auto move_cells = [&](bool is_supp, int64_t key, int32_t nation,
                              int sign) {
    const auto *c = fetch(is_supp, key);
    if (!c || nation < 0)
      return;
    for (const auto &r : c->rows) {
      const int64_t other = r[0];
      if (other < 0)
        continue;
      if (other >= slots || nation >= slots) {
        rebuild = true;
        return;
      }
      const auto slot = is_supp ? static_cast<std::size_t>(nation) * slots + other
                                : static_cast<std::size_t>(other) * slots + nation;
      if (r[1] == kTpchQ7MinYear)
        rev95[slot] += sign * r[2];
      else if (r[1] == kTpchQ7MaxYear)
        rev96[slot] += sign * r[2];
      revenue_changed = true;
    }
  };

  const auto dim_insert = [&](Overlay<TpchQ7NationCount> &ov, bool is_supp,
                              int64_t key, int64_t nation) {
    const auto e = ov.Get(key);
    if (e && e->nation >= 0) {
      rebuild = true; // duplicate key: bag semantics need a rescan
      return;
    }
    const int32_t cnt = e ? e->window_count : 0;
    if (cnt != 0)
      move_cells(is_supp, key, static_cast<int32_t>(nation), +1);
    ov.Set(key, {static_cast<int32_t>(nation), cnt});
  };
  const auto dim_delete = [&](Overlay<TpchQ7NationCount> &ov, bool is_supp,
                              int64_t key) {
    const auto e = ov.Get(key);
    if (!e || e->nation < 0)
      return;
    if (e->window_count != 0) {
      move_cells(is_supp, key, e->nation, -1);
      ov.Set(key, {-1, e->window_count}); // tombstone keeps the count
      return;
    }
    ov.Erase(key);
  };
  const auto dim_apply = [&](Overlay<TpchQ7NationCount> &ov, bool is_supp,
                             const TpchQ7Op &op) {
    if (op.kind == 0) {
      dim_insert(ov, is_supp, op.k1, op.v1);
    } else if (op.kind == 1) {
      dim_delete(ov, is_supp, op.k0);
    } else if (op.k0 == op.k1) {
      const auto e = ov.Get(op.k0);
      if (!e || e->nation < 0) {
        rebuild = true;
        return;
      }
      if (e->window_count != 0) {
        move_cells(is_supp, op.k0, e->nation, -1);
        move_cells(is_supp, op.k0, static_cast<int32_t>(op.v1), +1);
      }
      ov.Set(op.k0, {static_cast<int32_t>(op.v1), e->window_count});
    } else {
      dim_delete(ov, is_supp, op.k0);
      dim_insert(ov, is_supp, op.k1, op.v1);
    }
  };

  const auto adjust_count = [&](Overlay<TpchQ7NationCount> &ov, int64_t key,
                                std::optional<TpchQ7NationCount> e, int kind) {
    if (!e) {
      if (kind < 0)
        return;
      e = TpchQ7NationCount{-1, 0};
    }
    e->window_count += kind;
    if (e->nation < 0 && e->window_count == 0)
      ov.Erase(key);
    else
      ov.Set(key, *e);
  };

  // Returns false when a +1 lineitem cannot be resolved yet (a dimension row
  // may arrive later in the same transaction).
  const auto process_lineitem = [&](const TpchQ7Op &op, bool final) {
    const auto s = supp.Get(op.v0);
    std::optional<int64_t> custkey;
    if (const auto o = ords.Get(op.k0))
      custkey = o->custkey;
    else if (op.kind < 0) {
      const auto d = deleted_orders.find(op.k0);
      if (d != deleted_orders.end())
        custkey = d->second;
    }
    std::optional<TpchQ7NationCount> c;
    if (custkey)
      c = cust.Get(*custkey);
    const bool s_live = s && s->nation >= 0;
    const bool c_live = c && c->nation >= 0;
    if (op.kind > 0 && !final && (!s_live || !c_live))
      return false;
    adjust_count(supp, op.v0, s, op.kind);
    if (custkey)
      adjust_count(cust, *custkey, c, op.kind);
    if (!s_live || !c_live)
      return true; // contributes nothing to the join
    if (s->nation >= slots || c->nation >= slots) {
      rebuild = true;
      return true;
    }
    const auto year = YearFromDays(static_cast<int32_t>(op.k1));
    const auto slot = static_cast<std::size_t>(s->nation) * slots + c->nation;
    const int64_t amount = op.kind * op.v1;
    if (year == kTpchQ7MinYear)
      rev95[slot] += amount;
    else if (year == kTpchQ7MaxYear)
      rev96[slot] += amount;
    else
      return true;
    revenue_changed = true;
    return true;
  };

  std::vector<const TpchQ7Op *> deferred;
  for (const auto &op : buffer.Ops()) {
    if (rebuild)
      break;
    switch (op.type) {
    case TpchQ7Op::Supplier:
      dim_apply(supp, true, op);
      break;
    case TpchQ7Op::Customer:
      dim_apply(cust, false, op);
      break;
    case TpchQ7Op::Order:
      if (op.kind == 0) {
        if (ords.Get(op.k1))
          rebuild = true;
        else
          ords.Set(op.k1, {op.v1});
      } else if (op.kind == 1) {
        const auto cur = ords.Get(op.k0);
        if (cur) {
          deleted_orders[op.k0] = cur->custkey;
          ords.Erase(op.k0);
        }
      } else {
        rebuild = true; // key/custkey change re-targets existing lineitems
      }
      break;
    case TpchQ7Op::Lineitem:
      if (!process_lineitem(op, false))
        deferred.push_back(&op);
      break;
    }
  }
  if (!rebuild)
    for (const auto *op : deferred)
      process_lineitem(*op, true);
  if (rebuild)
    return rebuild_all();

  const bool side_changed = supp.txn.Dirty() || cust.txn.Dirty() ||
                            ords.txn.Dirty();
  if (side_changed) {
    auto new_side = std::make_shared<TpchQ7State>();
    new_side->nation_names = side.nation_names;
    new_side->num_nation_slots = side.num_nation_slots;
    const auto for_each_mid = [&](std::size_t n, const auto &fn) {
      core::ParallelEach(services, n, kFinishGrain, fn);
    };
    new_side->supplier_nations = supp.txn.Dirty() ? supp.txn.Finish(for_each_mid)
                                 : side.supplier_nations;
    new_side->customer_nations = cust.txn.Dirty() ? cust.txn.Finish(for_each_mid)
                                 : side.customer_nations;
    new_side->order_custkeys = ords.txn.Dirty() ? ords.txn.Finish(for_each_mid)
                               : side.order_custkeys;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ7Layout::kAccelerator,
        core::TpchQ7Layout::kSidePartition, side_version,
        MakeStateImage(TpchQ7ImageKind::Side,
                       std::shared_ptr<const TpchQ7State>(new_side))));
  }
  if (revenue_changed) {
    auto new_rev = std::make_shared<TpchQ7State>();
    new_rev->revenue_1995 = std::move(rev95);
    new_rev->revenue_1996 = std::move(rev96);
    new_rev->num_nation_slots = slots;
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ7Layout::kAccelerator, kRevenuePartition, revenue_version,
        MakeStateImage(TpchQ7ImageKind::Revenue,
                       std::shared_ptr<const TpchQ7State>(new_rev))));
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  return outcome;
}

bool TpchQ7Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ7Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (arguments.values.size() < 2 ||
      arguments.values[0].is_null ||
      arguments.values[1].is_null) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::string nation1, nation2;
  if (std::holds_alternative<std::string>(arguments.values[0].payload))
    nation1 = std::get<std::string>(arguments.values[0].payload);
  if (std::holds_alternative<std::string>(arguments.values[1].payload))
    nation2 = std::get<std::string>(arguments.values[1].payload);
  if (nation1.empty() || nation2.empty()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ7Layout::kSidePartition, kRevenuePartition});

  std::shared_ptr<const TpchQ7State> side;
  std::shared_ptr<const TpchQ7State> revenue;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ7Layout::kSidePartition) {
      const auto *typed = AsQ7Image(image, TpchQ7ImageKind::Side);
      if (typed)
        side = typed->state;
    } else if (image.partition == kRevenuePartition) {
      const auto *typed = AsQ7Image(image, TpchQ7ImageKind::Revenue);
      if (typed)
        revenue = typed->state;
    }
  }

  if (!side || !revenue) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(side, revenue, nation1, nation2, sink);
}

void TpchQ7Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  std::string nation1, nation2;
  if (arguments.values.size() >= 2) {
    if (!arguments.values[0].is_null &&
        std::holds_alternative<std::string>(arguments.values[0].payload))
      nation1 = std::get<std::string>(arguments.values[0].payload);
    if (!arguments.values[1].is_null &&
        std::holds_alternative<std::string>(arguments.values[1].payload))
      nation2 = std::get<std::string>(arguments.values[1].payload);
  }

  auto state = BuildState(services);
  auto mutable_side = std::make_shared<TpchQ7State>();
  mutable_side->nation_names = std::move(state.nation_names);
  mutable_side->supplier_nations = std::move(state.supplier_nations);
  mutable_side->customer_nations = std::move(state.customer_nations);
  mutable_side->order_custkeys = std::move(state.order_custkeys);
  mutable_side->num_nation_slots = state.num_nation_slots;
  auto side = std::shared_ptr<const TpchQ7State>(std::move(mutable_side));

  auto mutable_revenue = std::make_shared<TpchQ7State>();
  mutable_revenue->revenue_1995 = std::move(state.revenue_1995);
  mutable_revenue->revenue_1996 = std::move(state.revenue_1996);
  mutable_revenue->num_nation_slots = state.num_nation_slots;
  auto revenue = std::shared_ptr<const TpchQ7State>(std::move(mutable_revenue));

  EmitResult(side, revenue, nation1, nation2, sink);
}

TpchQ7State TpchQ7Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ7State state;
  const bool profile = CustomProfileEnabled();

  if (!initial_scan)
    throw std::logic_error("TPC-H Q7 requires a typed initial scan source");

  size_t nation_count = 0;
  size_t supplier_count = 0;
  size_t customer_count = 0;
  size_t order_count = 0;
  size_t lineitem_count = 0;
  size_t lineitem_valid = 0;

  {
    CustomTimer timer("scan_nations");
    initial_scan->ScanNations([&](const TpchQ7NationInput &input) {
      state.nation_names.emplace(input.key, input.name);
      ++nation_count;
    });
    if (profile)
      timer.Emit(nation_count, 0);
  }

  int32_t max_nk = kTpchQ7MinNationSlots - 1;
  for (const auto &[nk, name] : state.nation_names)
    max_nk = std::max(max_nk, nk);

  // Scan into a flat vector first so the page table is sized from the actual
  // row count (no hard-coded scale-factor constants).
  using NcEntry = TpchQ7Map<TpchQ7NationCount>::Element;
  std::vector<NcEntry> supplier_rows, customer_rows;
  {
    CustomTimer timer("scan_suppliers");
    initial_scan->ScanSuppliers([&](const TpchQ7SupplierInput &input) {
      supplier_rows.push_back({input.key, {input.nationkey, 0}});
      max_nk = std::max(max_nk, input.nationkey);
      ++supplier_count;
    });
    if (profile)
      timer.Emit(supplier_count, 0);
  }
  {
    CustomTimer timer("scan_customers");
    initial_scan->ScanCustomers([&](const TpchQ7CustomerInput &input) {
      customer_rows.push_back({input.key, {input.nationkey, 0}});
      max_nk = std::max(max_nk, input.nationkey);
      ++customer_count;
    });
    if (profile)
      timer.Emit(customer_count, 0);
  }
  // Dense nation grid wide enough for every nation key present; the revenue
  // image never aliases (supp_nk * slots + cust_nk) across rows.
  state.num_nation_slots = max_nk + 1;
  const auto grid = static_cast<std::size_t>(state.num_nation_slots) *
                    static_cast<std::size_t>(state.num_nation_slots);
  state.revenue_1995.assign(grid, 0);
  state.revenue_1996.assign(grid, 0);

  TpchQ7Map<TpchQ7NationCount>::Builder supp_b(supplier_rows.size());
  for (const auto &e : supplier_rows)
    supp_b.Add(e);
  std::vector<NcEntry>().swap(supplier_rows);
  supp_b.Sort();
  TpchQ7Map<TpchQ7NationCount>::Builder cust_b(customer_rows.size());
  for (const auto &e : customer_rows)
    cust_b.Add(e);
  std::vector<NcEntry>().swap(customer_rows);
  cust_b.Sort();

  {
    CustomTimer timer("scan_orders");
    using scan::IntColumn;
    const int64_t expected = initial_scan->CountOrders();
    auto make_orders = [&] {
      return expected >= 0 ? TpchQ7Map<TpchQ7OrderCust>::Builder(static_cast<std::size_t>(expected))
                           : TpchQ7Map<TpchQ7OrderCust>::Builder();
    };
    auto ob = core::PartitionedBuild<TpchQ7Map<TpchQ7OrderCust>::Builder>(
        services, make_orders,
        [&](std::size_t parts, auto &&sink) {
          initial_scan->ScanTable(services, "orders",
                                  {IntColumn("o_orderkey"), IntColumn("o_custkey")},
                                  parts, sink);
        },
        [](auto &builder, const auto &row) {
          builder.Add({row.Int(0), {row.Int(1)}});
        },
        [](auto &into, auto &&from) { into.Merge(std::move(from)); });
    ob.Sort([&](std::size_t n, const auto &fn) {
      core::ParallelEach(services, n, kSortShardGrain, fn);
    });
    state.order_custkeys = ob.Finish();
    order_count = state.order_custkeys.size();
    if (profile)
      timer.Emit(order_count, 0);
  }

  const auto add_revenue = [&](int32_t supp_nk, int32_t cust_nk, int32_t year,
                               int64_t revenue) {
    if (supp_nk < 0 || cust_nk < 0 || supp_nk >= state.num_nation_slots ||
        cust_nk >= state.num_nation_slots)
      return;
    const auto slot = static_cast<std::size_t>(supp_nk) *
                          state.num_nation_slots + cust_nk;
    if (year == kTpchQ7MinYear)
      state.revenue_1995[slot] += revenue;
    else if (year == kTpchQ7MaxYear)
      state.revenue_1996[slot] += revenue;
  };

  {
    CustomTimer timer("scan_lineitems");
    // Counts for keys with no dimension row become tombstones (nation -1) so a
    // later insert of that key can add the lineitems' revenue.
    std::unordered_map<int64_t, int64_t> supp_orphans, cust_orphans;
    // One build thread: the engine runs join + GROUP BY (supp nation, cust
    // nation, year) and the per-key window counts in SQL. More: the lineitem
    // scan is split into partitions that probe the dimension indexes.
    const bool pushed =
        services.WorkerCount() == 1 &&
        initial_scan->ScanWindowAggregates(
            [&](int32_t s, int32_t c, int32_t y, int64_t r) {
              add_revenue(s, c, y, r);
              ++lineitem_valid;
            },
            [&](int64_t key, int64_t n) {
              if (auto *v = TpchQ7ValMut(supp_b.Find(key)))
                v->window_count = static_cast<int32_t>(n);
              else
                supp_orphans[key] += n;
            },
            [&](int64_t key, int64_t n) {
              if (auto *v = TpchQ7ValMut(cust_b.Find(key)))
                v->window_count = static_cast<int32_t>(n);
              else
                cust_orphans[key] += n;
            });
    if (!pushed) {
      using scan::DateColumn;
      using scan::DecimalColumn;
      using scan::IntColumn;
      const int32_t lo = DaysFromCivil(kTpchQ7MinYear, 1, 1);
      const int32_t hi = DaysFromCivil(kTpchQ7MaxYear, 12, 31);
      const std::size_t grid_size = state.revenue_1995.size();
      auto lines = core::PartitionedBuild<LineitemPartial>(
          services,
          [&] { return LineitemPartial{std::vector<int64_t>(grid_size, 0),
                                       std::vector<int64_t>(grid_size, 0), {}, {}, 0, 0}; },
          [&](std::size_t parts, auto &&sink) {
            initial_scan->ScanTable(services, "lineitem",
                                    {IntColumn("l_orderkey"), IntColumn("l_suppkey"),
                                     DecimalColumn("l_extendedprice", 2),
                                     DecimalColumn("l_discount", 2), DateColumn("l_shipdate")},
                                    parts, sink);
          },
          [&](LineitemPartial &p, const auto &row) {
            ++p.lineitem_count;
            const int32_t shipdate = row.Date(4);
            if (shipdate < lo || shipdate > hi)
              return;
            // The dimension indexes are sorted and fixed: partitions add to
            // their window counters atomically.
            auto *supp_v = TpchQ7ValMut(supp_b.Find(row.Int(1)));
            if (supp_v)
              AtomicAdd<int32_t>(supp_v->window_count, 1);
            else
              ++p.supp_orphans[row.Int(1)];
            const auto *order_v = TpchQ7Val(state.order_custkeys.Find(row.Int(0)));
            if (!order_v)
              return;
            auto *cust_v = TpchQ7ValMut(cust_b.Find(order_v->custkey));
            if (cust_v)
              AtomicAdd<int32_t>(cust_v->window_count, 1);
            else
              ++p.cust_orphans[order_v->custkey];
            if (!supp_v || !cust_v)
              return;
            ++p.lineitem_valid;
            const int32_t supp_nk = supp_v->nation, cust_nk = cust_v->nation;
            if (supp_nk < 0 || cust_nk < 0 || supp_nk >= state.num_nation_slots ||
                cust_nk >= state.num_nation_slots)
              return;
            const auto slot = static_cast<std::size_t>(supp_nk) * state.num_nation_slots + cust_nk;
            const int32_t year = YearFromDays(shipdate);
            const int64_t revenue = TpchQ7RevenueRaw(row.Decimal(2), 2, row.Decimal(3), 2);
            if (year == kTpchQ7MinYear)
              p.revenue_1995[slot] += revenue;
            else if (year == kTpchQ7MaxYear)
              p.revenue_1996[slot] += revenue;
          },
          [](LineitemPartial &into, LineitemPartial &&from) {
            for (std::size_t i = 0; i < into.revenue_1995.size(); ++i) {
              into.revenue_1995[i] += from.revenue_1995[i];
              into.revenue_1996[i] += from.revenue_1996[i];
            }
            for (const auto &[k, n] : from.supp_orphans)
              into.supp_orphans[k] += n;
            for (const auto &[k, n] : from.cust_orphans)
              into.cust_orphans[k] += n;
            into.lineitem_count += from.lineitem_count;
            into.lineitem_valid += from.lineitem_valid;
          });
      state.revenue_1995 = std::move(lines.revenue_1995);
      state.revenue_1996 = std::move(lines.revenue_1996);
      supp_orphans = std::move(lines.supp_orphans);
      cust_orphans = std::move(lines.cust_orphans);
      lineitem_count = lines.lineitem_count;
      lineitem_valid = lines.lineitem_valid;
    }
    for (const auto &[k, n] : supp_orphans)
      supp_b.Add({k, {-1, static_cast<int32_t>(n)}});
    for (const auto &[k, n] : cust_orphans)
      cust_b.Add({k, {-1, static_cast<int32_t>(n)}});
    supp_b.Sort();
    cust_b.Sort();
    if (profile)
      timer.Emit(lineitem_count, lineitem_valid);
  }
  state.supplier_nations = supp_b.Finish();
  state.customer_nations = cust_b.Finish();
  return state;
}

bool TpchQ7Module::LoadState(
    core::AuxEngineServices &services,
    const std::string &, const std::string &,
    std::shared_ptr<const TpchQ7State> &side_out,
    std::vector<std::pair<int32_t, std::shared_ptr<const TpchQ7State>>> &months_out,
    std::map<core::PartitionId, core::Version> *) const {
  (void)months_out;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ7Layout::kSidePartition});

  for (const auto &image : snapshot.partitions) {
    if (image.partition == core::TpchQ7Layout::kSidePartition) {
      const auto *typed = AsQ7Image(image, TpchQ7ImageKind::Side);
      if (typed) {
        side_out = typed->state;
        return side_out != nullptr;
      }
    }
  }
  return false;
}

void TpchQ7Module::EmitResult(
    const std::shared_ptr<const TpchQ7State> &side,
    const std::shared_ptr<const TpchQ7State> &revenue,
    const std::string &nation1, const std::string &nation2,
    core::AuxRowSink &sink) const {
  // Group by nation *name*: every nation key carrying the name participates.
  // (A handful of keys at most: small on-stack lists, heap only if enormous.)
  struct KeyList {
    int32_t inline_keys[32];
    std::vector<int32_t> overflow;
    std::size_t n = 0;
    void Add(int32_t k) {
      if (n < 32) {
        inline_keys[n++] = k;
        return;
      }
      if (overflow.empty())
        overflow.assign(inline_keys, inline_keys + 32);
      overflow.push_back(k);
      ++n;
    }
    const int32_t *data() const {
      return overflow.empty() ? inline_keys : overflow.data();
    }
  };
  KeyList set1, set2;
  for (const auto &[nk, name] : side->nation_names) {
    if (name == nation1)
      set1.Add(nk);
    if (name == nation2)
      set2.Add(nk);
  }
  if (set1.n == 0 || set2.n == 0) {
    sink.Begin({{{"supp_nation", core::AuxType::String, true},
                 {"cust_nation", core::AuxType::String, true},
                 {"l_year", core::AuxType::Int64, true},
                 {"revenue", core::AuxType::Double, true}}});
    sink.End();
    return;
  }

  const int32_t slots = revenue->num_nation_slots;
  const auto sum = [&](const KeyList &from, const KeyList &to, int year) {
    int64_t total = 0;
    const auto &v = year == kTpchQ7MinYear ? revenue->revenue_1995
                                           : revenue->revenue_1996;
    const int32_t *fp = from.data();
    const int32_t *tp = to.data();
    for (std::size_t i = 0; i < from.n; ++i) {
      const int32_t f = fp[i];
      if (f < 0 || f >= slots)
        continue;
      const std::size_t row = static_cast<std::size_t>(f) * slots;
      for (std::size_t j = 0; j < to.n; ++j) {
        const int32_t t = tp[j];
        if (t < 0 || t >= slots)
          continue;
        const std::size_t slot = row + static_cast<std::size_t>(t);
        if (slot < v.size())
          total += v[slot];
      }
    }
    return total;
  };

  const int64_t a_1995 = sum(set1, set2, kTpchQ7MinYear);
  const int64_t a_1996 = sum(set1, set2, kTpchQ7MaxYear);
  // With identical names the OR predicate matches each row once.
  const bool same_name = nation1 == nation2;
  const int64_t b_1995 = same_name ? 0 : sum(set2, set1, kTpchQ7MinYear);
  const int64_t b_1996 = same_name ? 0 : sum(set2, set1, kTpchQ7MaxYear);

  struct ResultRow {
    const std::string *supp_nation;
    const std::string *cust_nation;
    int64_t year;
    double revenue;
  };
  // Rows sorted by (supp_nation, cust_nation, year): group A is
  // (nation1, nation2), group B is (nation2, nation1); order the groups by
  // their (supp, cust) name pair, years ascending inside a group.
  const bool a_first = nation1 < nation2 ||
                       (nation1 == nation2);  // equal names: B is empty
  ResultRow rows[4];
  std::size_t nrows = 0;
  const auto add_group = [&](const std::string *s, const std::string *c,
                             int64_t v95, int64_t v96) {
    if (v95 != 0)
      rows[nrows++] = {s, c, 1995, static_cast<double>(v95) / 10000.0};
    if (v96 != 0)
      rows[nrows++] = {s, c, 1996, static_cast<double>(v96) / 10000.0};
  };
  if (a_first) {
    add_group(&nation1, &nation2, a_1995, a_1996);
    add_group(&nation2, &nation1, b_1995, b_1996);
  } else {
    add_group(&nation2, &nation1, b_1995, b_1996);
    add_group(&nation1, &nation2, a_1995, a_1996);
  }

  sink.Begin({{{"supp_nation", core::AuxType::String, true},
               {"cust_nation", core::AuxType::String, true},
               {"l_year", core::AuxType::Int64, true},
               {"revenue", core::AuxType::Double, true}}});

  for (std::size_t ri = 0; ri < nrows; ++ri) {
    const auto &row = rows[ri];
    core::AuxRow out;
    out.values.reserve(4);
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, *row.supp_nation));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, *row.cust_nation));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Int64, row.year));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Double, row.revenue));
    sink.WriteRow(out);
  }

  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ7Module::CalibrationWrites() const {
  return TpchQ7CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
