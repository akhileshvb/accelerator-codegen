#include "tpch/q5/mysql/aux/module.hpp"
#include "tpch/q5/calibration_writes.hpp"
#include "prims/mysql_collation.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/partitioned_collect.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include "tpch/q5/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

using core::RadixRangeTree;

// Cell readers distinguish SQL NULL from an unreadable payload.
enum class Cell { Null, Ok, Bad };

Cell IntCell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = std::get<int64_t>(cell.payload);
    return Cell::Ok;
  }
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return Cell::Ok;
  }
  return Cell::Bad;
}

Cell Int32Cell(const core::AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  const Cell c = IntCell(row, index, value);
  if (c != Cell::Ok)
    return c;
  if (value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return Cell::Bad;
  out = static_cast<int32_t>(value);
  return Cell::Ok;
}

Cell StringCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (!std::holds_alternative<std::string>(cell.payload))
    return Cell::Bad;
  out = std::get<std::string>(cell.payload);
  return Cell::Ok;
}

// A decimal value rescaled to cents (scale 2); false if that would lose digits.
bool ScaleToCents(int64_t raw, int32_t scale, int64_t &cents) {
  if (scale == 2) {
    cents = raw;
    return true;
  }
  if (scale < 0 || scale > 18)
    return false;
  int64_t factor = 1;
  for (int32_t i = 0; i < (scale < 2 ? 2 - scale : scale - 2); ++i)
    factor *= 10;
  if (scale < 2)
    return !__builtin_mul_overflow(raw, factor, &cents);
  if (raw % factor != 0)
    return false;
  cents = raw / factor;
  return true;
}

Cell CentsCell(const core::AuxRow &row, std::size_t index, int64_t &cents) {
  if (index >= row.values.size())
    return Cell::Bad;
  const auto &cell = row.values[index];
  if (cell.is_null)
    return Cell::Null;
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto &d = std::get<core::ScaledDecimal>(cell.payload);
    return ScaleToCents(d.value, d.scale, cents) ? Cell::Ok : Cell::Bad;
  }
  if (std::holds_alternative<int64_t>(cell.payload)) {
    cents = std::get<int64_t>(cell.payload);
    return Cell::Ok;
  }
  return Cell::Bad;
}

