#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/state/invalidation_plan.hpp"
#include "tpch/q7/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ7Config;
using accelerator_codegen::aux_state::query::TpchQ7CustomerInput;
using accelerator_codegen::aux_state::query::TpchQ7InitialScan;
using accelerator_codegen::aux_state::query::TpchQ7LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ7Module;
using accelerator_codegen::aux_state::query::TpchQ7NationInput;
using accelerator_codegen::aux_state::query::TpchQ7OrderInput;
using accelerator_codegen::aux_state::query::TpchQ7SupplierInput;
using accelerator_codegen::aux_state::core::TpchQ7Layout;
using accelerator_codegen::aux_state::query::TpchQ7DaysFromYmd;
using accelerator_codegen::aux_state::InvalidationScope;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

int32_t Ymd(int year, unsigned month, unsigned day) {
  return TpchQ7DaysFromYmd(year, month, day);
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ7Layout::kAccelerator;
    snapshot.visibility = visibility;
    if (!valid) return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }
  StateSnapshot ReadPartitions(const VisibilityToken &visibility,
                               const std::vector<PartitionId> &partitions) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ7Layout::kAccelerator;
    snapshot.visibility = visibility;
    if (!valid) return snapshot;
    for (const auto &[partition, image] : images) {
      if (std::find(partitions.begin(), partitions.end(), partition) !=
          partitions.end())
        snapshot.partitions.push_back(image);
    }
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == TpchQ7Layout::kAccelerator && valid
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

class Services final : public AuxEngineServices, public TpchQ7InitialScan {
public:
  void ScanNations(
      const std::function<void(const TpchQ7NationInput &)> &consumer) override {
    for (const auto &n : nations) consumer(n);
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ7SupplierInput &)> &consumer) override {
    for (const auto &s : suppliers) consumer(s);
  }
  void ScanCustomers(
      const std::function<void(const TpchQ7CustomerInput &)> &consumer) override {
    for (const auto &c : customers) consumer(c);
  }
  void ScanOrders(
      const std::function<void(const TpchQ7OrderInput &)> &consumer) override {
    for (const auto &o : orders) consumer(o);
  }
  void ScanLineitems(
      const std::function<void(const TpchQ7LineitemInput &)> &consumer) override {
    for (const auto &l : lineitems) consumer(l);
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<TpchQ7NationInput> nations;
  std::vector<TpchQ7SupplierInput> suppliers;
  std::vector<TpchQ7CustomerInput> customers;
  std::vector<TpchQ7OrderInput> orders;
  std::vector<TpchQ7LineitemInput> lineitems;
};

