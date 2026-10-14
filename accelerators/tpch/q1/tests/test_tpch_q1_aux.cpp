#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "tpch/q1/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ1Config;
using accelerator_codegen::aux_state::query::TpchQ1CutoffDays;
using accelerator_codegen::aux_state::query::TpchQ1InitialScan;
using accelerator_codegen::aux_state::query::TpchQ1LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ1Module;

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

AuxRow Lineitem(int32_t shipdate_days, int64_t quantity_raw, int64_t extended_raw,
                int64_t discount_raw, int64_t tax_raw, char returnflag,
                char linestatus) {
  return {{Date(shipdate_days), Decimal(quantity_raw, 2), Decimal(extended_raw, 2),
           Decimal(discount_raw, 2), Decimal(tax_raw, 2),
           String(std::string(1, returnflag)), String(std::string(1, linestatus))}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q1";
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
    return key.accelerator == "tpch_q1" && valid
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

class Services final : public AuxEngineServices, public TpchQ1InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const TpchQ1LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &quantity = std::get<ScaledDecimal>(row.values[1].payload);
      const auto &extended = std::get<ScaledDecimal>(row.values[2].payload);
      const auto &discount = std::get<ScaledDecimal>(row.values[3].payload);
      const auto &tax = std::get<ScaledDecimal>(row.values[4].payload);
      consumer({std::get<AuxDate>(row.values[0].payload).days_since_epoch,
                quantity.value, quantity.scale, extended.value, extended.scale,
                discount.value, discount.scale, tax.value, tax.scale,
                std::get<std::string>(row.values[5].payload).front(),
                std::get<std::string>(row.values[6].payload).front()});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
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
  void Begin(const OutputSchema &) override {}
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

int64_t CountOrders(const Sink &sink) {
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  return std::get<int64_t>(sink.rows[0].values[9].payload);
}

} // namespace

int main() {
  constexpr int32_t kDayOffset = 90;
  const int32_t shipdate = TpchQ1CutoffDays(kDayOffset) - 10;

  Services services;
  services.lineitems.push_back(
      Lineitem(shipdate, 1000, 10000, 500, 800, 'N', 'O'));

  TpchQ1Module module(TpchQ1Config{0}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  Sink sink;
  module.Execute(services, QueryArguments{{Int(kDayOffset)}}, sink);
  assert(CountOrders(sink) == 1);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ1Module>(TpchQ1Config{0}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 0;
  update.kind = MutationKind::Insert;
  update.new_rows = {Lineitem(shipdate, 2000, 20000, 500, 800, 'N', 'O')};
  services.lineitems.push_back(update.new_rows.front());
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink2;
  module.Execute(services, QueryArguments{{Int(kDayOffset)}}, sink2);
  assert(CountOrders(sink2) == 2);
  return 0;
}
