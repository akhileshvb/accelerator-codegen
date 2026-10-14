#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "tpch/q12/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ12Config;
using accelerator_codegen::aux_state::query::TpchQ12InitialScan;
using accelerator_codegen::aux_state::query::TpchQ12LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ12Module;
using accelerator_codegen::aux_state::query::TpchQ12OrderInput;
using accelerator_codegen::aux_state::query::TpchQ12DaysFromYmd;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}

AuxRow Order(int64_t key, const std::string &priority) {
  return {{Int(key), String(priority)}};
}

AuxRow Lineitem(int64_t orderkey, const std::string &shipmode, int32_t receipt) {
  return {{Int(orderkey), String(shipmode), Date(receipt - 10), Date(receipt),
           Date(receipt - 20)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q12";
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
    return key.accelerator == "tpch_q12" && valid
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

class Services final : public AuxEngineServices, public TpchQ12InitialScan {
public:
  void ScanOrders(
      const std::function<void(const TpchQ12OrderInput &)> &consumer) override {
    for (const auto &row : orders)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
  }
  void ScanLineitems(
      const std::function<void(const TpchQ12LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<AuxDate>(row.values[2].payload).days_since_epoch,
                std::get<AuxDate>(row.values[3].payload).days_since_epoch,
                std::get<AuxDate>(row.values[4].payload).days_since_epoch});
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
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
    assert(schema.columns.size() == 3);
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

} // namespace

int main() {
  const auto start = TpchQ12DaysFromYmd(1995, 1, 1);
  const auto receipt = start + 30;

  Services services;
  services.orders.push_back(Order(10, "1-URGENT"));
  services.lineitems.push_back(Lineitem(10, "MAIL", receipt));

  TpchQ12Module module(TpchQ12Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  Sink sink;
  module.Execute(services,
                 QueryArguments{{String("MAIL"), String("SHIP"), Date(start)}},
                 sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(std::get<std::string>(sink.rows[0].values[0].payload) == "MAIL");
  assert(std::get<int64_t>(sink.rows[0].values[1].payload) == 1);
  assert(std::get<int64_t>(sink.rows[0].values[2].payload) == 0);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ12Module>(TpchQ12Config{1, 2}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 2;
  update.kind = MutationKind::Insert;
  update.new_rows = {Lineitem(10, "MAIL", receipt)};
  services.lineitems.push_back(update.new_rows.front());
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink2;
  module.Execute(services,
                 QueryArguments{{String("MAIL"), String("SHIP"), Date(start)}},
                 sink2);
  assert(sink2.ended);
  assert(std::get<int64_t>(sink2.rows[0].values[1].payload) == 2);
  return 0;
}