// Revenue of a scanned lineitem; false if its decimals are not representable.
bool InputRevenue(const TpchQ5LineitemInput &row, TpchQ5Money &revenue) {
  int64_t price = 0, discount = 0;
  if (!ScaleToCents(row.extended_price_raw, row.extended_price_scale, price) ||
      !ScaleToCents(row.discount_raw, row.discount_scale, discount))
    return false;
  revenue = TpchQ5Revenue(price, discount);
  return true;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

std::shared_ptr<const TpchQ5StateImage>
MakeImage(TpchQ5ImageKind kind, const std::shared_ptr<const TpchQ5State> &state) {
  auto image = std::make_shared<TpchQ5StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const TpchQ5StateImage *AsQ5Image(const core::StateImage &image,
                                  TpchQ5ImageKind kind) {
  const auto *typed = image.As<TpchQ5StateImage>();
  return typed && typed->kind == kind && typed->state ? typed : nullptr;
}

// The join tuple of an order: its date and its customer's nation. Present only
// if both exist (otherwise the order can never reach the revenue sum).
struct Tuple {
  bool present = false;
  int32_t day = 0;
  int32_t nation = kTpchQ5NoNation;
  bool operator==(const Tuple &o) const {
    return present == o.present &&
           (!present || (day == o.day && nation == o.nation));
  }
};

Tuple OrderTuple(const TpchQ5State &s, int64_t orderkey) {
  const int32_t nation = s.order_nation.Get(orderkey);
  if (nation == kTpchQ5NoNation)
    return {};
  return {true, s.order_date.Get(orderkey), nation};
}

void SetOrderTuple(TpchQ5State &s, int64_t orderkey, const Tuple &t) {
  s.order_date.Set(orderkey, t.present ? t.day : kTpchQ5NoDate);
  s.order_nation.Set(orderkey, t.present ? t.nation : kTpchQ5NoNation);
}

// bucket(day, nation) += (revenue, rows)
void AddBucket(TpchQ5State &s, int32_t day, int32_t nation, TpchQ5Money revenue,
               int64_t rows) {
  const int64_t key = day + TpchQ5State::kDayBias;
  if (RadixRangeTree<TpchQ5Money>::InRange(key)) {
    auto &trees = s.trees[nation];
    trees.revenue.Add(key, revenue);
    trees.rows.Add(key, rows);
    return;
  }
  const auto bucket_key = std::make_pair(static_cast<int64_t>(day), nation);
  auto &bucket = s.outliers[bucket_key];
  bucket.revenue += revenue;
  bucket.rows += rows;
  if (bucket.rows == 0 && bucket.revenue == 0)
    s.outliers.erase(bucket_key);
}

// A lineitem of an order with tuple `t` counts iff its supplier's nation is the
// order's customer's nation (c_nationkey = s_nationkey).
void ApplyLineitem(TpchQ5State &s, const Tuple &t, int64_t suppkey,
                   TpchQ5Money revenue, int sign) {
  if (t.present && s.supp_nation.Get(suppkey) == t.nation)
    AddBucket(s, t.day, t.nation, static_cast<TpchQ5Money>(sign) * revenue, sign);
}

}  // namespace

int32_t TpchQ5DaysFromYmd(int32_t year, unsigned month, unsigned day) {
  return static_cast<int32_t>(core::sql_date::DaysFromCivil(year, month, day));
}

// date + INTERVAL 'months' MONTH with PostgreSQL/MySQL end-of-month clamping.
int64_t TpchQ5AddMonths(int64_t days_since_epoch, int64_t months) {
  return core::sql_date::AddMonths(days_since_epoch, months);
}

TpchQ5Money TpchQ5Revenue(int64_t extended_price_raw, int64_t discount_raw) {
  return static_cast<TpchQ5Money>(extended_price_raw) * (100 - discount_raw);
}

// TxnBuffer

bool TpchQ5TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ5TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitems.clear();
  orders.clear();
  customers.clear();
  suppliers.clear();
  nations.clear();
  regions.clear();
}
bool TpchQ5TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
void TpchQ5TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
}
void TpchQ5TxnBuffer::Add(const LineitemDelta &d) {
  pending = true;
  lineitems.push_back(d);
}
void TpchQ5TxnBuffer::Add(const OrderDelta &d) {
  pending = true;
  orders.push_back(d);
}
void TpchQ5TxnBuffer::AddCustomer(const KeyNationDelta &d) {
  pending = true;
  customers.push_back(d);
}
void TpchQ5TxnBuffer::AddSupplier(const KeyNationDelta &d) {
  pending = true;
  suppliers.push_back(d);
}
void TpchQ5TxnBuffer::Add(const NationDelta &d) {
  pending = true;
  nations.push_back(d);
}
void TpchQ5TxnBuffer::Add(const RegionDelta &d) {
  pending = true;
  regions.push_back(d);
}

// Module Implementation

core::AcceleratorId TpchQ5Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ5Module::Dependencies() const {
  return {
      {config.region_table, {0, 1}, {0, 1}},
      {config.nation_table, {0, 1, 2}, {0, 1, 2}},
      {config.customer_table, {0, 1}, {0, 3}},
      {config.supplier_table, {0, 1}, {0, 3}},
      {config.orders_table, {0, 1, 2}, {0, 1, 4}},
      {config.lineitem_table, {0, 1, 2, 3}, {0, 2, 5, 6}},
  };
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ5Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ5TxnBuffer>();
}

