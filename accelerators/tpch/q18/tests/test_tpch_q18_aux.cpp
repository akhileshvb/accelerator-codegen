#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "substrate/aux/state/invalidation_plan.hpp"
#include "tpch/q18/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ18Config;
using accelerator_codegen::aux_state::query::TpchQ18CustomerInput;
using accelerator_codegen::aux_state::query::TpchQ18InitialScan;
using accelerator_codegen::aux_state::query::TpchQ18LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ18Module;
using accelerator_codegen::aux_state::query::TpchQ18OrderInput;
using accelerator_codegen::aux_state::query::TpchQ18Layout;
using accelerator_codegen::aux_state::InvalidationScope;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}

AuxRow Lineitem(int64_t orderkey, int64_t quantity_raw) {
  // l_orderkey (col0), l_quantity as decimal (col1)
  return {{Int(orderkey), Decimal(quantity_raw, 2)}};
}

AuxRow Order(int64_t orderkey, int64_t custkey, int32_t orderdate_days,
             int64_t totalprice_raw) {
  return {{Int(orderkey), Int(custkey), Date(orderdate_days),
           Decimal(totalprice_raw, 2)}};
}

AuxRow Customer(int64_t custkey, const std::string &name) {
  return {{Int(custkey), String(name)}};
}

// ── Minimal Store ───────────────────────────────────────────────────────────
class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ18Layout::kAccelerator;
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == TpchQ18Layout::kAccelerator && valid
               ? ValidityResult{ValidityCode::Valid, version}
               : ValidityResult{ValidityCode::Invalidated, 0};
  }
  void Invalidate(const InvalidationPlan &) override { valid = false; }
  bool TryPublish(const StagedMaintenanceOutcome &outcome, Version) override {
    for (const auto &image : outcome.updates) {
      auto it = images.find(image.partition);
      if (!valid || it == images.end() ||
          (image.version != 0 && image.version != it->second.version)) {
        valid = false;
        return false;
      }
    }
    for (const auto &image : outcome.updates) {
      auto &published = images[image.partition];
      published = image;
      published.version++;
    }
    valid = true;
    version++;
    return true;
  }
  void InstallBuild(std::vector<StateImage> built) {
    images.clear();
    for (auto &image : built) {
      image.version = 1;
      images.emplace(image.partition, std::move(image));
    }
    valid = !images.empty();
    version = 1;
  }
  std::map<PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
};

