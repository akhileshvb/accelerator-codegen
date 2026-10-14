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
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "sec_edgar/q2/duckdb/aux/module.hpp"

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

// Mutation rows use the physical DuckDB layouts:
//   sub: 0 adsh, 1 cik, 2 name, ..., 10 fy (20 columns)
//   num: 0 adsh, 1 tag, 2 version, 3 ddate, 4 qtrs, 5 uom, 6 coreg, 7 value, 8 footnote
AuxRow Sub(const std::string &adsh, const std::string &name, int64_t fy) {
  AuxRow row;
  row.values.assign(20, AuxValue::Null(AuxType::String));
  row.values[0] = String(adsh);
  row.values[1] = Int(1);
  row.values[2] = String(name);
  row.values[10] = Int(fy);
  return row;
}

AuxRow Num(const std::string &adsh, const std::string &tag, double value,
           const std::string &uom = "pure") {
  return {{String(adsh), String(tag), String("v"), Int(20220101), Int(4),
           String(uom), AuxValue::Null(AuxType::String), Double(value),
           AuxValue::Null(AuxType::String)}};
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
    snapshot.available = !snapshot.partitions.empty();
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
    valid = !images.empty();
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
                std::get<std::string>(row.values[5].payload),
                std::get<double>(row.values[7].payload), false});
    }
  }

  void ScanSub(const std::function<void(const SecEdgarQ2SubInput &)> &consumer) override {
    for (const auto &row : sub) {
      consumer({std::get<std::string>(row.values[0].payload),
                std::get<std::string>(row.values[2].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[10].payload))});
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

  const auto run = [&](const QueryArguments &args) {
    Sink out;
    module.Execute(services, args, out);
    assert(out.ended);
    return out.rows;
  };
  const auto args = [](int64_t fy, const std::string &uom) {
    QueryArguments a;
    a.values = {Int(fy), String(uom)};
    return a;
  };
  const auto commit = [&](const MutationBatch &m) {
    coordinator.OnMutation(m);
    coordinator.PreCommit();
    coordinator.Commit();
  };

  // Other parameter values are answered from the same state.
  auto rows = run(args(2021, "pure"));
  assert(rows.size() == 1);
  assert(std::get<std::string>(rows[0].values[0].payload) == "OldCo");
  assert(run(args(2022, "USD")).empty());
  assert(run(args(1999, "pure")).empty());
  {
    QueryArguments nulls;
    nulls.values = {AuxValue::Null(AuxType::Int64), String("pure")};
    assert(run(nulls).empty());
  }

  // DELETE: the group of adsh-b disappears.
  MutationBatch del;
  del.table = 1;
  del.kind = MutationKind::Delete;
  del.old_rows = {Num("adsh-b", "Assets", 40.0)};
  services.num.erase(services.num.begin() + 3);
  commit(del);
  rows = run({});
  assert(rows.size() == 2);
  assert(std::get<std::string>(rows[0].values[0].payload) == "Alpha");
  assert(std::abs(std::get<double>(rows[0].values[2].payload) - 30.0) <= 1e-9);

  // UPDATE of sub.name (physical column 2).
  MutationBatch rename;
  rename.table = 2;
  rename.kind = MutationKind::Update;
  rename.updated_columns = {2};
  rename.old_rows = {Sub("adsh-a", "Alpha", 2022)};
  rename.new_rows = {Sub("adsh-a", "Zed", 2022)};
  services.sub[0] = rename.new_rows[0];
  commit(rename);
  rows = run({});
  assert(std::get<std::string>(rows[0].values[0].payload) == "Zed");

  // UPDATE of num.uom moves a row to another unit of measure.
  MutationBatch move;
  move.table = 1;
  move.kind = MutationKind::Update;
  move.updated_columns = {5};
  move.old_rows = {Num("adsh-a", "Liab", 5.0, "pure")};
  move.new_rows = {Num("adsh-a", "Liab", 5.0, "USD")};
  services.num[2] = move.new_rows[0];
  commit(move);
  assert(run({}).size() == 1);
  rows = run(args(2022, "USD"));
  assert(rows.size() == 1);
  assert(std::get<std::string>(rows[0].values[1].payload) == "Liab");

  // UPDATE of sub.fy moves a whole filing in / out of the result.
  MutationBatch refy;
  refy.table = 2;
  refy.kind = MutationKind::Update;
  refy.updated_columns = {10};
  refy.old_rows = {Sub("adsh-old", "OldCo", 2021)};
  refy.new_rows = {Sub("adsh-old", "OldCo", 2022)};
  services.sub[2] = refy.new_rows[0];
  commit(refy);
  rows = run({});
  assert(rows.size() == 2);
  assert(std::get<std::string>(rows[0].values[0].payload) == "OldCo");
  assert(std::abs(std::get<double>(rows[0].values[2].payload) - 100.0) <= 1e-9);
  assert(run(args(2021, "pure")).empty());

  // A mutation row in an unknown layout forces a rebuild and stays correct.
  MutationBatch odd;
  odd.table = 1;
  odd.kind = MutationKind::Insert;
  odd.new_rows = {{{String("adsh-b"), String("Assets"), Double(55.0)}}};
  services.num.push_back(Num("adsh-b", "Assets", 55.0));
  commit(odd);
  rows = run({});
  assert(std::abs(std::get<double>(rows[1].values[2].payload) - 55.0) <= 1e-9);

  return 0;
}
