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
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q4_common.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::SecEdgarQ4Config;
using accelerator_codegen::aux_state::query::SecEdgarQ4InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ4Module;
using accelerator_codegen::aux_state::query::SecEdgarQ4NumInput;
using accelerator_codegen::aux_state::query::SecEdgarQ4PreInput;
using accelerator_codegen::aux_state::query::SecEdgarQ4SubInput;
using accelerator_codegen::aux_state::query::SecEdgarQ4TagInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}

AuxValue Double(double value) { return AuxValue::Of(AuxType::Double, value); }

AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

AuxRow Num(const std::string &adsh, const std::string &tag,
           const std::string &version, double value) {
  return {{String(adsh), String(tag), String(version), Double(value),
           String("USD")}};
}

AuxRow Sub(const std::string &adsh, int64_t cik, int64_t sic) {
  return {{String(adsh), Int(cik), Int(sic)}};
}

AuxRow Tag(const std::string &tag, const std::string &version,
           int64_t abstract_flag, const std::string &tlabel) {
  return {{String(tag), String(version), Int(abstract_flag), String(tlabel)}};
}

AuxRow Pre(const std::string &adsh, const std::string &tag,
           const std::string &version, const std::string &stmt) {
  return {{String(adsh), String(tag), String(version), String(stmt)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "sec_edgar_q4";
    snapshot.visibility = visibility;
    if (!valid) {
      return snapshot;
    }
    for (const auto &[partition, image] : images) {
      snapshot.partitions.push_back(image);
    }
    snapshot.available = snapshot.partitions.size() == 4;
    return snapshot;
  }

  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    if (key.accelerator != "sec_edgar_q4") {
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
    valid = images.size() == 4;
  }

  std::map<PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
};

class Services final : public AuxEngineServices, public SecEdgarQ4InitialScan {
public:
  void ScanSub(
      const std::function<void(const SecEdgarQ4SubInput &)> &consumer) const override {
    for (const auto &row : sub) {
      consumer({std::get<std::string>(row.values[0].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[1].payload)),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload))});
    }
  }

  void ScanTag(
      const std::function<void(const SecEdgarQ4TagInput &)> &consumer) const override {
    for (const auto &row : tag) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[2].payload)),
                std::get<std::string>(row.values[3].payload)});
    }
  }

  void ScanPre(
      const std::function<void(const SecEdgarQ4PreInput &)> &consumer) const override {
    for (const auto &row : pre) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                std::get<std::string>(row.values[3].payload)});
    }
  }

  void ScanNum(
      const std::function<void(const SecEdgarQ4NumInput &)> &consumer) const override {
    for (const auto &row : num) {
      bool is_null = row.values[3].is_null;
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                std::get<std::string>(row.values[4].payload),
                is_null ? 0.0 : std::get<double>(row.values[3].payload),
                is_null});
    }
  }

  VisibilityToken CurrentVisibility() const override { return {}; }

  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> num;
  std::vector<AuxRow> sub;
  std::vector<AuxRow> tag;
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

void ExecuteAndRequire(SecEdgarQ4Module &module, Services &services,
                       std::size_t expected_rows, int32_t expected_sic,
                       int64_t expected_companies, double expected_total) {
  Sink sink;
  module.Execute(services, {}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == expected_rows);
  if (expected_rows == 0) {
    return;
  }
  assert(std::get<int64_t>(sink.rows[0].values[0].payload) == expected_sic);
  assert(std::get<int64_t>(sink.rows[0].values[3].payload) ==
         expected_companies);
  assert(std::abs(std::get<double>(sink.rows[0].values[4].payload) -
                  expected_total) <= 1e-9);
}

} // namespace

int main() {
  {
    const auto small = SelectAuxMaintenanceAction(
        {1, 1, true, false, true, false, false, MaintenanceAction::NoOp, 10.0},
        accelerator_codegen::aux_state::maintenance::CostModelParams{});
    assert(small.action == MaintenanceAction::ApplyDelta);
    const auto bulk = SelectAuxMaintenanceAction(
        {1025, 1, true, false, true, false, false, MaintenanceAction::NoOp,
         10.0},
        accelerator_codegen::aux_state::maintenance::CostModelParams{});
    assert(bulk.action == MaintenanceAction::RebuildAll);
  }

  Services services;
  services.sub.push_back(Sub("adsh-a", 100, 4100));
  services.sub.push_back(Sub("adsh-b", 200, 4100));
  services.tag.push_back(Tag("Revenue", "1", 0, "Revenue"));
  services.pre.push_back(Pre("adsh-a", "Revenue", "1", "EQ"));
  services.pre.push_back(Pre("adsh-b", "Revenue", "1", "EQ"));
  services.num.push_back(Num("adsh-a", "Revenue", "1", 100.0));
  services.num.push_back(Num("adsh-b", "Revenue", "1", 200.0));

  SecEdgarQ4Module module(SecEdgarQ4Config{1, 2, 3, 4}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 1, 4100, 2, 300.0);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(std::make_shared<SecEdgarQ4Module>(SecEdgarQ4Config{1, 2, 3, 4},
                                                         &services));
  coordinator.AttachServices(services);

  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {Num("adsh-a", "Revenue", "1", 50.0)};
  services.num.push_back(insert.new_rows.front());
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  assert(services.store.valid);
  coordinator.Commit();
  ExecuteAndRequire(module, services, 1, 4100, 2, 350.0);

  MutationBatch update;
  update.table = 1;
  update.kind = MutationKind::Update;
  update.updated_columns = {3};
  update.old_rows = {Num("adsh-a", "Revenue", "1", 50.0)};
  update.new_rows = {Num("adsh-a", "Revenue", "1", 75.0)};
  services.num.back() = update.new_rows.front();
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();
  ExecuteAndRequire(module, services, 1, 4100, 2, 375.0);

  MutationBatch erase;
  erase.table = 1;
  erase.kind = MutationKind::Delete;
  erase.old_rows = {Num("adsh-a", "Revenue", "1", 50.0)};
  coordinator.OnMutation(erase);
  coordinator.PreCommit();
  coordinator.Abort();
  ExecuteAndRequire(module, services, 1, 4100, 2, 375.0);

  MutationBatch pre_insert;
  pre_insert.table = 4;
  pre_insert.kind = MutationKind::Insert;
  pre_insert.new_rows = {Pre("adsh-c", "Revenue", "1", "EQ")};
  services.pre.push_back(pre_insert.new_rows.front());
  coordinator.OnMutation(pre_insert);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(!module.CheckValidity(services, {}));
  ExecuteAndRequire(module, services, 1, 4100, 2, 375.0);

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
  ExecuteAndRequire(module, services, 1, 4100, 2, 375.0);

  return 0;
}
