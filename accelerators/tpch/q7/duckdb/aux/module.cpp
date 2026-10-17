#include "tpch/q7/duckdb/aux/module.hpp"
#include "tpch/q7/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include "tpch/q7/layout.hpp"

namespace accelerator_codegen::aux_state::query {

namespace {

namespace sql_date = core::sql_date;

// Per-partition sums of the lineitem scan.
struct LineitemPartial {
  std::vector<int64_t> revenue_1995, revenue_1996; // supplier nation x customer nation
  std::unordered_map<int64_t, int64_t> supp_orphans, cust_orphans;
};

// Shards per slice when sorting a built index.
constexpr std::size_t kSortShardGrain = 1024;

// Touched mid arrays per slice when publishing a side index.
constexpr std::size_t kMidArrayGrain = 16;

constexpr core::PartitionId kRevenuePartition = 1;
// extendedprice * (100 - discount), both at scale 2.
constexpr double kRevenueUnit = 10000.0;
// Nation keys per name kept on the stack when emitting.
constexpr std::size_t kInlineNationKeys = 32;

int32_t YearFromDays(int32_t days) { return sql_date::YearOf(days); }

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

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &suppkey, int32_t &shipdate_days,
                  int64_t &extended_raw, int32_t &extended_scale,
                  int64_t &discount_raw, int32_t &discount_scale) {
  // Mutation / scan layout: orderkey, suppkey, extendedprice, discount, shipdate
  return core::IntCell(row, 0, orderkey) && core::IntCell(row, 1, suppkey) &&
         core::DecimalCell(row, 2, extended_raw, extended_scale) &&
         core::DecimalCell(row, 3, discount_raw, discount_scale) &&
         core::DateCell(row, 4, shipdate_days);
}

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

struct LoadedState {
  std::shared_ptr<const TpchQ7State> side, revenue;
  core::Version side_version = 0, revenue_version = 0;
};

LoadedState LoadState(core::AuxEngineServices &services) {
  LoadedState loaded;
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ7Layout::kSidePartition, kRevenuePartition});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == kRevenuePartition) {
      if (const auto *typed = AsQ7Image(image, TpchQ7ImageKind::Revenue)) {
        loaded.revenue = typed->state;
        loaded.revenue_version = image.version;
      }
    } else if (image.partition == core::TpchQ7Layout::kSidePartition) {
      if (const auto *typed = AsQ7Image(image, TpchQ7ImageKind::Side)) {
        loaded.side = typed->state;
        loaded.side_version = image.version;
      }
    }
  }
  return loaded;
}

// Replays the buffered ops over the published state: dimension and order
// changes go to overlays, revenue changes to a copy of the grid. A false
// Resolve means the delta cannot be applied exactly and the state rebuilds.
class OpResolver {
public:
  OpResolver(const TpchQ7State &side, const TpchQ7State &base_rev,
             TpchQ7InitialScan *initial_scan, const std::vector<TpchQ7Op> &ops)
      : side(side), initial_scan(initial_scan), ops(ops),
        slots(base_rev.num_nation_slots > 0 ? base_rev.num_nation_slots
                                            : kTpchQ7MinNationSlots),
        supp(side.supplier_nations), cust(side.customer_nations),
        ords(side.order_custkeys), rev95(base_rev.revenue_1995),
        rev96(base_rev.revenue_1996) {
    rev95.resize(static_cast<std::size_t>(slots) * slots, 0);
    rev96.resize(static_cast<std::size_t>(slots) * slots, 0);
    // Point SQL (one per distinct key, cached) is only exact when the DB state
    // it sees equals the pre-transaction state for every table it joins: so no
    // lineitem/order ops, and no ops on the opposite dimension, in this txn.
    for (const auto &op : ops) {
      has_li_order |= op.type == TpchQ7Op::Lineitem || op.type == TpchQ7Op::Order;
      has_supp |= op.type == TpchQ7Op::Supplier;
      has_cust |= op.type == TpchQ7Op::Customer;
    }
  }

