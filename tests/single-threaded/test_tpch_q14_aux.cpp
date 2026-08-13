#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "tpch/q14/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ14Config;
using accelerator_codegen::aux_state::query::TpchQ14InitialScan;
using accelerator_codegen::aux_state::query::TpchQ14LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ14Module;
using accelerator_codegen::aux_state::query::TpchQ14PartInput;
using accelerator_codegen::aux_state::query::TpchQ14DaysFromYmd;
using accelerator_codegen::aux_state::query::TpchQ14MonthKeyFromDays;

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

AuxRow Part(int64_t key, const std::string &type) {
  return {{Int(key), String(type)}};
}
AuxRow Lineitem(int64_t partkey, int64_t price_raw, int32_t ship_days) {
  return {{Int(partkey), Decimal(price_raw, 2), Decimal(5, 2), Date(ship_days)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q14";
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
    return key.accelerator == "tpch_q14" && valid
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

class Services final : public AuxEngineServices, public TpchQ14InitialScan {
public:
  void ScanParts(
      const std::function<void(const TpchQ14PartInput &)> &consumer) override {
    for (const auto &row : parts)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
  }
  void ScanLineitems(
      const std::function<void(const TpchQ14LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &price = std::get<ScaledDecimal>(row.values[1].payload);
      const auto &discount = std::get<ScaledDecimal>(row.values[2].payload);
      consumer({std::get<int64_t>(row.values[0].payload), price.value,
                price.scale, discount.value, discount.scale,
                std::get<AuxDate>(row.values[3].payload).days_since_epoch});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> parts;
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

} // namespace

int main() {
  const auto start = TpchQ14DaysFromYmd(1995, 6, 1);
  const auto month = TpchQ14MonthKeyFromDays(start);

  Services services;
  services.parts.push_back(Part(1, "PROMO BOX"));
  services.lineitems.push_back(Lineitem(1, 10000, start + 5));

  TpchQ14Module module(TpchQ14Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  Sink sink;
  module.Execute(services, QueryArguments{{Date(start)}}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(!sink.rows[0].values[0].is_null);
  const auto promo = std::get<double>(sink.rows[0].values[0].payload);
  assert(std::fabs(promo - 100.0) < 1e-9);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ14Module>(TpchQ14Config{1, 2}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 2;
  update.kind = MutationKind::Insert;
  update.new_rows = {Lineitem(1, 10000, start + 6)};
  services.lineitems.push_back(update.new_rows.front());
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink2;
  module.Execute(services, QueryArguments{{Date(start)}}, sink2);
  assert(sink2.ended);
  const auto promo2 = std::get<double>(sink2.rows[0].values[0].payload);
  assert(std::fabs(promo2 - 100.0) < 1e-9);
  (void)month;
  return 0;
}
