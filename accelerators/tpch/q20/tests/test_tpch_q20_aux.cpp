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
#include "tpch/q20/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ20Config;
using accelerator_codegen::aux_state::query::TpchQ20InitialScan;
using accelerator_codegen::aux_state::query::TpchQ20Layout;
using accelerator_codegen::aux_state::query::TpchQ20Module;
using accelerator_codegen::aux_state::query::TpchQ20LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ20PartInput;
using accelerator_codegen::aux_state::query::TpchQ20PartsuppInput;
using accelerator_codegen::aux_state::query::TpchQ20SupplierInput;
using accelerator_codegen::aux_state::query::TpchQ20NationInput;
using accelerator_codegen::aux_state::InvalidationScope;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}

AuxRow Lineitem(int32_t shipdate, int64_t partkey, int64_t suppkey,
                int64_t quantity_raw) {
  return {{Date(shipdate), Int(partkey), Int(suppkey),
           Decimal(quantity_raw, 2)}};
}

AuxRow Part(int64_t partkey, const std::string &name) {
  return {{Int(partkey), String(name)}};
}

AuxRow Partsupp(int64_t partkey, int64_t suppkey, int64_t availqty) {
  return {{Int(partkey), Int(suppkey), Int(availqty)}};
}

AuxRow Supplier(int64_t suppkey, const std::string &name,
                const std::string &address, int32_t nationkey) {
  return {{Int(suppkey), String(name), String(address), Int(nationkey)}};
}

AuxRow Nation(int32_t nationkey, const std::string &name) {
  return {{Int(nationkey), String(name)}};
}

// ── Minimal Store ───────────────────────────────────────────────────────────
class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ20Layout::kAccelerator;
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
    return key.accelerator == TpchQ20Layout::kAccelerator && valid
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

// ── Services (implements both AuxEngineServices and TpchQ20InitialScan) ─────
class Services final : public AuxEngineServices, public TpchQ20InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const TpchQ20LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &quantity = std::get<ScaledDecimal>(row.values[3].payload);
      consumer({std::get<AuxDate>(row.values[0].payload).days_since_epoch,
                std::get<int64_t>(row.values[1].payload),
                std::get<int64_t>(row.values[2].payload),
                quantity.value});
    }
  }
  void ScanParts(
      const std::function<void(const TpchQ20PartInput &)> &consumer) override {
    for (const auto &row : parts) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  void ScanPartsupps(
      const std::function<void(const TpchQ20PartsuppInput &)> &consumer) override {
    for (const auto &row : partsupps) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<int64_t>(row.values[2].payload)});
    }
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ20SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload))});
    }
  }
  void ScanNations(
      const std::function<void(const TpchQ20NationInput &)> &consumer) override {
    for (const auto &row : nations) {
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
    }
  }

  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> lineitems;
  std::vector<AuxRow> parts;
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

// ── Helpers ─────────────────────────────────────────────────────────────────
std::string NameFromSinkRow(const AuxRow &row) {
  assert(row.values.size() >= 2);
  assert(!row.values[0].is_null);
  return std::get<std::string>(row.values[0].payload);
}

std::string AddressFromSinkRow(const AuxRow &row) {
  assert(!row.values[1].is_null);
  return std::get<std::string>(row.values[1].payload);
}

int64_t SuppkeyFromSinkRow(const AuxRow &row) {
  // Not directly available; we match by name for checking
  (void)row;
  return 0;
}

} // namespace