  bool Resolve() {
    std::vector<const TpchQ7Op *> deferred;
    for (const auto &op : ops) {
      if (rebuild)
        break;
      switch (op.type) {
      case TpchQ7Op::Supplier:
        ApplyDimension(supp, true, op);
        break;
      case TpchQ7Op::Customer:
        ApplyDimension(cust, false, op);
        break;
      case TpchQ7Op::Order:
        ApplyOrder(op);
        break;
      case TpchQ7Op::Lineitem:
        if (!ApplyLineitem(op, false))
          deferred.push_back(&op);
        break;
      }
    }
    if (!rebuild)
      for (const auto *op : deferred)
        ApplyLineitem(*op, true);
    return !rebuild;
  }

  bool SideChanged() const {
    return supp.txn.Dirty() || cust.txn.Dirty() || ords.txn.Dirty();
  }

  std::shared_ptr<const TpchQ7State> FinishSide(core::AuxEngineServices &services) {
    auto new_side = std::make_shared<TpchQ7State>();
    new_side->nation_names = side.nation_names;
    new_side->num_nation_slots = side.num_nation_slots;
    const auto for_each_mid = [&](std::size_t n, const auto &fn) {
      core::ParallelEach(services, n, kMidArrayGrain, fn);
    };
    new_side->supplier_nations = supp.txn.Dirty() ? supp.txn.Finish(for_each_mid)
                                 : side.supplier_nations;
    new_side->customer_nations = cust.txn.Dirty() ? cust.txn.Finish(for_each_mid)
                                 : side.customer_nations;
    new_side->order_custkeys = ords.txn.Dirty() ? ords.txn.Finish(for_each_mid)
                               : side.order_custkeys;
    return new_side;
  }

  bool RevenueChanged() const { return revenue_changed; }

  std::shared_ptr<const TpchQ7State> FinishRevenue() {
    auto new_rev = std::make_shared<TpchQ7State>();
    new_rev->revenue_1995 = std::move(rev95);
    new_rev->revenue_1996 = std::move(rev96);
    new_rev->num_nation_slots = slots;
    return new_rev;
  }

private:
  struct Contribution {
    bool ok = false;
    std::vector<std::array<int64_t, 3>> rows; // other nk, year, revenue
  };

  const Contribution *Fetch(bool is_supp, int64_t key) {
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
  }

  // Adds sign * contribution into the grid with this key on the given
  // nationkey side (supplier side when is_supp, else customer side).
  void MoveCells(bool is_supp, int64_t key, int32_t nation, int sign) {
    const auto *c = Fetch(is_supp, key);
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
  }

  void InsertDimension(Overlay<TpchQ7NationCount> &ov, bool is_supp,
                       int64_t key, int64_t nation) {
    const auto e = ov.Get(key);
    if (e && e->nation >= 0) {
      rebuild = true; // duplicate key: bag semantics need a rescan
      return;
    }
    const int32_t cnt = e ? e->window_count : 0;
    if (cnt != 0)
      MoveCells(is_supp, key, static_cast<int32_t>(nation), +1);
    ov.Set(key, {static_cast<int32_t>(nation), cnt});
  }

  void DeleteDimension(Overlay<TpchQ7NationCount> &ov, bool is_supp,
                       int64_t key) {
    const auto e = ov.Get(key);
    if (!e || e->nation < 0)
      return;
    if (e->window_count != 0) {
      MoveCells(is_supp, key, e->nation, -1);
      ov.Set(key, {-1, e->window_count}); // tombstone keeps the count
      return;
    }
    ov.Erase(key);
  }

