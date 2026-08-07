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

#include "accelerator_codegen/aux_state/core/aux_registry.hpp"
#include "accelerator_codegen/aux_state/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q2_common.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::SecEdgarQ2Config;
using accelerator_codegen::aux_state::query::SecEdgarQ2InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ2Module;
using accelerator_codegen::aux_state::query::SecEdgarQ2NumInput;
using accelerator_codegen::aux_state::query::SecEdgarQ2SubInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}

AuxValue Double(double value) { return AuxValue::Of(AuxType::Double, value); }

AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

AuxRow Sub(const std::string &adsh, const std::string &name, int64_t fy) {
  return {{String(adsh), String(name), Int(fy)}};
}

AuxRow Num(const std::string &adsh, const std::string &tag, double value) {
  return {{String(adsh), String(tag), String("pure"), Double(value)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "sec_edgar_q2";
    snapshot.visibility = visibility;
    if (!valid) {
      return snapshot;
    }
    for (const auto &[partition, image] : images) {
      snapshot.partitions.push_back(image);
    }
    snapshot.available = snapshot.partitions.size() == 3;
    return snapshot;
  }

  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    if (key.accelerator != "sec_edgar_q2") {
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
    valid = images.size() == 3;
  }

  std::map<PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
};

class Services final : public AuxEngineServices, public SecEdgarQ2InitialScan {
public:
  void ScanNum(const std::function<void(const SecEdgarQ2NumInput &)> &consumer) override {
    for (const auto &row : num) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                std::get<double>(row.values[3].payload), false});
    }
  }

  void ScanSub(const std::function<void(const SecEdgarQ2SubInput &)> &consumer) override {
    for (const auto &row : sub) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload))});
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
  services.sub.push_back(Sub("adsh-a", "Alpha", 2022));
  services.sub.push_back(Sub("adsh-b", "Beta", 2022));
  services.sub.push_back(Sub("adsh-old", "OldCo", 2021));
  services.num.push_back(Num("adsh-a", "Assets", 10.0));
  services.num.push_back(Num("adsh-a", "Assets", 25.0));
  services.num.push_back(Num("adsh-a", "Liab", 5.0));
  services.num.push_back(Num("adsh-b", "Assets", 40.0));
  services.num.push_back(Num("adsh-old", "Assets", 100.0));

  SecEdgarQ2Module module(SecEdgarQ2Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(services, {}));

  Sink sink;
  module.Execute(services, {}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == 3);
  assert(std::get<std::string>(sink.rows[0].values[0].payload) == "Beta");
  assert(std::get<std::string>(sink.rows[0].values[1].payload) == "Assets");
  assert(std::abs(std::get<double>(sink.rows[0].values[2].payload) - 40.0) <= 1e-9);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<SecEdgarQ2Module>(SecEdgarQ2Config{1, 2}, &services));
  coordinator.AttachServices(services);

  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {Num("adsh-a", "Assets", 30.0)};
  services.num.push_back(insert.new_rows.front());
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  coordinator.Commit();

  sink.rows.clear();
  sink.ended = false;
  module.Execute(services, {}, sink);
  assert(sink.rows.size() == 3);
  assert(std::abs(std::get<double>(sink.rows[1].values[2].payload) - 30.0) <= 1e-9);

  return 0;
}