void TpchQ5Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  const auto state = std::make_shared<const TpchQ5State>(BuildState(services));
  for (const auto &[part, kind] :
       {std::make_pair(TpchQ5Parts::kSide, TpchQ5ImageKind::Side),
        std::make_pair(TpchQ5Parts::kTree, TpchQ5ImageKind::Tree),
        std::make_pair(TpchQ5Parts::kOrders, TpchQ5ImageKind::Orders),
        std::make_pair(TpchQ5Parts::kCustomers, TpchQ5ImageKind::Customers),
        std::make_pair(TpchQ5Parts::kSuppliers, TpchQ5ImageKind::Suppliers)})
    builder.Write(core::StateImage::Make(kId, part, 0, MakeImage(kind, state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ5Module::InterpretMutation(const core::MutationBatch &mutation) const {
  for (const auto &dep : Dependencies()) {
    if (dep.table == mutation.table) {
      core::InvalidationPlan plan;
      plan.scope =
          ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
      return plan;
    }
  }
  return {};
}

void TpchQ5Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &buf) {
  auto &buffer = dynamic_cast<TpchQ5TxnBuffer &>(buf);

  // Every table is replayed as (-old row, +new row) deltas.
  auto replay = [&](auto &&add) {
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
  };

  if (mutation.table == config.region_table) {
    replay([&](const core::AuxRow &row, int8_t sign) {
      TpchQ5TxnBuffer::RegionDelta d;
      d.sign = sign;
      const Cell k = Int32Cell(row, 0, d.key);
      const Cell n = StringCell(row, 1, d.name);
      if (k == Cell::Bad || n == Cell::Bad)
        return buffer.MarkRebuild();
      if (k == Cell::Null)
        return;  // a NULL regionkey joins nothing
      d.has_name = n == Cell::Ok;
      buffer.Add(d);
    });
  } else if (mutation.table == config.nation_table) {
    replay([&](const core::AuxRow &row, int8_t sign) {
      TpchQ5TxnBuffer::NationDelta d;
      d.sign = sign;
      const Cell k = Int32Cell(row, 0, d.key);
      const Cell n = StringCell(row, 1, d.nation.name);
      const Cell r = Int32Cell(row, 2, d.nation.regionkey);
      if (k == Cell::Bad || n == Cell::Bad || r == Cell::Bad)
        return buffer.MarkRebuild();
      if (k == Cell::Null)
        return;
      d.nation.name_is_null = n == Cell::Null;
      d.nation.has_region = r == Cell::Ok;
      buffer.Add(d);
    });
  } else if (mutation.table == config.customer_table ||
             mutation.table == config.supplier_table) {
    const bool customer = mutation.table == config.customer_table;
    replay([&](const core::AuxRow &row, int8_t sign) {
      TpchQ5TxnBuffer::KeyNationDelta d;
      d.sign = sign;
      int32_t nation = 0;
      const Cell k = IntCell(row, 0, d.key);
      const Cell n = Int32Cell(row, 1, nation);
      if (k == Cell::Bad || n == Cell::Bad)
        return buffer.MarkRebuild();
      if (k == Cell::Null)
        return;
      d.nationkey = n == Cell::Ok ? nation : kTpchQ5NoNation;
      customer ? buffer.AddCustomer(d) : buffer.AddSupplier(d);
    });
  } else if (mutation.table == config.orders_table) {
    replay([&](const core::AuxRow &row, int8_t sign) {
      TpchQ5TxnBuffer::OrderDelta d;
      d.sign = sign;
      const Cell k = IntCell(row, 0, d.orderkey);
      const Cell c = IntCell(row, 1, d.custkey);
      const Cell t = Int32Cell(row, 2, d.orderdate_days);
      if (k == Cell::Bad || c == Cell::Bad || t == Cell::Bad)
        return buffer.MarkRebuild();
      if (k == Cell::Null)
        return;
      d.has_tuple = c == Cell::Ok && t == Cell::Ok;
      buffer.Add(d);
    });
  } else if (mutation.table == config.lineitem_table) {
    replay([&](const core::AuxRow &row, int8_t sign) {
      TpchQ5TxnBuffer::LineitemDelta d;
      d.sign = sign;
      int64_t price = 0, discount = 0;
      const Cell k = IntCell(row, 0, d.orderkey);
      const Cell s = IntCell(row, 1, d.suppkey);
      const Cell p = CentsCell(row, 2, price);
      const Cell c = CentsCell(row, 3, discount);
      if (k == Cell::Bad || s == Cell::Bad || p == Cell::Bad || c == Cell::Bad)
        return buffer.MarkRebuild();
      // A NULL key, price or discount never reaches the SUM.
      if (k == Cell::Null || s == Cell::Null || p == Cell::Null ||
          c == Cell::Null)
        return;
      d.revenue = TpchQ5Revenue(price, discount);
      buffer.Add(d);
    });
  }
}

core::StagedMaintenanceOutcome
TpchQ5Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &buf) {
  auto &buffer = dynamic_cast<TpchQ5TxnBuffer &>(buf);
  core::StagedMaintenanceOutcome outcome;
  auto rebuild_all = [&]() {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.updates.clear();
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  };

  const bool facts = !buffer.Lineitems().empty() || !buffer.Orders().empty();
  const bool dims_keyed =
      !buffer.Customers().empty() || !buffer.Suppliers().empty();
  const bool dims_named = !buffer.Nations().empty() || !buffer.Regions().empty();
  if (!buffer.RequiresRebuild() && !facts && !dims_keyed && !dims_named)
    return outcome;

  // A batch that changes customers / suppliers together with orders /
  // lineitems is rare; it is rebuilt rather than reasoned about jointly.
  const bool mixed = facts && dims_keyed;
  if (buffer.RequiresRebuild() || mixed) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  const uint64_t total = buffer.Lineitems().size() + buffer.Orders().size() +
                         buffer.Customers().size() + buffer.Suppliers().size() +
                         buffer.Nations().size() + buffer.Regions().size();
  const auto decision = services.ChooseMaintenance(
      {total, 1, true, false, true, false, false, core::MaintenanceAction::NoOp,
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
  if ((facts || dims_keyed) && !initial_scan)
    return rebuild_all();

  // One private working copy of the published images (directory / small-map
  // copies); writes below clone only the chunks they touch.
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ5Parts::kSide, TpchQ5Parts::kTree, TpchQ5Parts::kOrders,
       TpchQ5Parts::kCustomers, TpchQ5Parts::kSuppliers});
  const TpchQ5StateImage *side = nullptr, *tree = nullptr, *orders = nullptr,
                         *customers = nullptr, *suppliers = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ5Parts::kSide)
      side = AsQ5Image(image, TpchQ5ImageKind::Side);
    else if (image.partition == TpchQ5Parts::kTree)
      tree = AsQ5Image(image, TpchQ5ImageKind::Tree);
    else if (image.partition == TpchQ5Parts::kOrders)
      orders = AsQ5Image(image, TpchQ5ImageKind::Orders);
    else if (image.partition == TpchQ5Parts::kCustomers)
      customers = AsQ5Image(image, TpchQ5ImageKind::Customers);
    else if (image.partition == TpchQ5Parts::kSuppliers)
      suppliers = AsQ5Image(image, TpchQ5ImageKind::Suppliers);
  }
  if (!side || !tree || !orders || !customers || !suppliers)
    return rebuild_all();
  auto work = std::make_shared<TpchQ5State>();
  work->nations = side->state->nations;
  work->region_names = side->state->region_names;
  work->trees = tree->state->trees;
  work->outliers = tree->state->outliers;
  work->order_date = orders->state->order_date;
  work->order_nation = orders->state->order_nation;
  work->cust_nation = customers->state->cust_nation;
  work->supp_nation = suppliers->state->supp_nation;

  bool unsupported_decimal = false;
  auto revenue_of = [&](const TpchQ5LineitemInput &row) {
    TpchQ5Money revenue = 0;
    if (!InputRevenue(row, revenue))
      unsupported_decimal = true;
    return revenue;
  };

  // --- nation / region: replayed on the side maps (they only name buckets).
  for (const auto &d : buffer.Nations()) {
    if (d.sign < 0)
      work->nations.erase(d.key);
    else
      work->nations[d.key] = d.nation;
  }
  for (const auto &d : buffer.Regions()) {
    if (d.sign < 0 || !d.has_name)
      work->region_names.erase(d.key);
    else
      work->region_names[d.key] = d.name;
  }

  // --- customer: a changed nation relocates the customer's orders (and so
  // all their lineitems) from the old nation's bucket to the new one's.
  if (!buffer.Customers().empty()) {
    struct Net {
      int32_t before, after;
    };
    std::map<int64_t, Net> net;
    for (const auto &d : buffer.Customers()) {
      auto it = net.find(d.key);
      if (it == net.end()) {
        const int32_t before = work->cust_nation.Get(d.key);
        it = net.emplace(d.key, Net{before, before}).first;
      }
      it->second.after = d.sign < 0 ? kTpchQ5NoNation : d.nationkey;
    }
    std::vector<int64_t> changed;
    for (const auto &[key, n] : net)
      if (n.before != n.after)
        changed.push_back(key);
    struct Moved {
      int32_t day, before, after;
    };
    std::unordered_map<int64_t, Moved> moved;
    initial_scan->LookupOrdersByCustomer(changed, [&](const TpchQ5OrderInput &o) {
      const Net &n = net.at(o.custkey);
      moved[o.orderkey] = {o.orderdate_days, n.before, n.after};
    });
    std::vector<int64_t> moved_keys;
    moved_keys.reserve(moved.size());
    for (const auto &[key, m] : moved)
      moved_keys.push_back(key);
    initial_scan->LookupLineitemsByOrder(
        moved_keys, [&](const TpchQ5LineitemInput &row) {
          const Moved &m = moved.at(row.orderkey);
          const TpchQ5Money revenue = revenue_of(row);
          const int32_t supplier_nation = work->supp_nation.Get(row.suppkey);
          if (m.before != kTpchQ5NoNation && supplier_nation == m.before)
            AddBucket(*work, m.day, m.before, -revenue, -1);
          if (m.after != kTpchQ5NoNation && supplier_nation == m.after)
            AddBucket(*work, m.day, m.after, revenue, +1);
        });
    for (const auto &[key, m] : moved)
      SetOrderTuple(*work, key,
                    m.after == kTpchQ5NoNation ? Tuple{}
                                               : Tuple{true, m.day, m.after});
    for (const auto &[key, n] : net)
      work->cust_nation.Set(key, n.after);
  }

  // --- supplier: a changed nation moves each of its lineitems between "counts
  // for its order's nation" and "does not".
  if (!buffer.Suppliers().empty()) {
    struct Net {
      int32_t before, after;
    };
    std::map<int64_t, Net> net;
    for (const auto &d : buffer.Suppliers()) {
      auto it = net.find(d.key);
      if (it == net.end()) {
        const int32_t before = work->supp_nation.Get(d.key);
        it = net.emplace(d.key, Net{before, before}).first;
      }
      it->second.after = d.sign < 0 ? kTpchQ5NoNation : d.nationkey;
    }
    std::vector<int64_t> changed;
    for (const auto &[key, n] : net)
      if (n.before != n.after)
        changed.push_back(key);
    initial_scan->LookupLineitemsBySupplier(
        changed, [&](const TpchQ5LineitemInput &row) {
          const Tuple t = OrderTuple(*work, row.orderkey);
          if (!t.present)
            return;
          const Net &n = net.at(row.suppkey);
          const TpchQ5Money revenue = revenue_of(row);
          if (t.nation == n.before)
            AddBucket(*work, t.day, t.nation, -revenue, -1);
          if (t.nation == n.after)
            AddBucket(*work, t.day, t.nation, revenue, +1);
        });
    for (const auto &[key, n] : net)
      work->supp_nation.Set(key, n.after);
  }

  // --- orders / lineitem.  Per order key: the join tuple before and after the
  // batch (order changes replayed in sequence: delete, then insert for an
  // update).  A key whose tuple changed (RF1 insert, RF2 delete, update)
  // relocates ALL of its lineitems, including ones this batch did not touch:
  // ask the database for them (index lookup).  Then the batch's own lineitem
  // inserts / deletes are corrected against the OLD tuple, because the lookup
  // already reflects them.
  bool orders_changed = false;
  if (facts) {
    struct Change {
      Tuple before, after;
    };
    std::map<int64_t, Change> changes;
    for (const auto &d : buffer.Orders()) {
      auto it = changes.find(d.orderkey);
      if (it == changes.end()) {
        const Tuple before = OrderTuple(*work, d.orderkey);
        it = changes.emplace(d.orderkey, Change{before, before}).first;
      }
      if (d.sign < 0 || !d.has_tuple) {
        it->second.after = {};
      } else {
        const int32_t nation = work->cust_nation.Get(d.custkey);
        it->second.after =
            nation == kTpchQ5NoNation ? Tuple{} : Tuple{true, d.orderdate_days, nation};
      }
    }
    std::vector<int64_t> moved;
    for (const auto &[key, c] : changes)
      if (!(c.before == c.after))
        moved.push_back(key);
    initial_scan->LookupLineitemsByOrder(moved, [&](const TpchQ5LineitemInput &row) {
      const Change &c = changes.at(row.orderkey);
      const TpchQ5Money revenue = revenue_of(row);
      ApplyLineitem(*work, c.after, row.suppkey, revenue, +1);
      ApplyLineitem(*work, c.before, row.suppkey, revenue, -1);
    });
    for (const auto &d : buffer.Lineitems()) {
      const auto it = changes.find(d.orderkey);
      const Tuple before =
          it != changes.end() ? it->second.before : OrderTuple(*work, d.orderkey);
      ApplyLineitem(*work, before, d.suppkey, d.revenue, d.sign);
    }
    for (const auto &[key, c] : changes)
      if (!(c.before == c.after)) {
        SetOrderTuple(*work, key, c.after);
        orders_changed = true;
      }
  }

  if (unsupported_decimal)
    return rebuild_all();

  const std::shared_ptr<const TpchQ5State> published = work;
  outcome.action = core::MaintenanceAction::ApplyDelta;
  auto publish = [&](core::PartitionId part, TpchQ5ImageKind kind) {
    outcome.updates.push_back(core::StateImage::Make(
        kId, part, 0, MakeImage(kind, published)));
  };
  if (dims_named)
    publish(TpchQ5Parts::kSide, TpchQ5ImageKind::Side);
  if (facts || dims_keyed)
    publish(TpchQ5Parts::kTree, TpchQ5ImageKind::Tree);
  if (orders_changed || !buffer.Customers().empty())
    publish(TpchQ5Parts::kOrders, TpchQ5ImageKind::Orders);
  if (!buffer.Customers().empty())
    publish(TpchQ5Parts::kCustomers, TpchQ5ImageKind::Customers);
  if (!buffer.Suppliers().empty())
    publish(TpchQ5Parts::kSuppliers, TpchQ5ImageKind::Suppliers);
  return outcome;
}