int main() {
  constexpr int32_t kDateBase = 10000; // arbitrary date after epoch
  // Q20 uses DATE '1997-01-01' which is days_since_epoch ~ 2440588 (in DuckDB).
  // For synthetic tests: kDateBase is the start date, end_date = kDateBase + 365.
  constexpr int32_t kEndDateBasis = kDateBase + 365;

  // ── Test 1: Basic Build + Execute (happy path) ──────────────────────────
  // SQL: p_name LIKE 'forest%' — part 10 = "forest fire"; nation "CANADA"
  {
    Services services;
    // Lineitems for (part=10, supp=100): 3 rows within date range, sum=600
    services.lineitems.push_back(Lineitem(kDateBase + 10, 10, 100, 20000)); // 200.00
    services.lineitems.push_back(Lineitem(kDateBase + 20, 10, 100, 40000)); // 400.00
    services.lineitems.push_back(Lineitem(kDateBase + 100, 10, 100, 15000)); // 150.00
    // Lineitems outside date range (excluded)
    services.lineitems.push_back(Lineitem(kDateBase + 400, 10, 100, 99900));
    // Lineitems for (part=20, supp=200): sum in range = 300
    services.lineitems.push_back(Lineitem(kDateBase + 5, 20, 200, 30000)); // 300.00

    services.parts.push_back(Part(10, "forest fire"));
    services.parts.push_back(Part(20, "blaze orange"));

    // partsupp: availqty must be > 0.5 * agg_quantity
    // For (10,100): availqty > 0.5*750 = 375 → availqty 400 passes
    services.partsupps.push_back(Partsupp(10, 100, 400));
    // For (20,200): availqty > 0.5*300 = 150 → availqty 160 passes
    services.partsupps.push_back(Partsupp(20, 200, 160));

    // Only suppkey=100 is in CANADA; suppkey=200 is in USA
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.suppliers.push_back(
        Supplier(200, "Supplier B", "Addr B", 2));

    services.nations.push_back(Nation(1, "CANADA"));
    services.nations.push_back(Nation(2, "USA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Execute: start_date=kDateBase, pattern="forest%", nation="CANADA"
    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    // Only Supplier A (suppkey=100) qualifies:
    // - Part 10 matches "forest%"
    // - (10,100): availqty 400 > 0.5 * (200+400+150) = 375 → pass
    // - Supplier A nationkey=1 → CANADA match
    // - (20,200): part "blaze orange" doesn't match "forest%"
    assert(sink.rows.size() == 1);
    assert(NameFromSinkRow(sink.rows[0]) == "Supplier A");
  }

  // ── Test 2: Exclusion by part name not matching ──────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase + 1, 10, 100, 50000));
    services.parts.push_back(Part(10, "navajo white"));
    services.partsupps.push_back(Partsupp(10, 100, 1000));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // pattern="forest%" should NOT match "navajo white"
    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 3: Exclusion by availqty too low ────────────────────────────────
  // SQL: ps_availqty > agg_quantity (= 0.5 * SUM(l_quantity))
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase + 1, 10, 100, 50000)); // 500.00
    services.parts.push_back(Part(10, "forest green"));
    // availqty=250: need > 0.5*500=250. So 250 does NOT pass (strictly greater)
    services.partsupps.push_back(Partsupp(10, 100, 250));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 4: availqty passes (just above threshold) ───────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase + 1, 10, 100, 50000)); // 500.00
    services.parts.push_back(Part(10, "forest green"));
    // availqty=251: > 250 → passes
    services.partsupps.push_back(Partsupp(10, 100, 251));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NameFromSinkRow(sink.rows[0]) == "Supplier A");
  }

  // ── Test 5: Exclusion by nation ──────────────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase + 1, 10, 100, 50000));
    services.parts.push_back(Part(10, "forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 500));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));
    services.nations.push_back(Nation(2, "USA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Nation "USA" — Supplier A is CANADA (nationkey=1), so excluded
    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("USA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 6: Exclusion by date range ──────────────────────────────────────
  {
    Services services;
    // All lineitems are outside the date range [kDateBase, kDateBase+365)
    services.lineitems.push_back(Lineitem(kDateBase - 100, 10, 100, 50000));
    services.lineitems.push_back(Lineitem(kDateBase + 400, 10, 100, 30000));
    services.parts.push_back(Part(10, "forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 500));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 7: Fallback when state missing ─────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase + 1, 10, 100, 50000));
    services.parts.push_back(Part(10, "forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 500));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    // Don't build — state is missing

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1); // fallback computes from scan
    assert(NameFromSinkRow(sink.rows[0]) == "Supplier A");
  }

  // ── Test 8: Mutation path — insert lineitem, republish, re-execute ──────
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase + 1, 10, 100, 20000)); // 200.00
    services.parts.push_back(Part(10, "forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 250)); // availqty=250
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Before: availqty=250 > 0.5*200=100 → passes
    {
      Sink sink;
      QueryArguments args{
          {Date(kDateBase), String("forest%"), String("CANADA")}};
      module.Execute(services, args, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 1);
    }

    // Insert a new lineitem for the same pair that pushes quantity above
    // availqty threshold
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ20Module>(TpchQ20Config{1, 2, 3, 4, 5},
                                        &services));
    coordinator.AttachServices(services);

    services.lineitems.push_back(
        Lineitem(kDateBase + 5, 10, 100, 40000)); // +400.00 → total 600
    MutationBatch li_ins;
    li_ins.table = 1; // lineitem
    li_ins.kind = MutationKind::Insert;
    li_ins.new_rows = {Lineitem(kDateBase + 5, 10, 100, 40000)};
    coordinator.OnMutation(li_ins);
    coordinator.PreCommit();
    coordinator.Commit();

    // After: availqty=250 ≤ 0.5*(200+400)=300 → fails
    {
      Sink sink;
      QueryArguments args{
          {Date(kDateBase), String("forest%"), String("CANADA")}};
      module.Execute(services, args, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 0);
    }
  }

  // ── Test 9: Mutation — update partsupp availqty ─────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase + 1, 10, 100, 50000));
    services.parts.push_back(Part(10, "forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 250)); // availqty=250 → fails
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Before: availqty=250 ≤ 0.5*500=250 → NOT >, so excluded
    {
      Sink sink;
      QueryArguments args{
          {Date(kDateBase), String("forest%"), String("CANADA")}};
      module.Execute(services, args, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 0);
    }

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ20Module>(TpchQ20Config{1, 2, 3, 4, 5},
                                        &services));
    coordinator.AttachServices(services);

    // Update partsupp availqty from 250 → 300
    MutationBatch ps_update;
    ps_update.table = 3; // partsupp
    ps_update.kind = MutationKind::Update;
    ps_update.old_rows = {Partsupp(10, 100, 250)};
    ps_update.new_rows = {Partsupp(10, 100, 300)};
    coordinator.OnMutation(ps_update);
    coordinator.PreCommit();
    coordinator.Commit();

    // After: availqty=300 > 250 → passes
    {
      Sink sink;
      QueryArguments args{
          {Date(kDateBase), String("forest%"), String("CANADA")}};
      module.Execute(services, args, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 1);
      assert(NameFromSinkRow(sink.rows[0]) == "Supplier A");
    }
  }

  // ── Test 10: Multiple qualifying parts and suppliers ─────────────────────
  {
    Services services;
    // Part 10 "forest fire", Part 20 "forest green"
    services.lineitems.push_back(Lineitem(kDateBase, 10, 100, 20000)); // 200
    services.lineitems.push_back(Lineitem(kDateBase, 10, 101, 30000)); // 300
    services.lineitems.push_back(Lineitem(kDateBase, 20, 200, 40000)); // 400
    services.parts.push_back(Part(10, "forest fire"));
    services.parts.push_back(Part(20, "forest green"));
    services.partsupps.push_back(Partsupp(10, 100, 200)); // > 100 → pass
    services.partsupps.push_back(Partsupp(10, 101, 300)); // > 150 → pass
    services.partsupps.push_back(Partsupp(20, 200, 300)); // > 200 → pass
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.suppliers.push_back(
        Supplier(101, "Supplier B", "Addr B", 1));
    services.suppliers.push_back(
        Supplier(200, "Supplier C", "Addr C", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 3);
    // Sorted by s_name: A, B, C
    assert(NameFromSinkRow(sink.rows[0]) == "Supplier A");
    assert(NameFromSinkRow(sink.rows[1]) == "Supplier B");
    assert(NameFromSinkRow(sink.rows[2]) == "Supplier C");
  }

  // ── Test 11: Sort order verified ────────────────────────────────────────
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase, 10, 200, 10000));
    services.lineitems.push_back(Lineitem(kDateBase, 10, 100, 10000));
    services.parts.push_back(Part(10, "forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 100));
    services.partsupps.push_back(Partsupp(10, 200, 100));
    services.suppliers.push_back(
        Supplier(200, "Zeta Corp", "Addr Z", 1));
    services.suppliers.push_back(
        Supplier(100, "Alpha Inc", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 2);
    // ORDER BY s_name: Alpha Inc < Zeta Corp
    assert(NameFromSinkRow(sink.rows[0]) == "Alpha Inc");
    assert(NameFromSinkRow(sink.rows[1]) == "Zeta Corp");
  }

  // ── Test 12: Empty lineitem set ──────────────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(10, "forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 500));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 13: LIKE pattern with wildcard prefix (not just trailing %) ────
  // SQL: p_name LIKE '%forest%' — not optimal but correct
  {
    Services services;
    services.lineitems.push_back(Lineitem(kDateBase, 10, 100, 50000));
    services.parts.push_back(Part(10, "red forest fire"));
    services.partsupps.push_back(Partsupp(10, 100, 500));
    services.suppliers.push_back(
        Supplier(100, "Supplier A", "Addr A", 1));
    services.nations.push_back(Nation(1, "CANADA"));

    TpchQ20Module module(TpchQ20Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args{
        {Date(kDateBase), String("%forest%"), String("CANADA")}};
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NameFromSinkRow(sink.rows[0]) == "Supplier A");
  }

  return 0;
}
