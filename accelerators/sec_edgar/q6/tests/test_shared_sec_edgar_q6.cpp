// Module-level unit test for SEC EDGAR Q6 against an in-memory store and a
// fake typed scan source (no DuckDB). Rows use the module's projected layout
// (SecEdgarQ6Columns; the engine delivers the columns the module declares in
// TableDependency::mutation_attnums):
//   num(adsh, tag, version, uom, value)
//   sub(adsh, name, fy)
//   pre(adsh, stmt, tag, version, plabel)
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

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "sec_edgar/q6/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::SecEdgarQ6Columns;
using accelerator_codegen::aux_state::query::SecEdgarQ6Config;
using accelerator_codegen::aux_state::query::SecEdgarQ6InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ6Module;
using accelerator_codegen::aux_state::query::SecEdgarQ6NumInput;
using accelerator_codegen::aux_state::query::SecEdgarQ6PreInput;
using accelerator_codegen::aux_state::query::SecEdgarQ6SubInput;

namespace {

AuxValue S(std::string v) { return AuxValue::Of(AuxType::String, std::move(v)); }
AuxValue I(int64_t v) { return AuxValue::Of(AuxType::Int64, v); }
AuxValue D(double v) { return AuxValue::Of(AuxType::Double, v); }
AuxValue NullS() { return AuxValue::Null(AuxType::String); }

AuxRow Sub(const std::string &adsh, const std::string &name, int64_t fy) {
  return {{S(adsh), S(name), I(fy)}};
}
AuxRow Pre(const std::string &adsh, const std::string &tag, const std::string &version,
           const std::string &stmt, const std::string &plabel) {
  return {{S(adsh), S(stmt), S(tag), S(version), S(plabel)}};
}
AuxRow Num(const std::string &adsh, const std::string &tag, const std::string &version,
           double value, const std::string &uom = "USD") {
  return {{S(adsh), S(tag), S(version), S(uom), D(value)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "sec_edgar_q6";
    snapshot.visibility = visibility;
    if (!valid) return snapshot;
    for (const auto &[partition, image] : images) snapshot.partitions.push_back(image);
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key, const VisibilityToken &) const override {
    if (key.accelerator != "sec_edgar_q6") return {ValidityCode::Missing, 0};
    if (images.empty()) return {ValidityCode::Missing, 0};
    return {valid ? ValidityCode::Valid : ValidityCode::Invalidated, valid ? version : 0};
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
    return true;
  }
  void InstallBuild(std::vector<StateImage> built) {
    images.clear();
    for (auto &image : built) {
      image.version = 1;
      images.emplace(image.partition, std::move(image));
    }
    version = 1;
    valid = !images.empty();
  }
  std::map<PartitionId, StateImage> images;
  Version version = 0;
  bool valid = false;
};

class Services final : public AuxEngineServices, public SecEdgarQ6InitialScan {
public:
  void ScanNum(const std::function<void(const SecEdgarQ6NumInput &)> &consumer) override {
    for (const auto &row : num) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                std::get<std::string>(row.values[3].payload),
                std::get<double>(row.values[4].payload), false});
    }
  }
  void ScanSub(const std::function<void(const SecEdgarQ6SubInput &)> &consumer) override {
    for (const auto &row : sub) {
      SecEdgarQ6SubInput in;
      in.adsh = std::get<std::string>(row.values[0].payload);
      in.name_is_null = row.values[1].is_null;
      if (!in.name_is_null) in.name = std::get<std::string>(row.values[1].payload);
      in.fy_is_null = row.values[2].is_null;
      if (!in.fy_is_null) in.fy = static_cast<int32_t>(std::get<int64_t>(row.values[2].payload));
      consumer(in);
    }
  }
  void ScanPre(const std::function<void(const SecEdgarQ6PreInput &)> &consumer) override {
    for (const auto &row : pre) {
      SecEdgarQ6PreInput in;
      in.adsh = std::get<std::string>(row.values[0].payload);
      in.stmt = std::get<std::string>(row.values[1].payload);
      in.tag = std::get<std::string>(row.values[2].payload);
      in.version = std::get<std::string>(row.values[3].payload);
      in.plabel_is_null = row.values[4].is_null;
      if (!in.plabel_is_null) in.plabel = std::get<std::string>(row.values[4].payload);
      consumer(in);
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> num, sub, pre;
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
  void Begin(const OutputSchema &schema) override { assert(schema.columns.size() == 6); }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

void RequireRow(const Sink &sink, size_t index, const std::string &name, double sum, int64_t cnt) {
  assert(sink.ended);
  assert(index < sink.rows.size());
  assert(std::get<std::string>(sink.rows[index].values[0].payload) == name);
  assert(std::abs(std::get<double>(sink.rows[index].values[4].payload) - sum) <= 1e-9);
  assert(std::get<int64_t>(sink.rows[index].values[5].payload) == cnt);
}

class Recorder final : public SecEdgarQ6Module {
public:
  using SecEdgarQ6Module::SecEdgarQ6Module;
  StagedMaintenanceOutcome StageMaintenance(AuxEngineServices &s, const InvalidationPlan &p,
                                            AuxTxnBuffer &b) override {
    auto out = SecEdgarQ6Module::StageMaintenance(s, p, b);
    last = out.action;
    return out;
  }
  MaintenanceAction last = MaintenanceAction::NoOp;
};

} // namespace

int main() {
  Services services;
  services.sub.push_back(Sub("adsh-1", "Acme Corp", 2023));
  services.sub.push_back(Sub("adsh-2", "Other Inc", 2022));
  services.pre.push_back(Pre("adsh-1", "Revenues", "v1", "IS", "Revenue"));
  services.pre.push_back(Pre("adsh-2", "Revenues", "v1", "IS", "Revenue"));
  services.num.push_back(Num("adsh-1", "Revenues", "v1", 100.0));
  services.num.push_back(Num("adsh-2", "Revenues", "v1", 900.0));

  auto module = std::make_shared<Recorder>(SecEdgarQ6Config{1, 2, 3}, &services);
  Builder builder;
  module->Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
  assert(module->CheckValidity(services, {}));

  Sink sink;
  module->Execute(services, {}, sink);
  assert(sink.rows.size() == 1);
  RequireRow(sink, 0, "Acme Corp", 100.0, 1);

  AuxTransactionCoordinator coordinator;
  coordinator.Register(module);
  coordinator.AttachServices(services);

  auto commit = [&](MutationBatch m) {
    coordinator.OnMutation(m);
    coordinator.PreCommit();
    coordinator.Commit();
  };

  // 1. num insert -> ApplyDelta
  {
    MutationBatch m;
    m.table = 1;
    m.kind = MutationKind::Insert;
    m.new_rows = {Num("adsh-1", "Revenues", "v1", 50.0)};
    services.num.push_back(m.new_rows.front());
    commit(m);
    assert(module->last == MaintenanceAction::ApplyDelta);
    Sink s;
    module->Execute(services, {}, s);
    RequireRow(s, 0, "Acme Corp", 150.0, 2);
  }
  // 2. num update value (+ non-USD row ignored)
  {
    MutationBatch m;
    m.table = 1;
    m.kind = MutationKind::Update;
    m.updated_columns = {SecEdgarQ6Columns::kNumValue};
    m.old_rows = {Num("adsh-1", "Revenues", "v1", 50.0)};
    m.new_rows = {Num("adsh-1", "Revenues", "v1", 80.0)};
    services.num[2] = m.new_rows.front();
    commit(m);
    assert(module->last == MaintenanceAction::ApplyDelta);
    Sink s;
    module->Execute(services, {}, s);
    RequireRow(s, 0, "Acme Corp", 180.0, 2);
  }
  // 3. num delete
  {
    MutationBatch m;
    m.table = 1;
    m.kind = MutationKind::Delete;
    m.old_rows = {Num("adsh-1", "Revenues", "v1", 80.0)};
    services.num.erase(services.num.begin() + 2);
    commit(m);
    Sink s;
    module->Execute(services, {}, s);
    RequireRow(s, 0, "Acme Corp", 100.0, 1);
  }
  // 4. update that changed none of the projected columns (the engine still
  //    reports it with equal old/new rows) -> no rebuild, same answer
  {
    MutationBatch m;
    m.table = 1;
    m.kind = MutationKind::Update;
    m.old_rows = {Num("adsh-1", "Revenues", "v1", 100.0)};
    m.new_rows = m.old_rows;
    module->last = MaintenanceAction::RebuildAll;
    commit(m);
    assert(module->last != MaintenanceAction::RebuildAll);
    Sink s;
    module->Execute(services, {}, s);
    RequireRow(s, 0, "Acme Corp", 100.0, 1);
  }
  // 5. sub fy flip brings adsh-2 into the cohort -> rebuild
  {
    MutationBatch m;
    m.table = 2;
    m.kind = MutationKind::Update;
    m.updated_columns = {SecEdgarQ6Columns::kSubFy};
    m.old_rows = {Sub("adsh-2", "Other Inc", 2022)};
    m.new_rows = {Sub("adsh-2", "Other Inc", 2023)};
    services.sub[1] = m.new_rows.front();
    commit(m);
    assert(module->last == MaintenanceAction::RebuildAll);
    Sink s;
    module->Execute(services, {}, s);
    assert(s.rows.size() == 2);
    RequireRow(s, 0, "Other Inc", 900.0, 1);
    RequireRow(s, 1, "Acme Corp", 100.0, 1);
  }
  // 6. pre insert (extra label for Acme) -> rebuild, count 2 groups
  {
    MutationBatch m;
    m.table = 3;
    m.kind = MutationKind::Insert;
    m.new_rows = {Pre("adsh-1", "Revenues", "v1", "IS", "Sales")};
    services.pre.push_back(m.new_rows.front());
    commit(m);
    assert(module->last == MaintenanceAction::RebuildAll);
    Sink s;
    module->Execute(services, {}, s);
    assert(s.rows.size() == 3);
  }
  // 7. pre insert for an adsh that is not in the cohort: no maintenance (the
  //    state is valid again after a fresh build)
  {
    Builder rebuilt;
    module->Build(services, rebuilt);
    services.store.InstallBuild(std::move(rebuilt.images));
    MutationBatch m;
    m.table = 3;
    m.kind = MutationKind::Insert;
    m.new_rows = {Pre("adsh-zzz", "Revenues", "v1", "BS", "Sales")};
    module->last = MaintenanceAction::ApplyDelta;
    commit(m);
    assert(module->last == MaintenanceAction::NoOp);
  }
  // 8. malformed (short) num row -> safe rebuild
  {
    MutationBatch m;
    m.table = 1;
    m.kind = MutationKind::Insert;
    AuxRow bad;
    bad.values = {S("adsh-1"), S("Revenues"), S("v1")};
    m.new_rows = {bad};
    commit(m);
    assert(module->last == MaintenanceAction::RebuildAll);
    Sink s;
    module->Execute(services, {}, s);
    assert(s.rows.size() == 3);
  }
  // 9. parameters: any (fy, stmt, uom) is answered from the same state.
  {
    const auto args = [](AuxValue fy, AuxValue stmt, AuxValue uom) {
      QueryArguments a;
      a.values = {std::move(fy), std::move(stmt), std::move(uom)};
      return a;
    };
    Sink s;
    module->Execute(services, args(I(2022), S("IS"), S("USD")), s);
    assert(s.rows.empty()); // adsh-2 moved to 2023 in step 5
    Sink s2;
    module->Execute(services, args(I(2023), S("IS"), S("USD")), s2);
    assert(s2.rows.size() == 3);
    Sink s3;
    module->Execute(services, args(I(2023), S("BS"), S("USD")), s3);
    assert(s3.rows.empty());
    Sink s4;
    module->Execute(services, args(I(2023), S("IS"), S("shares")), s4);
    assert(s4.rows.empty());
    Sink s5;
    module->Execute(services, args(AuxValue::Null(AuxType::Int64), S("IS"), S("USD")), s5);
    assert(s5.rows.empty() && s5.ended);
    // a num row in another uom and a BS label appear incrementally
    MutationBatch m;
    m.table = 1;
    m.kind = MutationKind::Insert;
    m.new_rows = {Num("adsh-1", "Revenues", "v1", 7.0, "CAD")};
    services.num.push_back(m.new_rows.front());
    commit(m);
    Sink s6;
    module->Execute(services, args(I(2023), S("IS"), S("CAD")), s6);
    assert(s6.rows.size() == 2);
    RequireRow(s6, 0, "Acme Corp", 7.0, 1);
    Sink s7;
    module->Execute(services, {}, s7);
    assert(s7.rows.size() == 3);
  }
  return 0;
}
