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
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q6_common.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::SecEdgarQ6Config;
using accelerator_codegen::aux_state::query::SecEdgarQ6InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ6Module;
using accelerator_codegen::aux_state::query::SecEdgarQ6NumInput;
using accelerator_codegen::aux_state::query::SecEdgarQ6PreInput;
using accelerator_codegen::aux_state::query::SecEdgarQ6SubInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Double(double value) { return AuxValue::Of(AuxType::Double, value); }

AuxRow Sub(const std::string &adsh, const std::string &name, int64_t fy) {
  return {{String(adsh), String(name), Int(fy)}};
}
AuxRow Pre(const std::string &adsh, const std::string &tag,
           const std::string &version, const std::string &stmt,
           const std::string &plabel) {
  return {{String(adsh), String(tag), String(version), String(stmt),
           String(plabel)}};
}
AuxRow Num(const std::string &adsh, const std::string &tag,
           const std::string &version, double value) {
  return {{String(adsh), String(tag), String(version), String("USD"),
           Double(value)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "sec_edgar_q6";
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
    if (key.accelerator != "sec_edgar_q6") {
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

class Services final : public AuxEngineServices, public SecEdgarQ6InitialScan {
public:
  void ScanNum(
      const std::function<void(const SecEdgarQ6NumInput &)> &consumer) override {
    for (const auto &row : num) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                std::get<std::string>(row.values[3].payload),
                std::get<double>(row.values[4].payload), false});
    }
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ6SubInput &)> &consumer) override {
    for (const auto &row : sub) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload))});
    }
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ6PreInput &)> &consumer) override {
    for (const auto &row : pre) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                std::get<std::string>(row.values[3].payload),
                std::get<std::string>(row.values[4].payload)});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> num;
  std::vector<AuxRow> sub;
  std::vector<AuxRow> pre;
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
    assert(schema.columns.size() == 6);
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

void RequireTopRow(const Sink &sink, double expected_sum, int64_t expected_cnt) {
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(std::abs(std::get<double>(sink.rows[0].values[4].payload) -
                  expected_sum) <= 1e-9);
  assert(std::get<int64_t>(sink.rows[0].values[5].payload) == expected_cnt);
}

} // namespace

int main() {
  Services services;
  services.sub.push_back(Sub("adsh-1", "Acme Corp", 2023));
  services.pre.push_back(Pre("adsh-1", "Revenues", "v1", "IS", "Revenue"));
  services.num.push_back(Num("adsh-1", "Revenues", "v1", 100.0));

  SecEdgarQ6Module module(SecEdgarQ6Config{1, 2, 3}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(services, {}));

  Sink sink;
  module.Execute(services, {}, sink);
  RequireTopRow(sink, 100.0, 1);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<SecEdgarQ6Module>(SecEdgarQ6Config{1, 2, 3}, &services));
  coordinator.AttachServices(services);

  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {Num("adsh-1", "Revenues", "v1", 50.0)};
  services.num.push_back(insert.new_rows.front());
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink2;
  module.Execute(services, {}, sink2);
  RequireTopRow(sink2, 150.0, 2);
  return 0;
}
