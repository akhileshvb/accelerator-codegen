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
#include "tpch/q16/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ16Config;
using accelerator_codegen::aux_state::query::TpchQ16InitialScan;
using accelerator_codegen::aux_state::query::TpchQ16Module;
using accelerator_codegen::aux_state::query::TpchQ16PartInput;
using accelerator_codegen::aux_state::query::TpchQ16PartsuppInput;
using accelerator_codegen::aux_state::query::TpchQ16State;
using accelerator_codegen::aux_state::query::TpchQ16SupplierInput;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }

AuxRow Part(int64_t key, const std::string &brand, const std::string &type,
            int32_t size) {
  return {{Int(key), String(brand), String(type), Int(size)}};
}
AuxRow Partsupp(int64_t partkey, int64_t suppkey) {
  return {{Int(partkey), Int(suppkey)}};
}
AuxRow Supplier(int64_t key, const std::string &comment) {
  return {{Int(key), String(comment)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    (void)visibility;
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q16_aux";
    snapshot.visibility = {};
    if (!valid)
      return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    if (key.accelerator != "tpch_q16_aux" || !valid)
      return {ValidityCode::Invalidated, 0};
    return {ValidityCode::Valid, version};
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

class Services final : public AuxEngineServices, public TpchQ16InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const TpchQ16SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
  }
  void ScanParts(
      const std::function<void(const TpchQ16PartInput &)> &consumer) override {
    for (const auto &row : parts)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload),
                static_cast<int32_t>(std::get<int64_t>(row.values[3].payload))});
  }
  void ScanPartsupp(
      const std::function<void(const TpchQ16PartsuppInput &)> &consumer) override {
    for (const auto &row : partsupps)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<int64_t>(row.values[1].payload)});
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }
  MaintenanceDecision ChooseMaintenance(
      const MaintenanceRequest &request) const override {
    (void)request;
    return {MaintenanceAction::ApplyDelta, 0.0};
  }

  Store store;
  std::vector<AuxRow> suppliers;
  std::vector<AuxRow> parts;
  std::vector<AuxRow> partsupps;
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

QueryArguments MakeArgs(const std::string &brand, const std::string &type,
                        const std::vector<int32_t> &sizes) {
  QueryArguments args;
  args.values.push_back(String(brand));
  args.values.push_back(String(type));
  for (const auto size : sizes)
    args.values.push_back(Int(size));
  return args;
}

} // namespace

int main() {
  // Args match TPC-H Q16: p_brand <> brand, p_type NOT LIKE type_pattern,
  // p_size IN (...), and exclude suppliers whose comment matches
  // '%Customer%Complaints%'.
  // ── Test 1: basic build + execute with eligible supplier ─────────────
  {
    Services services;
    services.suppliers.push_back(Supplier(1, "regular"));
    services.parts.push_back(Part(1, "Brand#1", "MED BOX", 10));
    services.parts.push_back(Part(2, "Brand#1", "MED BOX", 10));
    services.partsupps.push_back(Partsupp(1, 1));
    services.partsupps.push_back(Partsupp(2, 1));

    TpchQ16Module module(TpchQ16Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    // Exclude Brand#9; NOT LIKE 'SMALL%' keeps MED BOX.
    module.Execute(services, MakeArgs("Brand#9", "SMALL%", {10, 20}), sink);
    assert(sink.ended);
    // One group: Brand#1 / MED BOX / size 10 with 1 eligible supplier.
    assert(sink.rows.size() == 1);
    assert(std::get<std::string>(sink.rows[0].values[0].payload) == "Brand#1");
    assert(std::get<std::string>(sink.rows[0].values[1].payload) == "MED BOX");
    assert(std::get<int64_t>(sink.rows[0].values[2].payload) == 10);
    assert(std::get<int64_t>(sink.rows[0].values[3].payload) == 1);
  }

  // ── Test 2: anti-join excludes complaint supplier ────────────────────
  {
    Services services;
    services.suppliers.push_back(Supplier(1, "regular"));
    services.suppliers.push_back(Supplier(2, "Customer Complaints"));
    services.parts.push_back(Part(1, "Brand#1", "MED BOX", 10));
    services.partsupps.push_back(Partsupp(1, 1));
    services.partsupps.push_back(Partsupp(1, 2));

    TpchQ16Module module(TpchQ16Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, MakeArgs("Brand#9", "SMALL%", {10}), sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(std::get<int64_t>(sink.rows[0].values[3].payload) == 1);
  }

  // ── Test 3: brand exclusion ──────────────────────────────────────────
  {
    Services services;
    services.suppliers.push_back(Supplier(1, "regular"));
    services.parts.push_back(Part(1, "Brand#13", "MED BOX", 10));
    services.parts.push_back(Part(2, "Brand#14", "MED BOX", 10));
    services.partsupps.push_back(Partsupp(1, 1));
    services.partsupps.push_back(Partsupp(2, 1));

    TpchQ16Module module(TpchQ16Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    module.Execute(services, MakeArgs("Brand#13", "SMALL%", {10}), sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(std::get<std::string>(sink.rows[0].values[0].payload) == "Brand#14");
  }

  // ── Test 4: supplier comment mutation removes eligibility ────────────
  {
    Services services;
    services.suppliers.push_back(Supplier(1, "regular"));
    services.parts.push_back(Part(1, "Brand#1", "MED BOX", 10));
    services.partsupps.push_back(Partsupp(1, 1));

    TpchQ16Module module(TpchQ16Config{1, 2, 3}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    MutationBatch update;
    update.table = 3;
    update.kind = MutationKind::Update;
    update.updated_columns = {1};
    update.old_rows = {Supplier(1, "regular")};
    update.new_rows = {Supplier(1, "Customer Complaints")};

    auto buffer = module.CreateTxnBuffer();
    module.AccumulateMutation(update, services, *buffer);
    assert(buffer->HasPendingChanges());

    auto plan = module.InterpretMutation(update);
    auto outcome = module.StageMaintenance(services, plan, *buffer);
    assert(outcome.action == MaintenanceAction::RebuildAll);
    assert(!outcome.updates.empty());
    assert(services.store.TryPublish(outcome, 0));

    Sink sink;
    module.Execute(services, MakeArgs("Brand#9", "SMALL%", {10}), sink);
    assert(sink.ended);
    assert(sink.rows.empty());
  }

  return 0;
}
