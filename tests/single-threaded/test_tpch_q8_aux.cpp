#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/state/invalidation_plan.hpp"
#include "tpch/q8/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ8Config;
using accelerator_codegen::aux_state::query::TpchQ8CustomerInput;
using accelerator_codegen::aux_state::query::TpchQ8InitialScan;
using accelerator_codegen::aux_state::query::TpchQ8LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ8Module;
using accelerator_codegen::aux_state::query::TpchQ8NationInput;
using accelerator_codegen::aux_state::query::TpchQ8OrdersInput;
using accelerator_codegen::aux_state::query::TpchQ8PartInput;
using accelerator_codegen::aux_state::query::TpchQ8RegionInput;
using accelerator_codegen::aux_state::query::TpchQ8SupplierInput;
using accelerator_codegen::aux_state::query::TpchQ8Layout;
using accelerator_codegen::aux_state::InvalidationScope;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Int32(int32_t value) { return AuxValue::Of(AuxType::Int64, static_cast<int64_t>(value)); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}

// 1995-06-15: days since epoch ~9131+165=9296, let's use 9300 for simplicity
constexpr int32_t kYear1995Day = 9300;   // ~1995-06-15
constexpr int32_t kYear1996Day = 9665;   // ~1996-06-15

AuxRow Part(int64_t key, const std::string &type) {
  return {{Int(key), String(type)}};
}
AuxRow Supplier(int64_t key, int32_t nationkey) {
  return {{Int(key), Int32(nationkey)}};
}
AuxRow Customer(int64_t key, int32_t nationkey) {
  return {{Int(key), Int32(nationkey)}};
}
AuxRow Orders(int64_t key, int64_t custkey, int32_t orderdate_days) {
  return {{Int(key), Int(custkey), Date(orderdate_days)}};
}
AuxRow Lineitem(int64_t partkey, int64_t orderkey, int64_t suppkey,
                int64_t price_raw, int64_t discount_raw) {
  // lineitem: partkey, orderkey, suppkey, extendedprice, discount
  return {{Int(partkey), Int(orderkey), Int(suppkey),
           Decimal(price_raw, 2), Decimal(discount_raw, 2)}};
}
AuxRow Nation(int32_t key, const std::string &name, int32_t regionkey) {
  return {{Int32(key), String(name), Int32(regionkey)}};
}
AuxRow Region(int32_t key, const std::string &name) {
  return {{Int32(key), String(name)}};
}

// ── Minimal Store ───────────────────────────────────────────────────────────
class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ8Layout::kAccelerator;
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
    return key.accelerator == TpchQ8Layout::kAccelerator && valid
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