class Builder final : public StateBuilder {
public:
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

class Sink final : public AuxRowSink {
public:
  void Begin(const OutputSchema &schema) override {
    assert(schema.columns.size() == 4);
    assert(schema.columns[0].name == "supp_nation");
    assert(schema.columns[1].name == "cust_nation");
    assert(schema.columns[2].name == "l_year");
    assert(schema.columns[3].name == "revenue");
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

double SinkRowRevenue(const AuxRow &row) {
  return std::get<double>(row.values[3].payload);
}

}  // namespace

int main() {
  // ── Test 1: Build + Execute with matching nations ─────────────────────────
  {
    Services services;
    services.nations.push_back({10, "BRAZIL"});
    services.nations.push_back({11, "UNITED KINGDOM"});
    services.customers.push_back({100, 10});
    services.customers.push_back({101, 11});
    services.suppliers.push_back({200, 10});
    services.suppliers.push_back({201, 11});
    services.orders.push_back({1000, 100});
    services.orders.push_back({1001, 101});

    // Lines: supp 200 (nk=10) → order 1000 → cust=100 (nk=10) same-nation
    //        supp 201 (nk=11) → order 1001 → cust=101 (nk=11) same-nation
    //        supp 200 (nk=10) → order 1001 → cust=101 (nk=11) crossing A
    //        supp 201 (nk=11) → order 1000 → cust=100 (nk=10) crossing B
    int32_t d_1995 = Ymd(1995, 6, 1);
    int32_t d_1996 = Ymd(1996, 6, 1);

    // Same-nation: should NOT appear (TpchQ7 convention: only cross-nation pairs are
    // given as arguments, but same-nation rows don't match the nation1/nation2 filter)
    services.lineitems.push_back({1000, 200, 10000, 2, 0, 2, d_1995});  // BRA→BRA, ext=100.00 disc=0%
    services.lineitems.push_back({1001, 201, 20000, 2, 0, 2, d_1996});  // UK→UK, ext=200.00 disc=0%

    // Direction A: BRAZIL(supp) → UK(cust): supp 200 (BRA) → order 1001 → cust 101 (UK)
    services.lineitems.push_back({1001, 200, 10000, 2, 10, 2, d_1995});  // ext=100.00, disc=0.10, rev=90.00
    services.lineitems.push_back({1001, 200, 20000, 2, 10, 2, d_1996});  // ext=200.00, disc=0.10, rev=180.00

    // Direction B: UK(supp) → BRA(cust): supp 201 (UK) → order 1000 → cust 100 (BRA)
    services.lineitems.push_back({1000, 201, 5000, 2, 5, 2, d_1995});    // ext=50.00, disc=0.05, rev=47.50
    services.lineitems.push_back({1000, 201, 30000, 2, 0, 2, d_1996});   // ext=300.00, disc=0.00, rev=300.00

    // Out of window: excluded
    services.lineitems.push_back({1000, 201, 10000, 2, 0, 2, Ymd(1994, 12, 31)});

    TpchQ7Module module(TpchQ7Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Query with direction A
    {
      Sink sink;
      module.Execute(services, QueryArguments{{String("BRAZIL"), String("UNITED KINGDOM")}}, sink);
      assert(sink.ended);
      // Should have: BRA→UK 1995 (90.00), BRA→UK 1996 (180.00)
      //              UK→BRA 1995 (47.50), UK→BRA 1996 (300.00)
      assert(sink.rows.size() == 4);

      // Sorted by supp_nation, cust_nation, l_year
      assert(std::get<std::string>(sink.rows[0].values[0].payload) == "BRAZIL");
      assert(std::get<std::string>(sink.rows[0].values[1].payload) == "UNITED KINGDOM");
      assert(std::get<int64_t>(sink.rows[0].values[2].payload) == 1995);
      assert(std::abs(SinkRowRevenue(sink.rows[0]) - 90.00) < 0.01);

      assert(std::get<std::string>(sink.rows[1].values[0].payload) == "BRAZIL");
      assert(std::get<std::string>(sink.rows[1].values[1].payload) == "UNITED KINGDOM");
      assert(std::get<int64_t>(sink.rows[1].values[2].payload) == 1996);
      assert(std::abs(SinkRowRevenue(sink.rows[1]) - 180.00) < 0.01);

      assert(std::get<std::string>(sink.rows[2].values[0].payload) == "UNITED KINGDOM");
      assert(std::get<std::string>(sink.rows[2].values[1].payload) == "BRAZIL");
      assert(std::get<int64_t>(sink.rows[2].values[2].payload) == 1995);
      assert(std::abs(SinkRowRevenue(sink.rows[2]) - 47.50) < 0.01);

      assert(std::get<std::string>(sink.rows[3].values[0].payload) == "UNITED KINGDOM");
      assert(std::get<std::string>(sink.rows[3].values[1].payload) == "BRAZIL");
      assert(std::get<int64_t>(sink.rows[3].values[2].payload) == 1996);
      assert(std::abs(SinkRowRevenue(sink.rows[3]) - 300.00) < 0.01);
    }

    // Query with reverse args: same results but swapped roles
    {
      Sink sink;
      module.Execute(services, QueryArguments{{String("UNITED KINGDOM"), String("BRAZIL")}}, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 4);
      // Same sorted order
      assert(std::get<std::string>(sink.rows[0].values[0].payload) == "BRAZIL");
      assert(std::get<std::string>(sink.rows[0].values[1].payload) == "UNITED KINGDOM");
    }

    // Fallback should match
    {
      Sink sink, fallback;
      module.Execute(services, QueryArguments{{String("BRAZIL"), String("UNITED KINGDOM")}}, sink);
      module.ExecuteFallback(services, QueryArguments{{String("BRAZIL"), String("UNITED KINGDOM")}}, fallback);
      assert(sink.rows.size() == fallback.rows.size());
      for (size_t i = 0; i < sink.rows.size(); i++) {
        assert(std::get<std::string>(sink.rows[i].values[0].payload) ==
               std::get<std::string>(fallback.rows[i].values[0].payload));
        assert(std::get<std::string>(sink.rows[i].values[1].payload) ==
               std::get<std::string>(fallback.rows[i].values[1].payload));
        assert(std::get<int64_t>(sink.rows[i].values[2].payload) ==
               std::get<int64_t>(fallback.rows[i].values[2].payload));
        assert(std::abs(SinkRowRevenue(sink.rows[i]) - SinkRowRevenue(fallback.rows[i])) < 0.01);
      }
    }
  }

  // ── Test 2: Empty result when no matching nations ─────────────────────────
  {
    Services services;
    services.nations.push_back({10, "BRAZIL"});
    services.nations.push_back({11, "UNITED KINGDOM"});
    services.customers.push_back({100, 10});
    services.suppliers.push_back({200, 10});
    services.orders.push_back({1000, 100});
    int32_t d = Ymd(1995, 6, 1);
    services.lineitems.push_back({1000, 200, 10000, 2, 0, 2, d});

    TpchQ7Module module(TpchQ7Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Query with non-existent nation: should return empty
    Sink sink;
    module.Execute(services, QueryArguments{{String("FRANCE"), String("GERMANY")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 3: Fallback when state not built ─────────────────────────────────
  {
    Services services;
    services.nations.push_back({10, "BRAZIL"});
    services.nations.push_back({11, "UNITED KINGDOM"});
    services.customers.push_back({100, 10});
    services.suppliers.push_back({200, 11});
    services.orders.push_back({1000, 100});
    int32_t d = Ymd(1995, 6, 1);
    services.lineitems.push_back({1000, 200, 10000, 2, 10, 2, d});

    TpchQ7Module module(TpchQ7Config{1, 2, 3, 4, 5}, &services);
    // No build — should fallback and still work
    Sink sink;
    module.Execute(services, QueryArguments{{String("BRAZIL"), String("UNITED KINGDOM")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1); // UK→BRA 1995
  }

  // ── Test 4: Date filtering ────────────────────────────────────────────────
  {
    Services services;
    services.nations.push_back({10, "BRAZIL"});
    services.nations.push_back({11, "UNITED KINGDOM"});
    services.customers.push_back({100, 10});
    services.customers.push_back({101, 11});
    services.suppliers.push_back({200, 10});
    services.suppliers.push_back({201, 11});
    services.orders.push_back({1000, 100});
    services.orders.push_back({1001, 101});

    // Lineitem outside 1995-1996 window: should be excluded
    services.lineitems.push_back({1001, 200, 10000, 2, 0, 2, Ymd(1994, 12, 31)});
    services.lineitems.push_back({1001, 200, 10000, 2, 0, 2, Ymd(1997, 1, 1)});
    // One inside window
    services.lineitems.push_back({1001, 200, 30000, 2, 0, 2, Ymd(1995, 7, 1)});

    TpchQ7Module module(TpchQ7Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("BRAZIL"), String("UNITED KINGDOM")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(std::get<int64_t>(sink.rows[0].values[2].payload) == 1995);
  }

  // ── Test 5: Lineitem mutation (Insert → ApplyDelta) ──────────────────────
  {
    Services services;
    services.nations.push_back({10, "BRAZIL"});
    services.nations.push_back({11, "UNITED KINGDOM"});
    services.customers.push_back({100, 10});
    services.customers.push_back({101, 11});
    services.suppliers.push_back({200, 10});
    services.suppliers.push_back({201, 11});
    services.orders.push_back({1000, 100});
    services.orders.push_back({1001, 101});

    TpchQ7Module module(TpchQ7Config{1, 2, 3, 4, 5}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Initially empty
    {
      Sink sink;
      module.Execute(services, QueryArguments{{String("BRAZIL"), String("UNITED KINGDOM")}}, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 0);
    }

    // Insert new lineitem via coordinator
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ7Module>(TpchQ7Config{1, 2, 3, 4, 5}, &services));
    coordinator.AttachServices(services);

    // Build rows for mutation
    AuxRow li_row;
    li_row.values.push_back(Int(1001));                          // l_orderkey
    li_row.values.push_back(Int(200));                           // l_suppkey
    li_row.values.push_back(
        AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{30000, 2}));  // l_extendedprice
    li_row.values.push_back(
        AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{10, 2}));     // l_discount
    li_row.values.push_back(
        AuxValue::Of(AuxType::Date, AuxDate{Ymd(1995, 8, 1)}));  // l_shipdate

    MutationBatch ins;
    ins.table = 5;  // lineitem
    ins.kind = MutationKind::Insert;
    ins.new_rows = {li_row};
    coordinator.OnMutation(ins);
    coordinator.PreCommit();
    coordinator.Commit();

    // Now should have 1 row
    {
      Sink sink;
      module.Execute(services, QueryArguments{{String("BRAZIL"), String("UNITED KINGDOM")}}, sink);
      assert(sink.ended);
      assert(sink.rows.size() == 1);
      assert(std::get<int64_t>(sink.rows[0].values[2].payload) == 1995);
      assert(std::abs(SinkRowRevenue(sink.rows[0]) - 270.00) < 0.01);
    }
  }

  return 0;
}
