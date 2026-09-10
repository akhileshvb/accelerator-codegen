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
#include "sec_edgar/q24/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::SecEdgarQ24Config;
using accelerator_codegen::aux_state::query::SecEdgarQ24InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ24Module;
using accelerator_codegen::aux_state::query::SecEdgarQ24NumInput;
using accelerator_codegen::aux_state::query::SecEdgarQ24PreInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}

AuxValue Double(double value) { return AuxValue::Of(AuxType::Double, value); }

AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

AuxRow Pre(const std::string &adsh, const std::string &tag,
           const std::string &version) {
  return {{String(adsh), String(tag), String(version)}};
}

AuxRow Num(const std::string &adsh, const std::string &tag,
           const std::string &version, double value, int64_t ddate = 20230601) {
  return {{String(adsh), String(tag), String(version), String("USD"),
           Double(value), Int(ddate)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "sec_edgar_q24";
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
    if (key.accelerator != "sec_edgar_q24") {
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

class Services final : public AuxEngineServices, public SecEdgarQ24InitialScan {
public:
  void ScanNum(const std::function<void(const SecEdgarQ24NumInput &)> &consumer) override {
    for (const auto &row : num) {
      bool is_null = row.values[4].is_null;
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                std::get<std::string>(row.values[3].payload),
                is_null ? 0.0 : std::get<double>(row.values[4].payload),
                is_null,
                static_cast<int32_t>(std::get<int64_t>(row.values[5].payload))});
    }
  }
  void ScanPre(const std::function<void(const SecEdgarQ24PreInput &)> &consumer) override {
    for (const auto &row : pre)
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload)});
  }

  VisibilityToken CurrentVisibility() const override { return {}; }

  AuxStateStore &StateStore() override { return store; }


  Store store;
  std::vector<AuxRow> num;
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
    assert(schema.columns.size() == 4);
  }

  void WriteRow(const AuxRow &row) override { rows.push_back(row); }

  void End() override { ended = true; }

  std::vector<AuxRow> rows;
  bool ended = false;
};

void ExecuteAndRequire(SecEdgarQ24Module &module, Services &services,
                       std::size_t expected_count, double expected_total) {
  Sink sink;
  module.Execute(services, {}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(std::get<int64_t>(sink.rows[0].values[2].payload) ==
         static_cast<int64_t>(expected_count));
  assert(std::abs(std::get<double>(sink.rows[0].values[3].payload) -
                  expected_total) <= 1e-9);
}

} // namespace

int main() {
  {
    accelerator_codegen::aux_state::maintenance::CostModelParams gated;
    gated.apply_delta_max_rows = 1024;
    const auto small = SelectAuxMaintenanceAction(
        {1, 1, true, false, true, false, false, MaintenanceAction::NoOp, 10.0},
        gated);
    assert(small.action == MaintenanceAction::ApplyDelta);
    const auto bulk = SelectAuxMaintenanceAction(
        {1025, 1, true, false, true, false, false, MaintenanceAction::NoOp,
         10.0},
        gated);
    assert(bulk.action == MaintenanceAction::RebuildAll);
  }

  Services services;
  for (int i = 0; i < 12; ++i) {
    services.num.push_back(
        Num("adsh-" + std::to_string(i), "Revenue", "1", 10.0 + i));
  }
  services.pre.push_back(Pre("unrelated", "Other", "1"));

  SecEdgarQ24Module module(SecEdgarQ24Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 12, 186.0);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<SecEdgarQ24Module>(SecEdgarQ24Config{1, 2}, &services));
  coordinator.AttachServices(services);
  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {Num("adsh-new", "Revenue", "1", 22.0)};
  services.num.push_back(insert.new_rows.front());
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  assert(services.store.valid);
  coordinator.Commit();
  ExecuteAndRequire(module, services, 13, 208.0);

  MutationBatch update;
  update.table = 1;
  update.kind = MutationKind::Update;
  update.updated_columns = {4};
  update.old_rows = {Num("adsh-new", "Revenue", "1", 22.0)};
  update.new_rows = {Num("adsh-new", "Revenue", "1", 32.0)};
  services.num.back() = update.new_rows.front();
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();
  ExecuteAndRequire(module, services, 13, 218.0);

  MutationBatch erase;
  erase.table = 1;
  erase.kind = MutationKind::Delete;
  erase.old_rows = {Num("adsh-new", "Revenue", "1", 22.0)};
  coordinator.OnMutation(erase);
  coordinator.PreCommit();
  coordinator.Abort();
  ExecuteAndRequire(module, services, 13, 218.0);

  MutationBatch pre_insert;
  pre_insert.table = 2;
  pre_insert.kind = MutationKind::Insert;
  pre_insert.new_rows = {Pre("adsh-0", "Revenue", "1")};
  services.pre.push_back(pre_insert.new_rows.front());
  coordinator.OnMutation(pre_insert);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(!module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 12, 208.0);

  MutationBatch bulk_num;
  bulk_num.table = 1;
  bulk_num.kind = MutationKind::Insert;
  for (int i = 0; i < 1025; ++i) {
    bulk_num.new_rows.push_back(
        Num("adsh-bulk-" + std::to_string(i), "Revenue", "1", 1.0));
    services.num.push_back(bulk_num.new_rows.back());
  }
  coordinator.OnMutation(bulk_num);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(!module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 1037, 1233.0);

  return 0;
}
