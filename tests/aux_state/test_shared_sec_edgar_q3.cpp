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
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "sec_edgar/q3/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::SecEdgarQ3Config;
using accelerator_codegen::aux_state::query::SecEdgarQ3InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ3Module;
using accelerator_codegen::aux_state::query::SecEdgarQ3NumInput;
using accelerator_codegen::aux_state::query::SecEdgarQ3SubInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}

AuxValue Double(double value) { return AuxValue::Of(AuxType::Double, value); }

AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

AuxRow Sub(const std::string &adsh, const std::string &name, int64_t cik,
           int64_t fy) {
  return {{String(adsh), String(name), Int(cik), Int(fy)}};
}

AuxRow Num(const std::string &adsh, double value) {
  return {{String(adsh), String("USD"), Double(value)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "sec_edgar_q3";
    snapshot.visibility = visibility;
    if (!valid) {
      return snapshot;
    }
    for (const auto &[partition, image] : images) {
      snapshot.partitions.push_back(image);
    }
    snapshot.available = snapshot.partitions.size() == 2;
    return snapshot;
  }

  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    if (key.accelerator != "sec_edgar_q3") {
      return {ValidityCode::Missing, 0};
    }
    return {valid ? ValidityCode::Valid : ValidityCode::Invalidated,
            valid ? version : 0};
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
    version++;
    valid = true;
    return true;
  }

  void InstallBuild(std::vector<StateImage> built) {
    images.clear();
    for (auto &image : built) {
      image.version = 1;
      images.emplace(image.partition, std::move(image));
    }
    version = 1;
    valid = images.size() == 2;
  }

  std::map<PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
};

class Services final : public AuxEngineServices, public SecEdgarQ3InitialScan {
public:
  void ScanNum(const std::function<void(const SecEdgarQ3NumInput &)> &consumer) override {
    for (const auto &row : num) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<double>(row.values[2].payload), false});
    }
  }

  void ScanSub(const std::function<void(const SecEdgarQ3SubInput &)> &consumer) override {
    for (const auto &row : sub) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload)),
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload))});
    }
  }

  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> num;
  std::vector<AuxRow> sub;
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
  Services services;
  services.sub.push_back(Sub("adsh-a", "Alpha", 1, 2022));
  services.sub.push_back(Sub("adsh-b", "Beta", 2, 2022));
  services.sub.push_back(Sub("adsh-c", "Gamma", 3, 2022));
  services.num.push_back(Num("adsh-a", 10.0));
  services.num.push_back(Num("adsh-b", 20.0));
  services.num.push_back(Num("adsh-c", 100.0));

  SecEdgarQ3Module module(SecEdgarQ3Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(services, {}));

  Sink sink;
  module.Execute(services, {}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(std::get<std::string>(sink.rows[0].values[0].payload) == "Gamma");
  assert(std::get<int64_t>(sink.rows[0].values[1].payload) == 3);
  assert(std::abs(std::get<double>(sink.rows[0].values[2].payload) - 100.0) <=
         1e-9);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<SecEdgarQ3Module>(SecEdgarQ3Config{1, 2}, &services));
  coordinator.AttachServices(services);

  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {Num("adsh-a", 51.0)};
  services.num.push_back(insert.new_rows.front());
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  coordinator.Commit();

  sink.rows.clear();
  sink.ended = false;
  module.Execute(services, {}, sink);
  assert(sink.rows.size() == 2);
  assert(std::get<std::string>(sink.rows[0].values[0].payload) == "Gamma");

  return 0;
}