// ── Services (implements both AuxEngineServices and TpchQ8InitialScan) ─────
class Services final : public AuxEngineServices, public TpchQ8InitialScan {
public:
  void ScanParts(
      const std::function<void(const TpchQ8PartInput &)> &consumer) override {
    for (const auto &row : parts) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ8SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[1].payload))});
    }
  }
  void ScanCustomers(
      const std::function<void(const TpchQ8CustomerInput &)> &consumer) override {
    for (const auto &row : customers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[1].payload))});
    }
  }
  void ScanOrders(
      const std::function<void(const TpchQ8OrdersInput &)> &consumer) override {
    for (const auto &row : orders) {
      const auto &date = std::get<AuxDate>(row.values[2].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                date.days_since_epoch});
    }
  }
  void ScanLineitems(
      const std::function<void(const TpchQ8LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &price = std::get<ScaledDecimal>(row.values[3].payload);
      const auto &discount = std::get<ScaledDecimal>(row.values[4].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<int64_t>(row.values[2].payload),
                price.value, discount.value});
    }
  }
  void ScanNations(
      const std::function<void(const TpchQ8NationInput &)> &consumer) override {
    for (const auto &row : nations) {
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload))});
    }
  }
  void ScanRegions(
      const std::function<void(const TpchQ8RegionInput &)> &consumer) override {
    for (const auto &row : regions) {
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  bool LookupOrder(int64_t orderkey, TpchQ8OrdersInput &out) override {
    for (const auto &row : orders) {
      if (std::get<int64_t>(row.values[0].payload) == orderkey) {
        out.key = orderkey;
        out.custkey = std::get<int64_t>(row.values[1].payload);
        out.orderdate_days = std::get<AuxDate>(row.values[2].payload).days_since_epoch;
        return true;
      }
    }
    return false;
  }
  bool LookupPart(int64_t partkey, TpchQ8PartInput &out) override {
    for (const auto &row : parts) {
      if (std::get<int64_t>(row.values[0].payload) == partkey) {
        out.key = partkey;
        out.type = std::get<std::string>(row.values[1].payload);
        return true;
      }
    }
    return false;
  }
  bool LookupSupplier(int64_t suppkey, TpchQ8SupplierInput &out) override {
    for (const auto &row : suppliers) {
      if (std::get<int64_t>(row.values[0].payload) == suppkey) {
        out.key = suppkey;
        out.nationkey = static_cast<int32_t>(std::get<int64_t>(row.values[1].payload));
        return true;
      }
    }
    return false;
  }
  bool LookupCustomer(int64_t custkey, TpchQ8CustomerInput &out) override {
    for (const auto &row : customers) {
      if (std::get<int64_t>(row.values[0].payload) == custkey) {
        out.key = custkey;
        out.nationkey = static_cast<int32_t>(std::get<int64_t>(row.values[1].payload));
        return true;
      }
    }
    return false;
  }

  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> parts;
  std::vector<AuxRow> suppliers;
  std::vector<AuxRow> customers;
  std::vector<AuxRow> orders;
  std::vector<AuxRow> lineitems;
  std::vector<AuxRow> nations;
  std::vector<AuxRow> regions;
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

double SinkYearShare(const std::vector<AuxRow> &rows, int64_t year) {
  for (const auto &row : rows) {
    if (std::get<int64_t>(row.values[0].payload) == year) {
      return std::get<double>(row.values[1].payload);
    }
  }
  return -1.0;
}

} // namespace

int main() {
  // ── Test 1: Happy path — single part, supplier-nation = target nation ─────
  // Q8: p_type = 'BRASS', nation_name = 'INDIA', region_name = 'ASIA'
  // SQL predicate: p_type = 'BRASS', r_name = 'ASIA' (customer nation in ASIA)
  // SUM(CASE WHEN n2.n_name = 'INDIA' THEN volume ELSE 0 END)
  // volume = l_extendedprice * (1 - l_discount)
  {
    Services services;
    // Dimension data
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.nations.push_back(Nation(2, "CHINA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    // Part: BRASS type
    services.parts.push_back(Part(1, "BRASS"));
    services.parts.push_back(Part(2, "STEEL"));
    // Supplier in INDIA (nationkey 1)
    services.suppliers.push_back(Supplier(1, 1));
    services.suppliers.push_back(Supplier(2, 2));
    // Customer in CHINA (nationkey 2, ASIA region)
    services.customers.push_back(Customer(1, 2));
    // Order in 1995
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    // Lineitem: part=BRASS(1), supp=INDIA-supplier(1), order in 1995
    // extendedprice=10000 (scale 2) → 100.00, discount=5 (scale 2) → 0.05
    // volume = 10000 * (100-5) = 950000 (scale 4 → 95.0000)
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    assert(builder.images.size() == 2);
    services.store.InstallBuild(std::move(builder.images));

    // Query: nation='INDIA', region='ASIA', type='BRASS'
    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);  // only 1995 has data
    auto share95 = SinkYearShare(sink.rows, 1995);
    assert(std::fabs(share95 - 1.0) < 1e-9);
  }

  // ── Test 2: Partial nation share — one INDIA supplier, one CHINA supplier ─
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.nations.push_back(Nation(2, "CHINA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "BRASS"));
    services.suppliers.push_back(Supplier(1, 1));
    services.suppliers.push_back(Supplier(2, 2));
    services.customers.push_back(Customer(1, 2));
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    // Two lineitems: one from INDIA supplier (1), one from CHINA supplier (2)
    // Same volume: 10000 * (100-5) = 950000 each
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));
    services.lineitems.push_back(Lineitem(1, 1, 2, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    auto share95 = SinkYearShare(sink.rows, 1995);
    // INDIA volume: 950000, total: 1900000 → 0.5
    assert(std::fabs(share95 - 0.5) < 1e-9);
  }

  // ── Test 3: Wrong region exclusion — customer not in ASIA, so no rows ──
  // p_type = 'BRASS', nation = 'FRANCE', region = 'EUROPE'
  // Customer is in ASIA → excluded, so cube query gives 0 results
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.nations.push_back(Nation(2, "FRANCE", 2));
    services.regions.push_back(Region(1, "ASIA"));
    services.regions.push_back(Region(2, "EUROPE"));
    services.parts.push_back(Part(1, "BRASS"));
    services.suppliers.push_back(Supplier(1, 1));
    services.customers.push_back(Customer(1, 1)); // ASIA customer
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Query says region='EUROPE' but customer is in ASIA → no rows match region filter
    Sink sink;
    module.Execute(services, QueryArguments{{String("FRANCE"), String("EUROPE"), String("BRASS")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);  // no qualifying years
  }

  // ── Test 4: Wrong part type — STEEL, but query asks for BRASS ─────────
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "STEEL"));
    services.suppliers.push_back(Supplier(1, 1));
    services.customers.push_back(Customer(1, 1));
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    // No matching part type → no output rows (GROUP BY skips empty years)
    assert(sink.rows.size() == 0);
  }

  // ── Test 5: Fallback when state missing ─────────────────────────────────
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "BRASS"));
    services.suppliers.push_back(Supplier(1, 1));
    services.customers.push_back(Customer(1, 1));
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    // Don't build — state missing. Execute should fallback to building from
    // scan and computing directly.

    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);  // only 1995 has data
    auto share95 = SinkYearShare(sink.rows, 1995);
    assert(std::fabs(share95 - 1.0) < 1e-9);
  }

  // ── Test 6: Mutation (lineitem insert) updates cube ────────────────────
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.nations.push_back(Nation(2, "CHINA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "BRASS"));
    services.suppliers.push_back(Supplier(1, 1));
    services.suppliers.push_back(Supplier(2, 2));
    services.customers.push_back(Customer(1, 2));
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ8Module>(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services));
    coordinator.AttachServices(services);

    // Insert a second lineitem from CHINA supplier
    MutationBatch ins;
    ins.table = 5; // lineitem
    ins.kind = MutationKind::Insert;
    ins.new_rows = {Lineitem(1, 1, 2, 10000, 5)};
    services.lineitems.push_back(ins.new_rows.front());
    coordinator.OnMutation(ins);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    auto share95 = SinkYearShare(sink.rows, 1995);
    // INDIA volume: 950000, total: 1900000 → 0.5
    assert(std::fabs(share95 - 0.5) < 1e-9);
  }

  // ── Test 7: Both years have data ─────────────────────────────────────
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "BRASS"));
    services.suppliers.push_back(Supplier(1, 1));
    services.customers.push_back(Customer(1, 1));
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    services.orders.push_back(Orders(2, 1, kYear1996Day));
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));
    services.lineitems.push_back(Lineitem(1, 2, 1, 20000, 10));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 2);
    auto share95 = SinkYearShare(sink.rows, 1995);
    auto share96 = SinkYearShare(sink.rows, 1996);
    assert(std::fabs(share95 - 1.0) < 1e-9);
    assert(std::fabs(share96 - 1.0) < 1e-9);
  }

  // ── Test 8: Mutation on part (dimension) triggers rebuild ──────────────
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "BRASS"));
    services.suppliers.push_back(Supplier(1, 1));
    services.customers.push_back(Customer(1, 1));
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ8Module>(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services));
    coordinator.AttachServices(services);

    // Part mutation → rebuild
    MutationBatch part_update;
    part_update.table = 1;
    part_update.kind = MutationKind::Update;
    part_update.old_rows = {Part(1, "BRASS")};
    part_update.new_rows = {Part(1, "NICKEL")};
    services.parts.clear();
    services.parts.push_back(Part(1, "NICKEL"));
    coordinator.OnMutation(part_update);
    coordinator.PreCommit();
    coordinator.Commit();

    // After rebuild, query for BRASS should find nothing (part type changed to NICKEL)
    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 9: Empty dataset ───────────────────────────────────────────────
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "BRASS"));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);  // no qualifying data in either year
  }

  // ── Test 10: Irrelevant mutation → NoOp ────────────────────────────────
  {
    Services services;
    services.nations.push_back(Nation(1, "INDIA", 1));
    services.regions.push_back(Region(1, "ASIA"));
    services.parts.push_back(Part(1, "BRASS"));
    services.suppliers.push_back(Supplier(1, 1));
    services.customers.push_back(Customer(1, 1));
    services.orders.push_back(Orders(1, 1, kYear1995Day));
    services.lineitems.push_back(Lineitem(1, 1, 1, 10000, 5));

    TpchQ8Module module(TpchQ8Config{1, 2, 3, 4, 5, 6, 7}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    // Irrelevant mutation on a table not watched (table 99)
    MutationBatch irrelevant;
    irrelevant.table = 99;
    irrelevant.kind = MutationKind::Insert;
    const auto plan = module.InterpretMutation(irrelevant);
    assert(plan.IsEmpty());

    // state should still be valid
    Sink sink;
    module.Execute(services, QueryArguments{{String("INDIA"), String("ASIA"), String("BRASS")}}, sink);
    assert(sink.ended);
    auto share95 = SinkYearShare(sink.rows, 1995);
    assert(std::fabs(share95 - 1.0) < 1e-9);
  }

  return 0;
}
