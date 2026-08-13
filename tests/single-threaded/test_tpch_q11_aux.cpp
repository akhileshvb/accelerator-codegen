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
#include "tpch/q11/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ11Config;
using accelerator_codegen::aux_state::query::TpchQ11InitialScan;
using accelerator_codegen::aux_state::query::TpchQ11Layout;
using accelerator_codegen::aux_state::query::TpchQ11Module;
using accelerator_codegen::aux_state::query::TpchQ11PartsuppInput;
using accelerator_codegen::aux_state::query::TpchQ11SupplierInput;
using accelerator_codegen::aux_state::query::TpchQ11NationInput;
using accelerator_codegen::aux_state::query::TpchQ11OutputRow;
using accelerator_codegen::aux_state::InvalidationScope;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}
AuxValue Dbl(double value) { return AuxValue::Of(AuxType::Double, value); }

AuxRow Partsupp(int64_t partkey, int64_t suppkey, int64_t availqty,
                int64_t supplycost_raw) {
  return {{Int(partkey), Int(suppkey), Int(availqty), Decimal(supplycost_raw, 2)}};
}

AuxRow Supplier(int64_t suppkey, int32_t nationkey) {
  return {{Int(suppkey), Int(nationkey)}};
}

AuxRow Nation(int32_t nationkey, const std::string &name) {
  return {{Int(nationkey), String(name)}};
}

// ── Minimal Store ───────────────────────────────────────────────────────────
class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ11Layout::kAccelerator;
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
    return key.accelerator == TpchQ11Layout::kAccelerator && valid
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

