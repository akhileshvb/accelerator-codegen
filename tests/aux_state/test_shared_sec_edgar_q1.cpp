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
#include "sec_edgar/q1/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::SecEdgarQ1Config;
using accelerator_codegen::aux_state::query::SecEdgarQ1InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ1Module;
using accelerator_codegen::aux_state::query::SecEdgarQ1PreInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}

AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

AuxRow Pre(const std::string &stmt, const std::string &rfile,
           const std::string &adsh, int64_t line) {
  return {{String(stmt), String(rfile), String(adsh), Int(line)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "sec_edgar_q1";
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
    if (key.accelerator != "sec_edgar_q1") {
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

class Services final : public AuxEngineServices, public SecEdgarQ1InitialScan {
public:
  void ScanPre(
      const std::function<void(const SecEdgarQ1PreInput &)> &consumer) const override {
    for (const auto &row : pre) {
      bool stmt_is_null = row.values[0].is_null;
      bool rfile_is_null = row.values[1].is_null;
      consumer({stmt_is_null ? std::string()
                             : std::get<std::string>(row.values[0].payload),
                rfile_is_null ? std::string()
                              : std::get<std::string>(row.values[1].payload),
                rfile_is_null,
                stmt_is_null,
                std::get<std::string>(row.values[2].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload))});
    }
  }

  VisibilityToken CurrentVisibility() const override { return {}; }

  AuxStateStore &StateStore() override { return store; }

  Store store;
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
    assert(schema.columns.size() == 5);
  }

  void WriteRow(const AuxRow &row) override { rows.push_back(row); }

  void End() override { ended = true; }

  std::vector<AuxRow> rows;
  bool ended = false;
};

void ExecuteAndRequire(SecEdgarQ1Module &module, Services &services,
                       std::size_t expected_rows, int64_t expected_cnt,
                       int64_t expected_filings, double expected_avg) {
  Sink sink;
  module.Execute(services, {}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == expected_rows);
  if (expected_rows == 0) {
    return;
  }
  assert(std::get<int64_t>(sink.rows[0].values[2].payload) == expected_cnt);
  assert(std::get<int64_t>(sink.rows[0].values[3].payload) == expected_filings);
  assert(std::abs(std::get<double>(sink.rows[0].values[4].payload) -
                  expected_avg) <= 1e-9);
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
    services.pre.push_back(
        Pre("BS", "A", "adsh-" + std::to_string(i), 10 + i));
  }
  services.pre.push_back(Pre("IS", "B", "other-adsh", 5));

  SecEdgarQ1Module module(SecEdgarQ1Config{1}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 2, 12, 12, 15.5);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<SecEdgarQ1Module>(SecEdgarQ1Config{1}, &services));
  coordinator.AttachServices(services);

  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {Pre("BS", "A", "adsh-new", 20)};
  services.pre.push_back(insert.new_rows.front());
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  assert(services.store.valid);
  coordinator.Commit();
  ExecuteAndRequire(module, services, 2, 13, 13, 15.846153846);

  MutationBatch update;
  update.table = 1;
  update.kind = MutationKind::Update;
  update.updated_columns = {3};
  update.old_rows = {Pre("BS", "A", "adsh-new", 20)};
  update.new_rows = {Pre("BS", "A", "adsh-new", 30)};
  services.pre.back() = update.new_rows.front();
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();
  ExecuteAndRequire(module, services, 2, 13, 13, 16.615384615);

  MutationBatch erase;
  erase.table = 1;
  erase.kind = MutationKind::Delete;
  erase.old_rows = {Pre("BS", "A", "adsh-new", 20)};
  coordinator.OnMutation(erase);
  coordinator.PreCommit();
  coordinator.Abort();
  ExecuteAndRequire(module, services, 2, 13, 13, 16.615384615);

  MutationBatch group_change;
  group_change.table = 1;
  group_change.kind = MutationKind::Update;
  group_change.updated_columns = {0};
  group_change.old_rows = {Pre("BS", "A", "adsh-0", 10)};
  group_change.new_rows = {Pre("CF", "A", "adsh-0", 10)};
  services.pre[0] = group_change.new_rows.front();
  coordinator.OnMutation(group_change);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(!module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 3, 12, 12, 206.0 / 12.0);

  MutationBatch bulk;
  bulk.table = 1;
  bulk.kind = MutationKind::Insert;
  for (int i = 0; i < 1025; ++i) {
    bulk.new_rows.push_back(
        Pre("BS", "A", "adsh-bulk-" + std::to_string(i), 1));
    services.pre.push_back(bulk.new_rows.back());
  }
  coordinator.OnMutation(bulk);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(!module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 3, 1037, 1037, 1231.0 / 1037.0);

  return 0;
}
