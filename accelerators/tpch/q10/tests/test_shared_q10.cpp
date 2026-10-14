#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "tpch/q10/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ10Config;
using accelerator_codegen::aux_state::query::TpchQ10Customer;
using accelerator_codegen::aux_state::query::TpchQ10CustomerInput;
using accelerator_codegen::aux_state::query::TpchQ10InitialScan;
using accelerator_codegen::aux_state::query::TpchQ10LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ10Module;
using accelerator_codegen::aux_state::query::TpchQ10MonthStartDays;
using accelerator_codegen::aux_state::query::TpchQ10NationInput;
using accelerator_codegen::aux_state::query::TpchQ10OrderInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}
AuxRow Customer(int64_t key, const std::string &name) {
  return {{Int(key), String(name), Decimal(1000, 2), Int(1), String("address"),
           String("phone"), String("comment")}};
}
AuxRow Nation() { return {{Int(1), String("USA")}}; }
AuxRow Order(int64_t key) {
  return {
      {Int(key), Int(1),
       AuxValue::Of(AuxType::Date, AuxDate{TpchQ10MonthStartDays(199306)})}};
}
AuxRow Lineitem(int64_t orderkey, int64_t price_raw) {
  return {{Int(orderkey), Decimal(price_raw, 2), Decimal(5, 2), String("R")}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q10";
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
    return key.accelerator == "tpch_q10" && valid
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

class Services final : public AuxEngineServices, public TpchQ10InitialScan {
public:
  void ScanCustomers(const std::function<void(const TpchQ10CustomerInput &)> &consumer) override {
    for (const auto &row : customers) {
      const auto &balance = std::get<ScaledDecimal>(row.values[2].payload);
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload), balance.value,
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload)),
                std::get<std::string>(row.values[4].payload),
                std::get<std::string>(row.values[5].payload),
                std::get<std::string>(row.values[6].payload)});
    }
  }
  void ScanNations(const std::function<void(const TpchQ10NationInput &)> &consumer) override {
    for (const auto &row : nations)
      consumer({static_cast<int32_t>(std::get<int64_t>(row.values[0].payload)),
                std::get<std::string>(row.values[1].payload)});
  }
  void ScanOrders(const std::function<void(const TpchQ10OrderInput &)> &consumer) override {
    for (const auto &row : orders)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload),
                std::get<AuxDate>(row.values[2].payload).days_since_epoch});
  }
  void ScanLineitems(const std::function<void(const TpchQ10LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &price = std::get<ScaledDecimal>(row.values[1].payload);
      const auto &discount = std::get<ScaledDecimal>(row.values[2].payload);
      consumer({std::get<int64_t>(row.values[0].payload), price.value,
                price.scale, discount.value, discount.scale,
                std::get<std::string>(row.values[3].payload) == "R"});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> customers;
  std::vector<AuxRow> nations;
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
    assert(schema.columns.size() == 8);
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

void RequireResult(TpchQ10Module &module, Services &services,
                   int64_t expected_raw) {
  Sink sink;
  module.Execute(services,
                 QueryArguments{{AuxValue::Of(
                     AuxType::Date, AuxDate{TpchQ10MonthStartDays(199306)})}},
                 sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(std::get<int64_t>(sink.rows[0].values[0].payload) == 1);
  const auto revenue = std::get<ScaledDecimal>(sink.rows[0].values[2].payload);
  assert(revenue.value == expected_raw && revenue.scale == 4);
}

} // namespace

int main() {
  {
    accelerator_codegen::aux_state::maintenance::CostModelParams gated;
    gated.apply_delta_max_rows = 1024;
    const auto small = SelectAuxMaintenanceAction(
        {1, 1, true, true, true, false, false, MaintenanceAction::NoOp, 10.0},
        gated);
    assert(small.action == MaintenanceAction::ApplyDelta);
    const auto bulk = SelectAuxMaintenanceAction(
        {1025, 1, true, true, true, false, false, MaintenanceAction::NoOp,
         10.0},
        gated);
    assert(bulk.action == MaintenanceAction::RebuildPartition);
  }

  Services services;
  services.customers.push_back(Customer(1, "Alice"));
  services.nations.push_back(Nation());
  services.orders.push_back(Order(10));
  services.lineitems.push_back(Lineitem(10, 10000));

  TpchQ10Module module(TpchQ10Config{1, 2, 3, 4}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(
      services, QueryArguments{{AuxValue::Of(
                    AuxType::Date, AuxDate{TpchQ10MonthStartDays(199306)})}}));
  RequireResult(module, services, 950000);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ10Module>(TpchQ10Config{1, 2, 3, 4}, &services));
  coordinator.AttachServices(services);
  MutationBatch update;
  update.table = 4;
  update.kind = MutationKind::Update;
  update.updated_columns = {1};
  update.old_rows = {Lineitem(10, 10000)};
  update.new_rows = {Lineitem(10, 12000)};
  services.lineitems[0] = update.new_rows.front();
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();
  RequireResult(module, services, 1140000);

  MutationBatch customer_update;
  customer_update.table = 1;
  customer_update.kind = MutationKind::Update;
  customer_update.updated_columns = {1};
  customer_update.old_rows = {Customer(1, "Alice")};
  customer_update.new_rows = {Customer(1, "Alice Updated")};
  services.customers[0] = customer_update.new_rows.front();
  coordinator.OnMutation(customer_update);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(module.CheckValidity(
      services, QueryArguments{{AuxValue::Of(
                    AuxType::Date, AuxDate{TpchQ10MonthStartDays(199306)})}}));
  RequireResult(module, services, 1140000);

  MutationBatch bulk_lineitems;
  bulk_lineitems.table = 4;
  bulk_lineitems.kind = MutationKind::Insert;
  for (int i = 0; i < 1025; ++i) {
    bulk_lineitems.new_rows.push_back(Lineitem(10, 10000));
    services.lineitems.push_back(bulk_lineitems.new_rows.back());
  }
  coordinator.OnMutation(bulk_lineitems);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(module.CheckValidity(
      services, QueryArguments{{AuxValue::Of(
                    AuxType::Date, AuxDate{TpchQ10MonthStartDays(199306)})}}));
  RequireResult(module, services, 1140000 + 1025 * 950000);
  return 0;
}