  void ApplyDimension(Overlay<TpchQ7NationCount> &ov, bool is_supp,
                      const TpchQ7Op &op) {
    if (op.kind == 0) {
      InsertDimension(ov, is_supp, op.k1, op.v1);
    } else if (op.kind == 1) {
      DeleteDimension(ov, is_supp, op.k0);
    } else if (op.k0 == op.k1) {
      const auto e = ov.Get(op.k0);
      if (!e || e->nation < 0) {
        rebuild = true;
        return;
      }
      if (e->window_count != 0) {
        MoveCells(is_supp, op.k0, e->nation, -1);
        MoveCells(is_supp, op.k0, static_cast<int32_t>(op.v1), +1);
      }
      ov.Set(op.k0, {static_cast<int32_t>(op.v1), e->window_count});
    } else {
      DeleteDimension(ov, is_supp, op.k0);
      InsertDimension(ov, is_supp, op.k1, op.v1);
    }
  }

  void ApplyOrder(const TpchQ7Op &op) {
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
  }

  static void AdjustCount(Overlay<TpchQ7NationCount> &ov, int64_t key,
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
  }

  // Returns false when a +1 lineitem cannot be resolved yet (a dimension row
  // may arrive later in the same transaction).
  bool ApplyLineitem(const TpchQ7Op &op, bool final) {
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
    AdjustCount(supp, op.v0, s, op.kind);
    if (custkey)
      AdjustCount(cust, *custkey, c, op.kind);
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
  }

  const TpchQ7State &side;
  TpchQ7InitialScan *initial_scan;
  const std::vector<TpchQ7Op> &ops;
  const int32_t slots;
  Overlay<TpchQ7NationCount> supp, cust;
  Overlay<TpchQ7OrderCust> ords;
  // Orders deleted in this transaction stay resolvable for lineitem deletes
  // that arrive after the order delete.
  std::unordered_map<int64_t, int64_t> deleted_orders;
  std::vector<int64_t> rev95, rev96;
  std::unordered_map<int64_t, Contribution> supp_contrib, cust_contrib;
  bool has_li_order = false, has_supp = false, has_cust = false;
  bool rebuild = false;
  bool revenue_changed = false;
};

} // namespace

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
        same = core::IntCell(mutation.old_rows[i], 0, ok) &&
               core::IntCell(mutation.new_rows[i], 0, nk) && ok == nk &&
               core::StringCell(mutation.old_rows[i], 1, on) &&
               core::StringCell(mutation.new_rows[i], 1, nn) && on == nn;
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
      if (!core::IntCell(row, 0, key) || !core::IntCell(row, 1, val))
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
    if (shipdate_days < sql_date::DaysFromCivil(kTpchQ7MinYear, 1, 1) ||
        shipdate_days > sql_date::DaysFromCivil(kTpchQ7MaxYear, 12, 31))
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

