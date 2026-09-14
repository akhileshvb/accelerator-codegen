#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/state/invalidation_plan.hpp"
#include "tpch/q21/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ21Config;
using accelerator_codegen::aux_state::query::TpchQ21InitialScan;
using accelerator_codegen::aux_state::query::TpchQ21Layout;
using accelerator_codegen::aux_state::query::TpchQ21Module;
using accelerator_codegen::aux_state::query::TpchQ21OutputRow;
using accelerator_codegen::aux_state::query::TpchQ21SupplierInput;
using accelerator_codegen::aux_state::query::TpchQ21NationInput;
using accelerator_codegen::aux_state::query::TpchQ21OrderInput;
using accelerator_codegen::aux_state::query::TpchQ21LineitemInput;
using accelerator_codegen::aux_state::InvalidationScope;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}

AuxRow Lineitem(int64_t orderkey, int64_t suppkey, int32_t commitdate,
                int32_t receiptdate) {
  return {{Int(orderkey), Int(suppkey), Date(commitdate), Date(receiptdate)}};
}

AuxRow Order(int64_t orderkey, char orderstatus) {
  return {{Int(orderkey), String(std::string(1, orderstatus))}};
}

AuxRow Supplier(int64_t suppkey, const std::string &name, int32_t nationkey) {
  return {{Int(suppkey), String(name), Int(nationkey)}};
}

AuxRow Nation(int32_t nationkey, const std::string &name) {
  return {{Int(nationkey), String(name)}};
}

// ── Minimal Store ───────────────────────────────────────────────────────────
class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ21Layout::kAccelerator;
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
    return key.accelerator == TpchQ21Layout::kAccelerator && valid
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

// ── Services (implements both AuxEngineServices and TpchQ21InitialScan) ─────
class Services final : public AuxEngineServices, public TpchQ21InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const TpchQ21SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload))});
    }
  }
  void ScanNations(
      const std::function<void(const TpchQ21NationInput &)> &consumer) override {
    for (const auto &row : nations) {
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  void ScanOrders(
      const std::function<void(const TpchQ21OrderInput &)> &consumer) override {
    for (const auto &row : orders) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)[0]});
    }
  }
  void ScanLineitems(
      const std::function<void(const TpchQ21LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<AuxDate>(row.values[2].payload).days_since_epoch,
                std::get<AuxDate>(row.values[3].payload).days_since_epoch});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> suppliers;
  std::vector<AuxRow> nations;
  std::vector<AuxRow> orders;
  std::vector<AuxRow> lineitems;
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
std::string NameFromSinkRow(const AuxRow &row) {
  assert(row.values.size() >= 2);
  assert(!row.values[0].is_null);
  return std::get<std::string>(row.values[0].payload);
}

int64_t NumwaitFromSinkRow(const AuxRow &row) {
  assert(row.values.size() >= 2);
  assert(!row.values[1].is_null);
  return std::get<int64_t>(row.values[1].payload);
}

} // namespace