// ── Services (implements both AuxEngineServices and TpchQ18InitialScan) ─────
class Services final : public AuxEngineServices, public TpchQ18InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const TpchQ18LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &quantity = std::get<ScaledDecimal>(row.values[1].payload);
      consumer({std::get<int64_t>(row.values[0].payload), quantity.value});
    }
  }
  void ScanOrders(
      const std::function<void(const TpchQ18OrderInput &)> &consumer) override {
    for (const auto &row : orders) {
      const auto &totalprice = std::get<ScaledDecimal>(row.values[3].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<AuxDate>(row.values[2].payload).days_since_epoch,
                totalprice.value});
    }
  }
  void ScanCustomers(
      const std::function<void(const TpchQ18CustomerInput &)> &consumer) override {
    for (const auto &row : customers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  bool LookupOrder(int64_t orderkey, TpchQ18OrderInput &out) override {
    for (const auto &row : orders) {
      if (std::get<int64_t>(row.values[0].payload) != orderkey)
        continue;
      const auto &totalprice = std::get<ScaledDecimal>(row.values[3].payload);
      out = {orderkey, std::get<int64_t>(row.values[1].payload),
             std::get<AuxDate>(row.values[2].payload).days_since_epoch,
             totalprice.value};
      return true;
    }
    return false;
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> lineitems;
  std::vector<AuxRow> orders;
  std::vector<AuxRow> customers;
};

// ── Builder ─────────────────────────────────────────────────────────────────
class Builder final : public StateBuilder {
public:
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

// ── Sink ────────────────────────────────────────────────────────────────────
class Sink final : public AuxRowSink {
public:
  void Begin(const OutputSchema &) override {}
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

// ── Helpers ─────────────────────────────────────────────────────────────────
int64_t QuantitySumFromSinkRow(const AuxRow &row) {
  // Output schema: name(str), custkey(i64), orderkey(i64),
  //                orderdate(date), totalprice(decimal), quantity_sum(decimal)
  assert(row.values.size() >= 6);
  assert(!row.values[5].is_null);
  return std::get<ScaledDecimal>(row.values[5].payload).value;
}

int64_t CustkeyFromSinkRow(const AuxRow &row) {
  assert(!row.values[1].is_null);
  return std::get<int64_t>(row.values[1].payload);
}

int64_t OrderkeyFromSinkRow(const AuxRow &row) {
  assert(!row.values[2].is_null);
  return std::get<int64_t>(row.values[2].payload);
}

} // namespace

int main() {
  constexpr int32_t kOrderDate = 10000; // arbitrary date after epoch
  constexpr int64_t kTotalPriceRaw = 99999900; // 999999.00

  // ── Test 1: Basic Build + Execute ───────────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 50000));  // quantity 500.00
    services.lineitems.push_back(Lineitem(10, 30000));  // quantity 300.00
    services.lineitems.push_back(Lineitem(20, 10000));  // quantity 100.00
    services.lineitems.push_back(Lineitem(30, 20000));  // quantity 200.00
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.orders.push_back(Order(20, 2, kOrderDate + 1, kTotalPriceRaw));
    services.orders.push_back(Order(30, 3, kOrderDate + 2, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));
    services.customers.push_back(Customer(2, "Bob"));
    services.customers.push_back(Customer(3, "Carol"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // threshold=200.00 (arg 200 → 20000 raw): order 10 (sum=800) qualifies, 20 (100) no, 30 (200) no
    Sink sink;
    module.Execute(services, QueryArguments{{Int(200)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(OrderkeyFromSinkRow(sink.rows[0]) == 10);
    assert(QuantitySumFromSinkRow(sink.rows[0]) == 80000);
    assert(CustkeyFromSinkRow(sink.rows[0]) == 1);

    // threshold=50.00 (arg 50 → 5000 raw): all three qualify
    Sink sink2;
    module.Execute(services, QueryArguments{{Int(50)}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 3);
    // Sort order: totalprice DESC, orderdate ASC
    // All have same totalprice, so orderdate ASC: 10, 20, 30
    assert(OrderkeyFromSinkRow(sink2.rows[0]) == 10);
    assert(OrderkeyFromSinkRow(sink2.rows[1]) == 20);
    assert(OrderkeyFromSinkRow(sink2.rows[2]) == 30);
  }

  // ── Test 2: Mutation (INSERT lineitem) ──────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 20000)); // 200.00
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // threshold=250.00 (arg 250): initial sum=200 → not qualifying
    Sink sink;
    module.Execute(services, QueryArguments{{Int(250)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);

    // Insert another lineitem for order 10: quantity 100.00 → sum=300.00
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ18Module>(TpchQ18Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    MutationBatch insert;
    insert.table = 1;
    insert.kind = MutationKind::Insert;
    insert.new_rows = {Lineitem(10, 10000)};
    services.lineitems.push_back(insert.new_rows.front());
    coordinator.OnMutation(insert);
    coordinator.PreCommit();
    coordinator.Commit();

    // Now threshold=250.00 should qualify
    Sink sink2;
    module.Execute(services, QueryArguments{{Int(250)}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 1);
    assert(QuantitySumFromSinkRow(sink2.rows[0]) == 30000);
  }

  // ── Test 3: Mutation (DELETE lineitem) ──────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 50000));
    services.lineitems.push_back(Lineitem(10, 30000));
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // threshold=700.00: sum=800 → qualifies
    Sink sink;
    module.Execute(services, QueryArguments{{Int(700)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(QuantitySumFromSinkRow(sink.rows[0]) == 80000);

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ18Module>(TpchQ18Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    MutationBatch del;
    del.table = 1;
    del.kind = MutationKind::Delete;
    del.old_rows = {Lineitem(10, 50000)};
    // Remove from services scan
    services.lineitems.erase(services.lineitems.begin());
    coordinator.OnMutation(del);
    coordinator.PreCommit();
    coordinator.Commit();

    // threshold=700.00: sum=300 → no longer qualifies
    Sink sink2;
    module.Execute(services, QueryArguments{{Int(700)}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 0);
  }

  // ── Test 4: UPDATE lineitem ─────────────────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 20000));
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ18Module>(TpchQ18Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    MutationBatch update;
    update.table = 1;
    update.kind = MutationKind::Update;
    update.old_rows = {Lineitem(10, 20000)};
    update.new_rows = {Lineitem(10, 90000)};
    services.lineitems[0] = update.new_rows.front();
    coordinator.OnMutation(update);
    coordinator.PreCommit();
    coordinator.Commit();

    // threshold=500.00: sum=900 → qualifies
    Sink sink;
    module.Execute(services, QueryArguments{{Int(500)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(QuantitySumFromSinkRow(sink.rows[0]) == 90000);
  }

  // ── Test 5: UPDATE with orderkey change ─────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 20000));
    services.lineitems.push_back(Lineitem(20, 10000));
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.orders.push_back(Order(20, 2, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));
    services.customers.push_back(Customer(2, "Bob"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ18Module>(TpchQ18Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    // Move lineitem from order 10 → 20
    MutationBatch update;
    update.table = 1;
    update.kind = MutationKind::Update;
    update.old_rows = {Lineitem(10, 20000)};
    update.new_rows = {Lineitem(20, 20000)};
    services.lineitems[0] = update.new_rows.front();
    coordinator.OnMutation(update);
    coordinator.PreCommit();
    coordinator.Commit();

    // threshold=100.00: order 10 sum=0 (no), order 20 sum=300 → qualifies
    Sink sink;
    module.Execute(services, QueryArguments{{Int(100)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(OrderkeyFromSinkRow(sink.rows[0]) == 20);
    assert(QuantitySumFromSinkRow(sink.rows[0]) == 30000);
  }

  // ── Test 6: Orders Insert is incremental (no MarkRebuild); Execute still works
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 30000));
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ18Module>(TpchQ18Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    MutationBatch ord_mut;
    ord_mut.table = 2; // orders
    ord_mut.kind = MutationKind::Insert;
    ord_mut.new_rows = {Order(99, 9, kOrderDate, 0)};
    auto plan = module.InterpretMutation(ord_mut);
    assert(plan.scope == InvalidationScope::PartitionSet);

    coordinator.OnMutation(ord_mut);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink;
    module.Execute(services, QueryArguments{{Int(200)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(OrderkeyFromSinkRow(sink.rows[0]) == 10);
  }

  // ── Test 6b: RF-style insert order+lineitem then delete, Execute still works
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 30000));
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));
    services.customers.push_back(Customer(2, "Bob"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ18Module>(TpchQ18Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    services.orders.push_back(Order(20, 2, kOrderDate, kTotalPriceRaw));
    MutationBatch ord_ins;
    ord_ins.table = 2;
    ord_ins.kind = MutationKind::Insert;
    ord_ins.new_rows = {Order(20, 2, kOrderDate, kTotalPriceRaw)};
    coordinator.OnMutation(ord_ins);

    services.lineitems.push_back(Lineitem(20, 35000));
    MutationBatch li_ins;
    li_ins.table = 1;
    li_ins.kind = MutationKind::Insert;
    li_ins.new_rows = {Lineitem(20, 35000)};
    coordinator.OnMutation(li_ins);
    coordinator.PreCommit();
    coordinator.Commit();

    {
      Sink sink;
      module.Execute(services, QueryArguments{{Int(200)}}, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 2);
    }

    // The deletes are in the tables too (maintenance reads them back).
    services.lineitems.pop_back();
    services.orders.pop_back();
    MutationBatch li_del;
    li_del.table = 1;
    li_del.kind = MutationKind::Delete;
    li_del.old_rows = {Lineitem(20, 35000)};
    coordinator.OnMutation(li_del);
    MutationBatch ord_del;
    ord_del.table = 2;
    ord_del.kind = MutationKind::Delete;
    ord_del.old_rows = {Order(20, 2, kOrderDate, kTotalPriceRaw)};
    coordinator.OnMutation(ord_del);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink;
    module.Execute(services, QueryArguments{{Int(200)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(OrderkeyFromSinkRow(sink.rows[0]) == 10);
  }

  // ── Test 7: Fallback when state missing ─────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 30000));
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    // Don't build — state is missing

    Sink sink;
    module.Execute(services, QueryArguments{{Int(200)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1); // fallback computes from scan
  }

  // ── Test 8: Sort order (different totalprices) ──────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(10, 50000));
    services.lineitems.push_back(Lineitem(20, 50000));
    services.lineitems.push_back(Lineitem(30, 50000));
    services.orders.push_back(
        Order(10, 1, kOrderDate, 10000000)); // totalprice=100000.00
    services.orders.push_back(
        Order(20, 2, kOrderDate + 5, 30000000)); // totalprice=300000.00
    services.orders.push_back(
        Order(30, 3, kOrderDate + 2, 20000000)); // totalprice=200000.00
    services.customers.push_back(Customer(1, "A"));
    services.customers.push_back(Customer(2, "B"));
    services.customers.push_back(Customer(3, "C"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{Int(100)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 3);
    // totalprice DESC: 300000, 200000, 100000 (orderkeys: 20, 30, 10)
    assert(OrderkeyFromSinkRow(sink.rows[0]) == 20);
    assert(OrderkeyFromSinkRow(sink.rows[1]) == 30);
    assert(OrderkeyFromSinkRow(sink.rows[2]) == 10);
  }

  // ── Test 9: Empty lineitem set ──────────────────────────────────────────
  {
    Services services;
    // No lineitems at all
    services.orders.push_back(Order(10, 1, kOrderDate, kTotalPriceRaw));
    services.customers.push_back(Customer(1, "Alice"));

    TpchQ18Module module(TpchQ18Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{Int(100)}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  return 0;
}