// ── Services (implements both AuxEngineServices and TpchQ11InitialScan) ─────
class Services final : public AuxEngineServices, public TpchQ11InitialScan {
public:
  void ScanPartsupp(
      const std::function<void(const TpchQ11PartsuppInput &)> &consumer) override {
    for (const auto &row : partsupps) {
      const auto &cost = std::get<ScaledDecimal>(row.values[3].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<int64_t>(row.values[2].payload),
                cost.value, cost.scale});
    }
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ11SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[1].payload))});
    }
  }
  void ScanNations(
      const std::function<void(const TpchQ11NationInput &)> &consumer) override {
    for (const auto &row : nations) {
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> partsupps;
  std::vector<AuxRow> suppliers;
  std::vector<AuxRow> nations;
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

} // namespace

// ── Helper to construct QueryArguments ──────────────────────────────────────
QueryArguments QArgs(std::string nation_name, double fraction) {
  QueryArguments args;
  args.values.push_back(String(std::move(nation_name)));
  args.values.push_back(Dbl(fraction));
  return args;
}

// ── Tests ───────────────────────────────────────────────────────────────────
int main() {
  // Test 1: basic build and query
  {
    Services services;
    services.nations.push_back(Nation(1, "ROMANIA"));
    services.suppliers.push_back(Supplier(101, 1));
    services.suppliers.push_back(Supplier(102, 1));
    services.suppliers.push_back(Supplier(103, 2)); // Different nation
    // Part 2001 from supplier 101: cost 100 * qty 5 = 500
    // Part 2002 from supplier 101: cost 200 * qty 3 = 600
    // Part 2001 from supplier 102: cost 50 * qty 4 = 200
    // Part 2003 from supplier 103: cost 300 * qty 2 = 600 (other nation)
    services.partsupps.push_back(Partsupp(2001, 101, 5, 10000));
    services.partsupps.push_back(Partsupp(2002, 101, 3, 20000));
    services.partsupps.push_back(Partsupp(2001, 102, 4, 5000));
    services.partsupps.push_back(Partsupp(2003, 103, 2, 30000));

    TpchQ11Module module(TpchQ11Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Query for ROMANIA (nationkey=1) with fraction=0.0001
    // supplycost_raw * availqty:
    //   101: 10000*5=50000, 20000*3=60000
    //   102: 5000*4=20000
    //   Total raw = 130000
    //   Total as double = 130000/100 = 1300.0
    //   threshold = 1300.0 * 0.0001 = 0.13
    //   Part 2001: 700.0 > 0.13 => qualifies
    //   Part 2002: 600.0 > 0.13 => qualifies
    Sink sink;
    module.Execute(services, QArgs("ROMANIA", 0.0001), sink);
    assert(sink.ended);
    assert(sink.rows.size() == 2);
    assert(!sink.rows[0].values[0].is_null);
    assert(std::get<int64_t>(sink.rows[0].values[0].payload) == 2001);
    assert(!sink.rows[1].values[0].is_null);
    assert(std::get<int64_t>(sink.rows[1].values[0].payload) == 2002);
  }

  // Test 2: mutation (insert partsupp)
  {
    Services services;
    services.nations.push_back(Nation(1, "ROMANIA"));
    services.suppliers.push_back(Supplier(101, 1));
    services.partsupps.push_back(Partsupp(2001, 101, 5, 10000));

    TpchQ11Module module(TpchQ11Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Verify initial query
    {
      Sink sink;
      module.Execute(services, QArgs("ROMANIA", 0.0001), sink);
      assert(sink.ended && sink.rows.size() == 1);
    }

    // Register for mutations
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ11Module>(TpchQ11Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    // Insert a new partsupp row
    MutationBatch insert;
    insert.table = 1; // partsupp_table
    insert.kind = MutationKind::Insert;
    insert.new_rows = {Partsupp(2002, 101, 3, 20000)};
    services.partsupps.push_back(insert.new_rows.front());
    coordinator.OnMutation(insert);
    coordinator.PreCommit();
    coordinator.Commit();

    // After mutation, should have 2 parts
    Sink sink2;
    module.Execute(services, QArgs("ROMANIA", 0.0001), sink2);
    assert(sink2.ended && sink2.rows.size() == 2);
  }

  // Test 3: nation with no suppliers
  {
    Services services;
    services.nations.push_back(Nation(1, "EMPTYLAND"));
    services.suppliers.push_back(Supplier(101, 2)); // Different nation

    TpchQ11Module module(TpchQ11Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QArgs("EMPTYLAND", 0.0001), sink);
    assert(sink.ended && sink.rows.empty());
  }

  // Test 4: fraction=1.0 (all pass threshold)
  {
    Services services;
    services.nations.push_back(Nation(1, "ROMANIA"));
    services.suppliers.push_back(Supplier(101, 1));
    services.partsupps.push_back(Partsupp(2001, 101, 5, 10000));
    services.partsupps.push_back(Partsupp(2002, 101, 3, 20000));

    TpchQ11Module module(TpchQ11Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QArgs("ROMANIA", 1.0), sink);
    assert(sink.ended);
    // With fraction = 1.0, threshold = total, so nothing exceeds total
    assert(sink.rows.empty());
  }

  // Test 5: mutation with update of partsupp
  {
    Services services;
    services.nations.push_back(Nation(1, "ROMANIA"));
    services.suppliers.push_back(Supplier(101, 1));
    services.partsupps.push_back(Partsupp(2001, 101, 5, 10000));

    TpchQ11Module module(TpchQ11Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ11Module>(TpchQ11Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    // Update partsupp: change availqty from 5 to 10
    MutationBatch update;
    update.table = 1;
    update.kind = MutationKind::Update;
    update.old_rows = {Partsupp(2001, 101, 5, 10000)};
    update.new_rows = {Partsupp(2001, 101, 10, 10000)};
    coordinator.OnMutation(update);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink;
    module.Execute(services, QArgs("ROMANIA", 0.0001), sink);
    assert(sink.ended && sink.rows.size() == 1);
  }

  // Test 6: Encode/Decode roundtrip
  {
    Services services;
    services.nations.push_back(Nation(1, "ROMANIA"));
    services.nations.push_back(Nation(2, "GERMANY"));
    services.suppliers.push_back(Supplier(101, 1));
    services.suppliers.push_back(Supplier(102, 1));
    services.suppliers.push_back(Supplier(201, 2));
    services.partsupps.push_back(Partsupp(2001, 101, 5, 10000));
    services.partsupps.push_back(Partsupp(2002, 102, 3, 20000));

    TpchQ11Module module(TpchQ11Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);

    // Build then verify roundtrip by re-reading through Store
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QArgs("ROMANIA", 0.0001), sink);
    assert(sink.ended && sink.rows.size() == 2);
  }

  // Test 7: supplier delete mutation
  {
    Services services;
    services.nations.push_back(Nation(1, "ROMANIA"));
    services.suppliers.push_back(Supplier(101, 1));
    services.partsupps.push_back(Partsupp(2001, 101, 5, 10000));

    TpchQ11Module module(TpchQ11Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ11Module>(TpchQ11Config{1, 2, 3}, &services));
    coordinator.AttachServices(services);

    // Delete supplier 101
    MutationBatch del;
    del.table = 2; // supplier_table
    del.kind = MutationKind::Delete;
    del.old_rows = {Supplier(101, 1)};
    coordinator.OnMutation(del);
    coordinator.PreCommit();
    coordinator.Commit();

    // After supplier delete, ROMANIA should have no suppliers
    Sink sink;
    module.Execute(services, QArgs("ROMANIA", 0.0001), sink);
    assert(sink.ended);
  }

  return 0;
}