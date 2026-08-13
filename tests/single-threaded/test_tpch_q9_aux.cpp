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
#include "tpch/q9/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ9Config;
using accelerator_codegen::aux_state::query::TpchQ9InitialScan;
using accelerator_codegen::aux_state::query::TpchQ9Module;
using accelerator_codegen::aux_state::query::TpchQ9Layout;
using accelerator_codegen::aux_state::query::TpchQ9PartInput;
using accelerator_codegen::aux_state::query::TpchQ9SupplierInput;
using accelerator_codegen::aux_state::query::TpchQ9NationInput;
using accelerator_codegen::aux_state::query::TpchQ9OrdersInput;
using accelerator_codegen::aux_state::query::TpchQ9PartsuppInput;
using accelerator_codegen::aux_state::query::TpchQ9LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ9StateImage;
using accelerator_codegen::aux_state::query::TpchQ9ImageKind;
using accelerator_codegen::aux_state::InvalidationScope;

namespace {

void AssertShardedQ9Build(const std::vector<StateImage> &images) {
  assert(!images.empty());
  bool have_metadata = false;
  for (const auto &image : images) {
    if (image.partition != TpchQ9Layout::kCompleteState)
      continue;
    const auto *typed = image.As<TpchQ9StateImage>();
    assert(typed != nullptr);
    assert(typed->kind == TpchQ9ImageKind::Metadata ||
           typed->kind == TpchQ9ImageKind::CompleteState);
    have_metadata = true;
  }
  assert(have_metadata);
}

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

// Lineitem row: partkey, suppkey, orderkey, quantity, extendedprice, discount
AuxRow Lineitem(int64_t partkey, int64_t suppkey, int64_t orderkey,
                int64_t quantity_raw, int64_t extendedprice_raw,
                int64_t discount_raw) {
  return {{Int(partkey), Int(suppkey), Int(orderkey),
           Decimal(quantity_raw, 2), Decimal(extendedprice_raw, 2),
           Decimal(discount_raw, 2)}};
}

AuxRow Part(int64_t partkey, const std::string &name) {
  return {{Int(partkey), String(name)}};
}

AuxRow Supplier(int64_t suppkey, int64_t nationkey) {
  return {{Int(suppkey), Int(nationkey)}};
}

AuxRow Nation(int64_t nationkey, const std::string &name) {
  return {{Int(nationkey), String(name)}};
}

// orders: orderkey, orderdate (as date days)
AuxRow Order(int64_t orderkey, int32_t orderdate_days) {
  return {{Int(orderkey), Date(orderdate_days)}};
}

// partsupp: partkey, suppkey, supplycost
AuxRow Partsupp(int64_t partkey, int64_t suppkey, int64_t supplycost_raw) {
  return {{Int(partkey), Int(suppkey), Decimal(supplycost_raw, 2)}};
}

// Days since proleptic-Gregorian epoch (719468); see CivilFromDays in module.cpp.
constexpr int32_t kDate1996 = 9496;   // 1996-01-01
constexpr int32_t kDate1997 = 9862;   // 1997-01-01
constexpr int32_t kDate1998 = 10227;  // 1998-01-01

// ── Minimal Store ───────────────────────────────────────────────────────────
class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ9Layout::kAccelerator;
    snapshot.visibility = visibility;
    if (!valid) return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == TpchQ9Layout::kAccelerator && valid
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

// ── Services ─────────────────────────────────────────────────────────────────
class Services final : public AuxEngineServices, public TpchQ9InitialScan {
public:
  void ScanParts(
      const std::function<void(const TpchQ9PartInput &)> &consumer) override {
    for (const auto &row : parts)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ9SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers)
      consumer({std::get<int64_t>(row.values[0].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[1].payload))});
  }
  void ScanNations(
      const std::function<void(const TpchQ9NationInput &)> &consumer) override {
    for (const auto &row : nations)
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
  }
  void ScanOrders(
      const std::function<void(const TpchQ9OrdersInput &)> &consumer) override {
    for (const auto &row : orders)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<AuxDate>(row.values[1].payload).days_since_epoch});
  }
  void ScanPartsupps(
      const std::function<void(const TpchQ9PartsuppInput &)> &consumer) override {
    for (const auto &row : partsupps) {
      const auto &cost = std::get<ScaledDecimal>(row.values[2].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload), cost.value});
    }
  }
  void ScanLineitems(
      const std::function<void(const TpchQ9LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &qty = std::get<ScaledDecimal>(row.values[3].payload);
      const auto &price = std::get<ScaledDecimal>(row.values[4].payload);
      const auto &disc = std::get<ScaledDecimal>(row.values[5].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<int64_t>(row.values[2].payload), qty.value, price.value,
                disc.value});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> parts;
  std::vector<AuxRow> suppliers;
  std::vector<AuxRow> nations;
  std::vector<AuxRow> orders;
  std::vector<AuxRow> partsupps;
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
std::string NationFromSinkRow(const AuxRow &row) {
  assert(row.values.size() >= 3);
  assert(!row.values[0].is_null);
  return std::get<std::string>(row.values[0].payload);
}

int64_t YearFromSinkRow(const AuxRow &row) {
  assert(!row.values[1].is_null);
  return std::get<int64_t>(row.values[1].payload);
}

int64_t ProfitFromSinkRow(const AuxRow &row) {
  assert(!row.values[2].is_null);
  return std::get<ScaledDecimal>(row.values[2].payload).value;
}

} // namespace