bool TpchQ5Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

namespace {

bool RegionArg(const core::AuxValue &value, std::string &name, bool &is_null) {
  is_null = value.is_null;
  if (value.is_null)
    return true;
  if (!std::holds_alternative<std::string>(value.payload))
    return false;
  name = std::get<std::string>(value.payload);
  return true;
}

bool DateArg(const core::AuxValue &value, int64_t &days, bool &is_null) {
  is_null = value.is_null;
  if (value.is_null)
    return true;
  if (std::holds_alternative<core::AuxDate>(value.payload)) {
    days = std::get<core::AuxDate>(value.payload).days_since_epoch;
    return true;
  }
  if (std::holds_alternative<int64_t>(value.payload)) {
    days = std::get<int64_t>(value.payload);
    return true;
  }
  return false;
}

// Groups for r_name = region and o_orderdate in [start, start + 1 year).
// `side` supplies nations / regions, `tree` the revenue trees (they may be the
// same object).
std::vector<TpchQ5Group> AnswerFrom(const TpchQ5State &side,
                                    const TpchQ5State &tree,
                                    const core::QueryArguments &arguments) {
  std::string region;
  bool region_null = false, date_null = false;
  int64_t start = 0;
  if (arguments.values.size() < 2 ||
      !RegionArg(arguments.values[0], region, region_null) ||
      !DateArg(arguments.values[1], start, date_null))
    throw std::invalid_argument("TPC-H Q5: bad arguments");
  // r_name = NULL / o_orderdate >= NULL are never true.
  if (region_null || date_null)
    return {};
  const int64_t end = core::sql_date::AddYears(start, 1);

  // Nations of any region with that name.
  std::set<int32_t> eligible;
  for (const auto &[nationkey, nation] : side.nations) {
    if (!nation.has_region)
      continue;
    const auto r = side.region_names.find(nation.regionkey);
    if (r != side.region_names.end() && mysql_collation::EqualsCi(r->second, region))
      eligible.insert(nationkey);
  }

  struct Total {
    TpchQ5Money revenue = 0;
    int64_t rows = 0;
  };
  std::map<std::pair<bool, std::string>, Total> by_name;  // GROUP BY n_name
  auto add = [&](int32_t nationkey, TpchQ5Money revenue, int64_t rows) {
    const TpchQ5Nation &nation = side.nations.at(nationkey);
    auto &total = by_name[{nation.name_is_null, nation.name}];
    total.revenue += revenue;
    total.rows += rows;
  };
  const int64_t a = start + TpchQ5State::kDayBias;
  const int64_t b = end + TpchQ5State::kDayBias;
  for (const int32_t nationkey : eligible) {
    const auto t = tree.trees.find(nationkey);
    if (t != tree.trees.end())
      add(nationkey, t->second.revenue.SumRange(a, b),
          t->second.rows.SumRange(a, b));
  }
  for (auto it = tree.outliers.lower_bound({start, std::numeric_limits<int32_t>::min()});
       it != tree.outliers.end() && it->first.first < end; ++it)
    if (eligible.count(it->first.second))
      add(it->first.second, it->second.revenue, it->second.rows);

  std::vector<TpchQ5Group> groups;
  for (const auto &[name, total] : by_name)
    if (total.rows > 0)  // a group exists iff some lineitem joined
      groups.push_back({name.second, name.first, total.revenue});
  return groups;
}

}  // namespace

