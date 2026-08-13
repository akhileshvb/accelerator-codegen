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

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "tpch/q6/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ6Config;
using accelerator_codegen::aux_state::query::TpchQ6InitialScan;
using accelerator_codegen::aux_state::query::TpchQ6LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ6Module;

namespace {

constexpr int32_t kEpoch = 719468;

int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}

AuxRow Lineitem(int32_t shipdate_days, int64_t discount_raw, int64_t quantity_raw,
                int64_t price_raw) {
  return {{Date(shipdate_days), Decimal(discount_raw, 2), Decimal(quantity_raw, 2),
           Decimal(price_raw, 2)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q6";
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
    return key.accelerator == "tpch_q6" && valid
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

class Services final : public AuxEngineServices, public TpchQ6InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems)
      consumer({std::get<AuxDate>(row.values[0].payload).days_since_epoch,
                std::get<ScaledDecimal>(row.values[1].payload).value, 2,
                std::get<ScaledDecimal>(row.values[2].payload).value, 2,
                std::get<ScaledDecimal>(row.values[3].payload).value, 2});
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
  void Begin(const OutputSchema &schema) override {
    assert(schema.columns.size() == 1);
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

void RequireRevenue(TpchQ6Module &module, Services &services, int32_t start_days,
                    int64_t expected_raw) {
  Sink sink;
  module.Execute(services,
                 QueryArguments{{Date(start_days), AuxValue::Of(AuxType::Double, 0.03),
                                 AuxValue::Of(AuxType::Double, 25.0)}},
                 sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  const auto revenue = std::get<ScaledDecimal>(sink.rows[0].values[0].payload);
  assert(revenue.value == expected_raw && revenue.scale == 4);
}

} // namespace

int main() {
  const auto start_date = DaysFromCivil(1995, 1, 1);
  const auto shipdate = DaysFromCivil(1995, 6, 15);

  Services services;
  services.lineitems.push_back(Lineitem(shipdate, 3, 1000, 10000));

  TpchQ6Module module(TpchQ6Config{0}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  RequireRevenue(module, services, start_date, 30000);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ6Module>(TpchQ6Config{0}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 0;
  update.kind = MutationKind::Insert;
  update.new_rows = {Lineitem(shipdate, 3, 1000, 10000)};
  services.lineitems.push_back(update.new_rows.front());
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  RequireRevenue(module, services, start_date, 60000);
  return 0;
}