int main() {
  constexpr int32_t kEarly = 100;   // commitdate
  constexpr int32_t kLate = 200;    // receiptdate > commitdate → late
  const auto kCANADA = QueryArguments{{String("CANADA")}};

  // ── Test 1: Basic Build + Execute ───────────────────────────────────────
  // Setup: nation 10 "CANADA", supplier 1 "S1" in nation 10, supplier 2 "S2" in nation 10
  // Order 100 status='F', with 2 lineitems:
  //   - supplier 1, late (receiptdate > commitdate)
  //   - supplier 1, late (receiptdate > commitdate)
  // Since there's only one supplier (supplier 1), no "other" supplier exists
  // → no contribution (supplier count <= 1 guard)
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "SupplierOne", 10));
    services.suppliers.push_back(Supplier(2, "SupplierTwo", 10));
    services.orders.push_back(Order(100, 'F'));
    services.lineitems.push_back(Lineitem(100, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(100, 1, kEarly, kLate));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // No other supplier on order 100 → no numwait
    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 2: Two suppliers, both late → no numwait (other_late guard) ────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(200, 'F'));
    // Supplier 1 has 1 late
    services.lineitems.push_back(Lineitem(200, 1, kEarly, kLate));
    // Supplier 2 has 1 late
    services.lineitems.push_back(Lineitem(200, 2, kEarly, kLate));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Both suppliers have late items → any_other_late is true → no contributions
    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 3: Single late supplier + other non-late supplier → contributes ─
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(300, 'F'));
    // Supplier 1 has 1 late, 1 non-late
    services.lineitems.push_back(Lineitem(300, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(300, 1, kEarly, kEarly));
    // Supplier 2 has 1 non-late
    services.lineitems.push_back(Lineitem(300, 2, kEarly, kEarly));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // S1: late_count=1, has_other=true, other_late=false → contributes 1
    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NameFromSinkRow(sink.rows[0]) == "S1");
    assert(NumwaitFromSinkRow(sink.rows[0]) == 1);
  }

  // ── Test 4: Multiple late lineitems for same supplier → late_count > 1 ───
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(400, 'F'));
    // Supplier 1 has 3 late
    services.lineitems.push_back(Lineitem(400, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(400, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(400, 1, kEarly, kLate));
    // Supplier 2 has 1 non-late
    services.lineitems.push_back(Lineitem(400, 2, kEarly, kEarly));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NameFromSinkRow(sink.rows[0]) == "S1");
    assert(NumwaitFromSinkRow(sink.rows[0]) == 3);
  }

  // ── Test 5: Non-F order status → no contribution ────────────────────────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(500, 'O')); // 'O' not 'F'
    services.lineitems.push_back(Lineitem(500, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(500, 2, kEarly, kEarly));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 6: Multiple qualifying orders, same supplier → sum numwait ─────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(600, 'F'));
    services.orders.push_back(Order(601, 'F'));
    // Order 600: S1 has 2 late, S2 has 1 non-late → S1 contributes 2
    services.lineitems.push_back(Lineitem(600, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(600, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(600, 2, kEarly, kEarly));
    // Order 601: S1 has 1 late, S2 has 1 non-late → S1 contributes 1
    services.lineitems.push_back(Lineitem(601, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(601, 2, kEarly, kEarly));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NameFromSinkRow(sink.rows[0]) == "S1");
    assert(NumwaitFromSinkRow(sink.rows[0]) == 3);
  }

  // ── Test 7: Multiple qualifying suppliers across orders ─────────────────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.suppliers.push_back(Supplier(3, "S3", 10));
    services.orders.push_back(Order(700, 'F'));
    services.orders.push_back(Order(701, 'F'));
    // Order 700: S1 has 1 late, S2 has 1 non-late → S1 contributes 1
    services.lineitems.push_back(Lineitem(700, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(700, 2, kEarly, kEarly));
    // Order 701: S3 has 2 late, S2 has 1 non-late → S3 contributes 2
    services.lineitems.push_back(Lineitem(701, 3, kEarly, kLate));
    services.lineitems.push_back(Lineitem(701, 3, kEarly, kLate));
    services.lineitems.push_back(Lineitem(701, 2, kEarly, kEarly));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 2);
    // Sorted by numwait DESC, name ASC: S3(2) then S1(1)
    assert(NameFromSinkRow(sink.rows[0]) == "S3");
    assert(NumwaitFromSinkRow(sink.rows[0]) == 2);
    assert(NameFromSinkRow(sink.rows[1]) == "S1");
    assert(NumwaitFromSinkRow(sink.rows[1]) == 1);
  }

  // ── Test 8: Mutation - INSERT lineitem creates new contribution ──────────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(800, 'F'));
    services.lineitems.push_back(Lineitem(800, 1, kEarly, kEarly)); // S1 non-late
    services.lineitems.push_back(Lineitem(800, 2, kEarly, kEarly)); // S2 non-late

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Initially no one has late items → 0 numwait
    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);

    // Insert a late lineitem for S1
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ21Module>(TpchQ21Config{1, 2, 3, 4}, &services));
    coordinator.AttachServices(services);

    MutationBatch insert;
    insert.table = 3; // lineitem
    insert.kind = MutationKind::Insert;
    insert.new_rows = {Lineitem(800, 1, kEarly, kLate)};
    services.lineitems.push_back(insert.new_rows.front());
    coordinator.OnMutation(insert);
    coordinator.PreCommit();
    coordinator.Commit();

    // Now S1 has 1 late, S2 non-late → S1 contributes 1
    Sink sink2;
    module.Execute(services, kCANADA, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 1);
    assert(NameFromSinkRow(sink2.rows[0]) == "S1");
    assert(NumwaitFromSinkRow(sink2.rows[0]) == 1);
  }

  // ── Test 9: Mutation - DELETE lineitem removes contribution ─────────────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(900, 'F'));
    services.lineitems.push_back(Lineitem(900, 1, kEarly, kLate)); // S1 late
    services.lineitems.push_back(Lineitem(900, 2, kEarly, kEarly)); // S2 non-late

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NumwaitFromSinkRow(sink.rows[0]) == 1);

    // Delete S1's late lineitem
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ21Module>(TpchQ21Config{1, 2, 3, 4}, &services));
    coordinator.AttachServices(services);

    MutationBatch del;
    del.table = 3; // lineitem
    del.kind = MutationKind::Delete;
    del.old_rows = {Lineitem(900, 1, kEarly, kLate)};
    services.lineitems.erase(services.lineitems.begin()); // remove the first (late)
    coordinator.OnMutation(del);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink2;
    module.Execute(services, kCANADA, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 0);
  }

  // ── Test 10: Mutation - UPDATE lineitem to late ─────────────────────────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(1000, 'F'));
    services.lineitems.push_back(Lineitem(1000, 1, kEarly, kEarly)); // non-late
    services.lineitems.push_back(Lineitem(1000, 2, kEarly, kEarly)); // non-late

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);

    // Update S1's lineitem to be late
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ21Module>(TpchQ21Config{1, 2, 3, 4}, &services));
    coordinator.AttachServices(services);

    MutationBatch update;
    update.table = 3; // lineitem
    update.kind = MutationKind::Update;
    update.old_rows = {Lineitem(1000, 1, kEarly, kEarly)};
    update.new_rows = {Lineitem(1000, 1, kEarly, kLate)};
    services.lineitems[0] = update.new_rows.front();
    coordinator.OnMutation(update);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink2;
    module.Execute(services, kCANADA, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 1);
    assert(NumwaitFromSinkRow(sink2.rows[0]) == 1);
  }

  // ── Test 11: Order status change from F to O → removes contribution ─────
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 10));
    services.orders.push_back(Order(1100, 'F'));
    services.lineitems.push_back(Lineitem(1100, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(1100, 2, kEarly, kEarly));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);

    // Change order status from 'F' to 'O'
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ21Module>(TpchQ21Config{1, 2, 3, 4}, &services));
    coordinator.AttachServices(services);

    MutationBatch ord_update;
    ord_update.table = 2; // orders
    ord_update.kind = MutationKind::Update;
    ord_update.old_rows = {Order(1100, 'F')};
    ord_update.new_rows = {Order(1100, 'O')};
    services.orders[0] = ord_update.new_rows.front();
    coordinator.OnMutation(ord_update);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink2;
    module.Execute(services, kCANADA, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 0);
  }

  // ── Test 12: InterpretMutation returns PartitionSet for relevant tables ──
  {
    Services services;
    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);

    MutationBatch lineitem_mut;
    lineitem_mut.table = 3;
    lineitem_mut.kind = MutationKind::Insert;
    lineitem_mut.new_rows = {Lineitem(1, 1, 1, 2)};
    auto plan = module.InterpretMutation(lineitem_mut);
    assert(plan.scope == InvalidationScope::PartitionSet);

    MutationBatch orders_mut;
    orders_mut.table = 2;
    orders_mut.kind = MutationKind::Update;
    auto plan2 = module.InterpretMutation(orders_mut);
    assert(plan2.scope == InvalidationScope::PartitionSet);

    MutationBatch irrelevant;
    irrelevant.table = 99;
    irrelevant.kind = MutationKind::Insert;
    auto plan3 = module.InterpretMutation(irrelevant);
    assert(plan3.scope == InvalidationScope::None);
  }

  // ── Test 13: BuildState handles empty data correctly ────────────────────
  {
    Services services;
    // No data at all

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 14: Supplier in different nation still contributes correctly ───
  {
    Services services;
    services.nations.push_back(Nation(10, "CANADA"));
    services.nations.push_back(Nation(20, "USA"));
    services.suppliers.push_back(Supplier(1, "S1", 10));
    services.suppliers.push_back(Supplier(2, "S2", 20));
    services.orders.push_back(Order(1200, 'F'));
    services.lineitems.push_back(Lineitem(1200, 1, kEarly, kLate));
    services.lineitems.push_back(Lineitem(1200, 2, kEarly, kEarly));

    TpchQ21Module module(TpchQ21Config{1, 2, 3, 4}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Query for CANADA — only S1 (nation 10) should appear
    Sink sink;
    module.Execute(services, kCANADA, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NameFromSinkRow(sink.rows[0]) == "S1");
    assert(NumwaitFromSinkRow(sink.rows[0]) == 1);

    // Query for USA — only S2 but S2 has no late items, so 0 rows
    Sink sink2;
    module.Execute(services, QueryArguments{{String("USA")}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 0);
  }

  return 0;
}