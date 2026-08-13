#ifdef NDEBUG
#undef NDEBUG
#endif
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
#include "tpch/q3/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ3Config;
using accelerator_codegen::aux_state::query::TpchQ3CustomerInput;
using accelerator_codegen::aux_state::query::TpchQ3InitialScan;
using accelerator_codegen::aux_state::query::TpchQ3LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ3Module;
using accelerator_codegen::aux_state::query::TpchQ3OrderInput;
using accelerator_codegen::aux_state::query::TpchQ3DaysFromYmd;

namespace {

AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}
AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

AuxRow Customer(int64_t key) {
  return {{Int(key), String("BUILDING")}};
}
AuxRow Order(int64_t key, int32_t orderdate_days) {
  return {{Int(key), Date(orderdate_days), Int(key), Int(0)}};
}
AuxRow Lineitem(int64_t orderkey, int64_t extended_raw, int64_t discount_raw,
                int32_t shipdate_days) {
  return {{Int(orderkey), Decimal(extended_raw, 2), Decimal(discount_raw, 2),
           Date(shipdate_days)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q3";
    snapshot.visibility = visibility;
    if (!valid) {
      return snapshot;
    }
    for (const auto &[partition, image] : images) {
      snapshot.partitions.push_back(image);
    }
    snapshot.available = !images.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == "tpch_q3" && valid
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

class Services final : public AuxEngineServices, public TpchQ3InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const TpchQ3CustomerInput &)> &consumer) override {
    for (const auto &row : customers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  void ScanOrders(
      const std::function<void(const TpchQ3OrderInput &)> &consumer) override {
    for (const auto &row : orders) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<AuxDate>(row.values[1].payload).days_since_epoch,
                std::get<int64_t>(row.values[2].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload))});
    }
  }
  void ScanLineitems(
      const std::function<void(const TpchQ3LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<ScaledDecimal>(row.values[1].payload).value, 2,
                std::get<ScaledDecimal>(row.values[2].payload).value, 2,
                std::get<AuxDate>(row.values[3].payload).days_since_epoch});
    }
  }
  bool LookupOrderContext(int64_t orderkey, OrderContext &out) override {
    out = {};
    for (const auto &row : orders) {
      if (std::get<int64_t>(row.values[0].payload) != orderkey)
        continue;
      const auto custkey = std::get<int64_t>(row.values[2].payload);
      out.orderdate_days =
          std::get<AuxDate>(row.values[1].payload).days_since_epoch;
      out.custkey = custkey;
      out.shippriority =
          static_cast<int32_t>(std::get<int64_t>(row.values[3].payload));
      for (const auto &cust : customers) {
        if (std::get<int64_t>(cust.values[0].payload) != custkey)
          continue;
        out.segment = accelerator_codegen::aux_state::query::
            TpchQ3PartitionFromMktsegment(
                std::get<std::string>(cust.values[1].payload));
        return out.segment != 0;
      }
      return false;
    }
    return false;
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> customers;
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
    assert(schema.columns.size() == 4);
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

int64_t RevenueRaw(const Sink &sink) {
  assert(sink.rows.size() == 1);
  return std::get<ScaledDecimal>(sink.rows[0].values[1].payload).value;
}

} // namespace

int main() {
  const auto orderdate = TpchQ3DaysFromYmd(1995, 1, 1);
  const auto cutoff = TpchQ3DaysFromYmd(1995, 3, 15);
  const auto shipdate = TpchQ3DaysFromYmd(1995, 4, 1);

  Services services;
  services.customers.push_back(Customer(1));
  services.orders.push_back(Order(1, orderdate));
  services.lineitems.push_back(Lineitem(1, 10000, 10, shipdate));

  TpchQ3Module module(TpchQ3Config{1, 2, 3}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(services, {}));

  Sink sink;
  module.Execute(services,
                 QueryArguments{{String("BUILDING"), Date(cutoff)}}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(RevenueRaw(sink) == 900000);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ3Module>(TpchQ3Config{1, 2, 3}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 3;
  update.kind = MutationKind::Update;
  update.updated_columns = {2};
  update.old_rows = {services.lineitems.front()};
  services.lineitems[0] = Lineitem(1, 10000, 20, shipdate);
  update.new_rows = {services.lineitems.front()};
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink2;
  module.Execute(services,
                 QueryArguments{{String("BUILDING"), Date(cutoff)}}, sink2);
  assert(sink2.ended);
  assert(RevenueRaw(sink2) == 800000);

  // RF-style: insert new order+lineitem then delete them; Execute still works.
  {
    services.customers.push_back(Customer(2));
    services.orders.push_back(Order(2, orderdate));
    MutationBatch ord_ins;
    ord_ins.table = 2;
    ord_ins.kind = MutationKind::Insert;
    ord_ins.new_rows = {Order(2, orderdate)};
    coordinator.OnMutation(ord_ins);

    const auto new_li = Lineitem(2, 10000, 10, shipdate);
    services.lineitems.push_back(new_li);
    MutationBatch li_ins;
    li_ins.table = 3;
    li_ins.kind = MutationKind::Insert;
    li_ins.new_rows = {new_li};
    coordinator.OnMutation(li_ins);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink3;
    module.Execute(services,
                   QueryArguments{{String("BUILDING"), Date(cutoff)}}, sink3);
    assert(sink3.ended);
    assert(sink3.rows.size() == 2);

    MutationBatch li_del;
    li_del.table = 3;
    li_del.kind = MutationKind::Delete;
    li_del.old_rows = {new_li};
    coordinator.OnMutation(li_del);
    MutationBatch ord_del;
    ord_del.table = 2;
    ord_del.kind = MutationKind::Delete;
    ord_del.old_rows = {Order(2, orderdate)};
    coordinator.OnMutation(ord_del);
    coordinator.PreCommit();
    coordinator.Commit();

    Sink sink4;
    module.Execute(services,
                   QueryArguments{{String("BUILDING"), Date(cutoff)}}, sink4);
    assert(sink4.ended);
    assert(sink4.rows.size() == 1);
    assert(RevenueRaw(sink4) == 800000);
  }
  return 0;
}
