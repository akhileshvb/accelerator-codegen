#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "tpch/q5/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ5Config;
using accelerator_codegen::aux_state::query::TpchQ5CustomerInput;
using accelerator_codegen::aux_state::query::TpchQ5InitialScan;
using accelerator_codegen::aux_state::query::TpchQ5LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ5Module;
using accelerator_codegen::aux_state::query::TpchQ5NationInput;
using accelerator_codegen::aux_state::query::TpchQ5OrderInput;
using accelerator_codegen::aux_state::query::TpchQ5RegionInput;
using accelerator_codegen::aux_state::query::TpchQ5SupplierInput;

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

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q5";
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
    return key.accelerator == "tpch_q5" && valid
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

class Services final : public AuxEngineServices, public TpchQ5InitialScan {
public:
  void ScanRegions(const std::function<void(const TpchQ5RegionInput &)> &consumer) override {
    for (const auto &row : regions)
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
  }
  void ScanNations(const std::function<void(const TpchQ5NationInput &)> &consumer) override {
    for (const auto &row : nations)
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload))});
  }
  void ScanCustomers(const std::function<void(const TpchQ5CustomerInput &)> &consumer) override {
    for (const auto &row : customers)
      consumer({std::get<int64_t>(row.values[0].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[1].payload))});
  }
  void ScanSuppliers(const std::function<void(const TpchQ5SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers)
      consumer({std::get<int64_t>(row.values[0].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[1].payload))});
  }
  void ScanOrders(const std::function<void(const TpchQ5OrderInput &)> &consumer) override {
    for (const auto &row : orders)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<AuxDate>(row.values[2].payload).days_since_epoch});
  }
  void ScanLineitems(const std::function<void(const TpchQ5LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &price = std::get<ScaledDecimal>(row.values[2].payload);
      const auto &discount = std::get<ScaledDecimal>(row.values[3].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload), price.value,
                price.scale, discount.value, discount.scale});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> regions;
  std::vector<AuxRow> nations;
  std::vector<AuxRow> customers;
  std::vector<AuxRow> suppliers;
  std::vector<AuxRow> orders;
  std::vector<AuxRow> lineitems;
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
    assert(schema.columns.size() == 2);
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

void RequireResult(TpchQ5Module &module, Services &services,
                   const std::string &region, int32_t start_days,
                   const std::vector<std::pair<std::string, int64_t>> &expected) {
  Sink sink;
  module.Execute(
      services,
      QueryArguments{{String(region), Date(start_days)}},
      sink);
  assert(sink.ended);
  assert(sink.rows.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); i++) {
    assert(std::get<std::string>(sink.rows[i].values[0].payload) == expected[i].first);
    const auto revenue = std::get<ScaledDecimal>(sink.rows[i].values[1].payload);
    assert(revenue.value == expected[i].second && revenue.scale == 4);
  }
}

} // namespace

int main() {
  Services services;
  services.regions.push_back({{Int(1), String("AMERICA")}});
  services.nations.push_back({{Int(1), String("USA"), Int(1)}});
  services.nations.push_back({{Int(2), String("CANADA"), Int(1)}});
  services.customers.push_back({{Int(10), Int(1)}});
  services.customers.push_back({{Int(11), Int(2)}});
  services.suppliers.push_back({{Int(20), Int(1)}});
  services.suppliers.push_back({{Int(21), Int(2)}});

  const int32_t d1 = accelerator_codegen::aux_state::query::TpchQ5DaysFromYmd(1995, 3, 15);
  const int32_t d2 = accelerator_codegen::aux_state::query::TpchQ5DaysFromYmd(1995, 10, 20);
  services.orders.push_back({{Int(100), Int(10), Date(d1)}});
  services.orders.push_back({{Int(101), Int(11), Date(d2)}});

  // lineitem on order 100/supplier 20: price 100.00 (raw 10000), discount 0.05 (raw 5)
  // revenue = 10000 * (100-5) = 950000 at scale 4
  services.lineitems.push_back({{Int(100), Int(20), Decimal(10000, 2), Decimal(5, 2)}});
  // lineitem on order 100/supplier 21: customer nation 1 != supplier nation 2 -> excluded
  services.lineitems.push_back({{Int(100), Int(21), Decimal(50000, 2), Decimal(5, 2)}});
  // lineitem on order 101/supplier 21: customer nation 2 == supplier nation 2, price 200.00 raw 20000, discount 0.10 raw 10
  // revenue = 20000 * (100-10) = 1800000 at scale 4
  services.lineitems.push_back({{Int(101), Int(21), Decimal(20000, 2), Decimal(10, 2)}});

  TpchQ5Module module(TpchQ5Config{1, 2, 3, 4, 5, 6}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  int32_t start = accelerator_codegen::aux_state::query::TpchQ5DaysFromYmd(1995, 1, 1);

  RequireResult(module, services, "AMERICA", start,
                {{"CANADA", 1800000}, {"USA", 950000}});

  RequireResult(module, services, "ASIA", start, {});

  // Mutation path: update lineitem price on included order 100.
  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ5Module>(TpchQ5Config{1, 2, 3, 4, 5, 6}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 6;
  update.kind = MutationKind::Update;
  update.updated_columns = {2};
  update.old_rows = {services.lineitems[0]};
  update.new_rows = {AuxRow{{Int(100), Int(20), Decimal(20000, 2), Decimal(5, 2)}}};
  services.lineitems[0] = update.new_rows.front();
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  // Now USA revenue: 20000 * 95 = 1900000, CANADA unchanged: 1800000
  RequireResult(module, services, "AMERICA", start,
                {{"USA", 1900000}, {"CANADA", 1800000}});

  return 0;
}