void TpchQ5Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  if (arguments.values.size() < 2)
    return ExecuteFallback(services, arguments, sink);
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ5Parts::kSide, TpchQ5Parts::kTree});
  const TpchQ5StateImage *side = nullptr, *tree = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ5Parts::kSide)
      side = AsQ5Image(image, TpchQ5ImageKind::Side);
    else if (image.partition == TpchQ5Parts::kTree)
      tree = AsQ5Image(image, TpchQ5ImageKind::Tree);
  }
  if (!side || !tree)
    return ExecuteFallback(services, arguments, sink);
  TpchQ5FinalizeAndEmit(AnswerFrom(*side->state, *tree->state, arguments), sink);
}

// Exact answer without published state: scan the tables.
void TpchQ5Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  const auto state = BuildState(services);
  TpchQ5FinalizeAndEmit(AnswerFrom(state, state, arguments), sink);
}

TpchQ5State TpchQ5Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q5 requires a typed initial scan source");
  TpchQ5State state;

  initial_scan->ScanRegions([&](const TpchQ5RegionInput &input) {
    state.region_names[input.key] = input.name;
  });
  initial_scan->ScanNations([&](const TpchQ5NationInput &input) {
    TpchQ5Nation nation;
    nation.name = input.name;
    nation.name_is_null = input.name_is_null;
    nation.regionkey = input.regionkey;
    nation.has_region = !input.regionkey_is_null;
    state.nations[input.key] = std::move(nation);
  });
  const std::size_t partitions = BuildPartitions(services);
  for (const auto &input : CollectRows<TpchQ5CustomerInput>(
           services, partitions, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanCustomersPartitioned(services, parts, sink);
           }))
    state.cust_nation.Set(input.custkey, input.nationkey);
  for (const auto &input : CollectRows<TpchQ5SupplierInput>(
           services, partitions, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanSuppliersPartitioned(services, parts, sink);
           }))
    state.supp_nation.Set(input.suppkey, input.nationkey);

  // An order joins only through a customer that has a nation (the customers
  // are final here, so partitions only read them).
  struct JoinedOrder {
    int64_t orderkey;
    Tuple tuple;
  };
  const auto orders = core::PartitionedBuild<std::vector<JoinedOrder>>(
      services, [] { return std::vector<JoinedOrder>{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrdersPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ5OrderInput &input) {
              sink(partition, OneRowBatch<TpchQ5OrderInput>{input});
            });
      },
      [&](std::vector<JoinedOrder> &into, const TpchQ5OrderInput &input) {
        const int32_t nation = state.cust_nation.Get(input.custkey);
        if (nation != kTpchQ5NoNation)
          into.push_back({input.orderkey, Tuple{true, input.orderdate_days, nation}});
      },
      [](std::vector<JoinedOrder> &into, std::vector<JoinedOrder> &&from) {
        into.insert(into.end(), from.begin(), from.end());
      },
      partitions);
  for (const auto &order : orders)
    SetOrderTuple(state, order.orderkey, order.tuple);

  // Revenue sums are exact integers: partitions add into their own buckets.
  using Buckets = std::map<std::pair<int32_t, int32_t>, TpchQ5Bucket>;
  const auto buckets = core::PartitionedBuild<Buckets>(
      services, [] { return Buckets{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitemsPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ5LineitemInput &input) {
              sink(partition, OneRowBatch<TpchQ5LineitemInput>{input});
            });
      },
      [&](Buckets &into, const TpchQ5LineitemInput &input) {
        TpchQ5Money revenue = 0;
        if (!InputRevenue(input, revenue))
          throw std::runtime_error("TPC-H Q5: unsupported decimal scale");
        const Tuple tuple = OrderTuple(state, input.orderkey);
        if (!tuple.present || state.supp_nation.Get(input.suppkey) != tuple.nation)
          return;
        auto &bucket = into[{tuple.day, tuple.nation}];
        bucket.revenue += revenue;
        bucket.rows += 1;
      },
      [](Buckets &into, Buckets &&from) {
        for (const auto &[key, bucket] : from) {
          into[key].revenue += bucket.revenue;
          into[key].rows += bucket.rows;
        }
      },
      partitions);
  for (const auto &[key, bucket] : buckets)
    AddBucket(state, key.first, key.second, bucket.revenue, bucket.rows);
  return state;
}

std::vector<core::CalibrationWrite> TpchQ5Module::CalibrationWrites() const {
  return TpchQ5CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