int main() {
  // ── Test 1: Basic Build + Execute ───────────────────────────────────────
  {
    Services services;
    // Parts with various colors
    services.parts.push_back(Part(10, "red apple"));
    services.parts.push_back(Part(20, "blue sky"));
    services.parts.push_back(Part(30, "reddish pink"));

    // Suppliers
    services.suppliers.push_back(Supplier(100, 1));
    services.suppliers.push_back(Supplier(200, 2));

    // Nations
    services.nations.push_back(Nation(1, "USA"));
    services.nations.push_back(Nation(2, "CANADA"));

    // Orders (date → 1996)
    services.orders.push_back(Order(1000, kDate1996));
    services.orders.push_back(Order(2000, kDate1997));

    // Partsupp
    services.partsupps.push_back(Partsupp(10, 100, 500));  // supplycost 5.00
    services.partsupps.push_back(Partsupp(10, 200, 300));  // supplycost 3.00
    services.partsupps.push_back(Partsupp(20, 100, 200));  // supplycost 2.00
    services.partsupps.push_back(Partsupp(30, 200, 400));  // supplycost 4.00

    // Lineitems
    // part 10, supp 100, order 1000: ext=2000, disc=5, qty=50
    // profit = 2000*(100-5) - 500*50 = 190000 - 25000 = 165000
    services.lineitems.push_back(Lineitem(10, 100, 1000, 5000, 200000, 5));

    // part 10, supp 200, order 2000: ext=1000, disc=3, qty=20
    // profit = 1000*(100-3) - 300*20 = 97000 - 6000 = 91000
    services.lineitems.push_back(Lineitem(10, 200, 2000, 2000, 100000, 3));

    // part 20, supp 100, order 1000: ext=800, disc=2, qty=10
    // profit = 800*(100-2) - 200*10 = 78400 - 2000 = 76400
    services.lineitems.push_back(Lineitem(20, 100, 1000, 1000, 80000, 2));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    // Query with "red" — should match part 10 and 30
    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() >= 1);

    // Check: part 10 contributes to USA (nation 1) in 1996, CANADA (nation 2) in 1997
    // Find USA row
    bool found_usa = false, found_canada = false;
    for (const auto &row : sink.rows) {
      if (NationFromSinkRow(row) == "USA") {
        found_usa = true;
        assert(YearFromSinkRow(row) == 1996);
      }
      if (NationFromSinkRow(row) == "CANADA") {
        found_canada = true;
        assert(YearFromSinkRow(row) == 1997);
      }
    }
    assert(found_usa);
    assert(found_canada);
  }

  // ── Test 2: Sort order (nation ASC, year DESC) ──────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red"));

    services.suppliers.push_back(Supplier(1, 3)); // nation 3
    services.suppliers.push_back(Supplier(2, 1)); // nation 1

    services.nations.push_back(Nation(1, "ARGENTINA"));
    services.nations.push_back(Nation(3, "BRAZIL"));

    // 1997 order for nation 3
    services.orders.push_back(Order(100, kDate1997));
    // 1998 order for nation 1
    services.orders.push_back(Order(200, kDate1998));

    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.partsupps.push_back(Partsupp(1, 2, 100));

    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 50000, 0));
    services.lineitems.push_back(Lineitem(1, 2, 200, 1000, 50000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 2);
    // ARGENTINA (nation 1) ASC before BRAZIL (nation 3)
    assert(NationFromSinkRow(sink.rows[0]) == "ARGENTINA");
    assert(NationFromSinkRow(sink.rows[1]) == "BRAZIL");

    // Within same nation, year DESC: 1997 before 1996 (only one per nation here)
    // Add another year for ARGENTINA
    services.orders.push_back(Order(300, kDate1996));
    services.suppliers.push_back(Supplier(3, 1));
    services.partsupps.push_back(Partsupp(1, 3, 100));
    services.lineitems.push_back(Lineitem(1, 3, 300, 1000, 50000, 0));

    // Rebuild
    Builder builder2;
    module.Build(services, builder2);
    assert(builder2.finished);
    AssertShardedQ9Build(builder2.images);
    services.store.InstallBuild(std::move(builder2.images));

    Sink sink2;
    module.Execute(services, QueryArguments{{String("red")}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 3);
    assert(NationFromSinkRow(sink2.rows[0]) == "ARGENTINA");
    assert(YearFromSinkRow(sink2.rows[0]) == 1998);
    assert(NationFromSinkRow(sink2.rows[1]) == "ARGENTINA");
    assert(YearFromSinkRow(sink2.rows[1]) == 1996);
    assert(NationFromSinkRow(sink2.rows[2]) == "BRAZIL");
  }

  // ── Test 3: Exact substring match vs partial ────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "reddish"));
    services.parts.push_back(Part(2, "red carpet"));
    services.parts.push_back(Part(3, "bored"));   // contains "red" but...

    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.partsupps.push_back(Partsupp(2, 1, 100));
    services.partsupps.push_back(Partsupp(3, 1, 100));
    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 50000, 0));
    services.lineitems.push_back(Lineitem(2, 1, 100, 1000, 50000, 0));
    services.lineitems.push_back(Lineitem(3, 1, 100, 1000, 50000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    // "red" should match "reddish", "red carpet", and "bored"
    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    // All three parts contribute through same nation/year → one row
    assert(NationFromSinkRow(sink.rows[0]) == "USA");
    assert(YearFromSinkRow(sink.rows[0]) == 1996);
  }

  // ── Test 4: Fallback when state missing ─────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red"));
    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 50000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    // No build — triggers fallback path

    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NationFromSinkRow(sink.rows[0]) == "USA");
  }

  // ── Test 5: Empty result for non-matching color ─────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red"));
    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 50000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("blue")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 6: Lineitem INSERT mutation ─────────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red"));
    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 10000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    // Check initial profit
    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    int64_t initial_profit = ProfitFromSinkRow(sink.rows[0]);

    // Insert another lineitem
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ9Module>(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services));
    coordinator.AttachServices(services);

    MutationBatch insert;
    insert.table = 6; // lineitem
    insert.kind = MutationKind::Insert;
    insert.new_rows = {Lineitem(1, 1, 100, 500, 5000, 0)};
    services.lineitems.push_back(insert.new_rows.front());
    coordinator.OnMutation(insert);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink2;
    module.Execute(services, QueryArguments{{String("red")}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 1);
    // New profit should be higher
    assert(ProfitFromSinkRow(sink2.rows[0]) > initial_profit);
  }

  // ── Test 7: Lineitem DELETE mutation ─────────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red"));
    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 20000, 0));
    services.lineitems.push_back(Lineitem(1, 1, 100, 500, 10000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    int64_t initial_profit = ProfitFromSinkRow(sink.rows[0]);

    // Delete the first lineitem
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ9Module>(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services));
    coordinator.AttachServices(services);

    MutationBatch del;
    del.table = 6;
    del.kind = MutationKind::Delete;
    del.old_rows = {Lineitem(1, 1, 100, 1000, 20000, 0)};
    services.lineitems.erase(services.lineitems.begin());
    coordinator.OnMutation(del);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink2;
    module.Execute(services, QueryArguments{{String("red")}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 1);
    assert(ProfitFromSinkRow(sink2.rows[0]) < initial_profit);
  }

  // ── Test 8: Part name update ────────────────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red apple"));
    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 50000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    // "red" matches
    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);

    // Change name to "blue sky"
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ9Module>(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services));
    coordinator.AttachServices(services);

    MutationBatch update;
    update.table = 1; // part
    update.kind = MutationKind::Update;
    update.old_rows = {Part(1, "red apple")};
    update.new_rows = {Part(1, "blue sky")};
    services.parts[0] = update.new_rows.front();
    coordinator.OnMutation(update);
    coordinator.PreCommit();
    coordinator.Commit();

    // "red" no longer matches
    Sink sink2;
    module.Execute(services, QueryArguments{{String("red")}}, sink2);
    assert(sink2.ended);
    assert(sink2.rows.size() == 0);

    // "blue" now matches
    Sink sink3;
    module.Execute(services, QueryArguments{{String("blue")}}, sink3);
    assert(sink3.ended);
    assert(sink3.rows.size() == 1);
  }

  // ── Test 9: Empty lineitems set ─────────────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red"));
    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 0);
  }

  // ── Test 10: Part INSERT posting ────────────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "red one"));
    services.suppliers.push_back(Supplier(1, 1));
    services.nations.push_back(Nation(1, "USA"));
    services.orders.push_back(Order(100, kDate1996));
    services.partsupps.push_back(Partsupp(1, 1, 100));
    services.lineitems.push_back(Lineitem(1, 1, 100, 1000, 50000, 0));

    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    AssertShardedQ9Build(builder.images);
    services.store.InstallBuild(std::move(builder.images));

    // Insert a new part "red two" plus its partsupp and lineitem.
    AuxTransactionCoordinator coordinator;
    coordinator.Register(
        std::make_shared<TpchQ9Module>(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services));
    coordinator.AttachServices(services);

    MutationBatch insert_part;
    insert_part.table = 1;
    insert_part.kind = MutationKind::Insert;
    insert_part.new_rows = {Part(2, "red two")};
    services.parts.push_back(insert_part.new_rows.front());
    coordinator.OnMutation(insert_part);

    services.partsupps.push_back(Partsupp(2, 1, 100));
    MutationBatch insert_partsupp;
    insert_partsupp.table = 5;
    insert_partsupp.kind = MutationKind::Insert;
    insert_partsupp.new_rows = {services.partsupps.back()};
    coordinator.OnMutation(insert_partsupp);

    services.lineitems.push_back(Lineitem(2, 1, 100, 1000, 50000, 0));
    MutationBatch insert_lineitem;
    insert_lineitem.table = 6;
    insert_lineitem.kind = MutationKind::Insert;
    insert_lineitem.new_rows = {services.lineitems.back()};
    coordinator.OnMutation(insert_lineitem);

    coordinator.PreCommit();
    coordinator.Commit();

    // Now "red" should find both parts (aggregated to one nation/year row).
    Sink sink;
    module.Execute(services, QueryArguments{{String("red")}}, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(NationFromSinkRow(sink.rows[0]) == "USA");
  }

  // ── Test 11: InterpretMutation coverage ──────────────────────────────────
  {
    Services services;
    TpchQ9Module module(TpchQ9Config{1, 2, 3, 4, 5, 6}, &services);

    // lineitem mutation → PartitionSet
    MutationBatch li_mut;
    li_mut.table = 6;
    li_mut.kind = MutationKind::Insert;
    auto plan = module.InterpretMutation(li_mut);
    assert(plan.scope == InvalidationScope::PartitionSet);

    // part mutation → PartitionSet
    MutationBatch p_mut;
    p_mut.table = 1;
    p_mut.kind = MutationKind::Update;
    plan = module.InterpretMutation(p_mut);
    assert(plan.scope == InvalidationScope::PartitionSet);

    // supplier mutation → PartitionSet
    MutationBatch s_mut;
    s_mut.table = 2;
    s_mut.kind = MutationKind::Insert;
    plan = module.InterpretMutation(s_mut);
    assert(plan.scope == InvalidationScope::PartitionSet);

    // Verify single-partition layout
    auto partitions = TpchQ9Layout::Partitions();
    assert(partitions.size() == 1);
    assert(partitions[0] == TpchQ9Layout::kCompleteState);

    // Unknown table → empty plan
    MutationBatch unknown;
    unknown.table = 999;
    unknown.kind = MutationKind::Insert;
    plan = module.InterpretMutation(unknown);
    assert(plan.scope == InvalidationScope::None);
  }

  return 0;
}