core::StagedMaintenanceOutcome
TpchQ7Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  core::StagedMaintenanceOutcome outcome;
  auto &buffer = dynamic_cast<TpchQ7TxnBuffer &>(base_buffer);
  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (!buffer.HasPendingChanges())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          std::max<std::size_t>(1, buffer.Ops().size()),
          services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  const auto loaded = LoadState(services);
  if (!loaded.side || !loaded.revenue) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  OpResolver resolver(*loaded.side, *loaded.revenue, initial_scan, buffer.Ops());
  if (!resolver.Resolve()) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  if (resolver.SideChanged()) {
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ7Layout::kAccelerator,
        core::TpchQ7Layout::kSidePartition, loaded.side_version,
        MakeStateImage(TpchQ7ImageKind::Side, resolver.FinishSide(services))));
  }
  if (resolver.RevenueChanged()) {
    outcome.updates.push_back(core::StateImage::Make(
        core::TpchQ7Layout::kAccelerator, kRevenuePartition,
        loaded.revenue_version,
        MakeStateImage(TpchQ7ImageKind::Revenue, resolver.FinishRevenue())));
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

  if (!CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto loaded = LoadState(services);
  if (!loaded.side || !loaded.revenue) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(loaded.side, loaded.revenue, nation1, nation2, sink);
}

TpchQ7State TpchQ7Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ7State state;

  if (!initial_scan)
    throw std::logic_error("TPC-H Q7 requires a typed initial scan source");

  initial_scan->ScanNations([&](const TpchQ7NationInput &input) {
    state.nation_names.emplace(input.key, input.name);
  });

  int32_t max_nk = kTpchQ7MinNationSlots - 1;
  for (const auto &[nk, name] : state.nation_names)
    max_nk = std::max(max_nk, nk);

  // Scan into a flat vector first so the page table is sized from the actual
  // row count (no hard-coded scale-factor constants).
  using NcEntry = TpchQ7Map<TpchQ7NationCount>::Element;
  using scan::IntColumn;
  const auto collect_nations = [&](const char *table, const char *key_column,
                                   const char *nation_column) {
    auto rows = CollectTable<NcEntry>(
        services, *initial_scan, table, {IntColumn(key_column), IntColumn(nation_column)},
        [](const auto &row) {
          return NcEntry{row.Int(0), {static_cast<int32_t>(row.Int(1)), 0}};
        });
    for (const auto &e : rows)
      max_nk = std::max(max_nk, e.value.nation);
    return rows;
  };
  auto supplier_rows = collect_nations("supplier", "s_suppkey", "s_nationkey");
  auto customer_rows = collect_nations("customer", "c_custkey", "c_nationkey");
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
    const int32_t lo = sql_date::DaysFromCivil(kTpchQ7MinYear, 1, 1);
    const int32_t hi = sql_date::DaysFromCivil(kTpchQ7MaxYear, 12, 31);
    const std::size_t grid_size = state.revenue_1995.size();
    auto lines = core::PartitionedBuild<LineitemPartial>(
        services,
        [&] { return LineitemPartial{std::vector<int64_t>(grid_size, 0),
                                     std::vector<int64_t>(grid_size, 0), {}, {}}; },
        [&](std::size_t parts, auto &&sink) {
          initial_scan->ScanTable(services, "lineitem",
                                  {IntColumn("l_orderkey"), IntColumn("l_suppkey"),
                                   DecimalColumn("l_extendedprice", 2),
                                   DecimalColumn("l_discount", 2), DateColumn("l_shipdate")},
                                  parts, sink);
        },
        [&](LineitemPartial &p, const auto &row) {
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
        });
    state.revenue_1995 = std::move(lines.revenue_1995);
    state.revenue_1996 = std::move(lines.revenue_1996);
    supp_orphans = std::move(lines.supp_orphans);
    cust_orphans = std::move(lines.cust_orphans);
  }
  for (const auto &[k, n] : supp_orphans)
    supp_b.Add({k, {-1, static_cast<int32_t>(n)}});
  for (const auto &[k, n] : cust_orphans)
    cust_b.Add({k, {-1, static_cast<int32_t>(n)}});
  supp_b.Sort();
  cust_b.Sort();
  state.supplier_nations = supp_b.Finish();
  state.customer_nations = cust_b.Finish();
  return state;
}

void TpchQ7Module::EmitResult(
    const std::shared_ptr<const TpchQ7State> &side,
    const std::shared_ptr<const TpchQ7State> &revenue,
    const std::string &nation1, const std::string &nation2,
    core::AuxRowSink &sink) const {
  // Group by nation *name*: every nation key carrying the name participates.
  // (A handful of keys at most: small on-stack lists, heap only if enormous.)
  struct KeyList {
    int32_t inline_keys[kInlineNationKeys];
    std::vector<int32_t> overflow;
    std::size_t n = 0;
    void Add(int32_t k) {
      if (n < kInlineNationKeys) {
        inline_keys[n++] = k;
        return;
      }
      if (overflow.empty())
        overflow.assign(inline_keys, inline_keys + kInlineNationKeys);
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
      rows[nrows++] = {s, c, kTpchQ7MinYear, static_cast<double>(v95) / kRevenueUnit};
    if (v96 != 0)
      rows[nrows++] = {s, c, kTpchQ7MaxYear, static_cast<double>(v96) / kRevenueUnit};
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
