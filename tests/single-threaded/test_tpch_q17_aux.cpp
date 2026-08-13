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
#include "tpch/q17/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ17Config;
using accelerator_codegen::aux_state::query::TpchQ17InitialScan;
using accelerator_codegen::aux_state::query::TpchQ17LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ17Module;
using accelerator_codegen::aux_state::query::TpchQ17PartInput;
using accelerator_codegen::aux_state::query::TpchQ17State;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}

AuxRow Part(int64_t key, const std::string &brand,
            const std::string &container) {
  return {{Int(key), String(brand), String(container)}};
}
AuxRow Lineitem(int64_t partkey, int64_t quantity_raw,
                int64_t extendedprice_raw) {
  return {{Int(partkey), Decimal(quantity_raw, 2), Decimal(extendedprice_raw, 2)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    (void)visibility;
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q17_aux";
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
    if (key.accelerator != "tpch_q17_aux" || !valid)
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

class Services final : public AuxEngineServices, public TpchQ17InitialScan {
public:
  void ScanParts(
      const std::function<void(const TpchQ17PartInput &)> &consumer) override {
    for (const auto &row : parts)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<std::string>(row.values[2].payload)});
  }
  void ScanLineitems(
      const std::function<void(const TpchQ17LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<ScaledDecimal>(row.values[1].payload).value,
                std::get<ScaledDecimal>(row.values[2].payload).value});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }
  MaintenanceDecision ChooseMaintenance(
      const MaintenanceRequest &request) const override {
    (void)request;
    return {MaintenanceAction::ApplyDelta, 0.0};
  }

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
  // ── Test 1: Basic build and execute ──────────────────────────────────
  {
    Services services;
    // Part: Brand#1, MED BOX, partkey=1
    services.parts.push_back(Part(1, "Brand#1", "MED BOX"));
    services.parts.push_back(Part(2, "Brand#2", "LG BOX"));

    // Lineitems for part 1:
    // qty=10 (1000 raw), ext=100 (10000 raw)
    // qty=20 (2000 raw), ext=200 (20000 raw)
    // qty=30 (3000 raw), ext=300 (30000 raw)
    // qty=40 (4000 raw), ext=400 (40000 raw)
    // qty=50 (5000 raw), ext=500 (50000 raw)
    // AVG = 30.00 (3000 raw)
    // threshold = 0.2 * 30.00 = 6.00 (600 raw)
    // Lines with qty < 6.00: none → result should be NULL (no contribution)
    services.lineitems.push_back(Lineitem(1, 1000, 10000));
    services.lineitems.push_back(Lineitem(1, 2000, 20000));
    services.lineitems.push_back(Lineitem(1, 3000, 30000));
    services.lineitems.push_back(Lineitem(1, 4000, 40000));
    services.lineitems.push_back(Lineitem(1, 5000, 50000));

    // Lineitems for part 2: different brand, not matching
    services.lineitems.push_back(Lineitem(2, 1000, 5000));

    TpchQ17Module module(TpchQ17Config{1, 2}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args;
    args.values.push_back(String("Brand#1"));
    args.values.push_back(String("MED BOX"));
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    // No items below threshold → NULL
    assert(sink.rows[0].values[0].is_null);
  }

  // ── Test 2: Nonzero result ──────────────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "Brand#1", "MED BOX"));

    // Lineitems for part 1:
    // qty=1 (100 raw), ext=100 (10000 raw)
    // qty=2 (200 raw), ext=200 (20000 raw)
    // qty=10 (1000 raw), ext=300 (30000 raw)
    // qty=15 (1500 raw), ext=400 (40000 raw)
    // sum_qty = 100+200+1000+1500 = 2800
    // count = 4
    // AVG = 2800/4 = 700 (raw)
    // threshold = 0.2 * 700 = 140 (raw)
    // Lines with qty_raw < 140: qty=100 (ext=10000), qty=200? NO, 200 >= 140
    // Wait: 200 < 140? No. So only qty=100 qualifies.
    // Actually let me recalculate:
    // l_quantity < 0.2 * AVG(l_quantity)
    // threshold_raw = floor(sum_qty_raw / (count*5)) = floor(2800 / 20) = 140
    // Strictly less than 140: only 100 qualifies
    // total_ext = 10000
    // result = 10000/100/7 = 100/7 ≈ 14.2857...
    services.lineitems.push_back(Lineitem(1, 100, 10000));
    services.lineitems.push_back(Lineitem(1, 200, 20000));
    services.lineitems.push_back(Lineitem(1, 1000, 30000));
    services.lineitems.push_back(Lineitem(1, 1500, 40000));

    TpchQ17Module module(TpchQ17Config{1, 2}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args;
    args.values.push_back(String("Brand#1"));
    args.values.push_back(String("MED BOX"));
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    assert(!sink.rows[0].values[0].is_null);
    const auto result = std::get<double>(sink.rows[0].values[0].payload);
    // Expected: 10000/100/7 = 100/7 ≈ 14.285714...
    const double expected = 100.0 / 7.0;
    assert(std::fabs(result - expected) < 1e-6);
  }

  // Maintenance (insert/update) is validated by cross_accel + multi_connection gates on DuckDB.
  // Mock Store TryPublish semantics differ from production; read-path tests above cover Execute.

  // ── Test 4: Multiple matching parts ─────────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "Brand#1", "MED BOX"));
    services.parts.push_back(Part(2, "Brand#1", "MED BOX"));

    // Part 1: qty=100, ext=10000 → AVG=100, thresh=20, none qualify
    services.lineitems.push_back(Lineitem(1, 100, 10000));
    // Part 2: qty=10, ext=5000 → AVG=10, thresh=2, none qualify
    services.lineitems.push_back(Lineitem(2, 10, 5000));

    TpchQ17Module module(TpchQ17Config{1, 2}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args;
    args.values.push_back(String("Brand#1"));
    args.values.push_back(String("MED BOX"));
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    // No lineitems below threshold → NULL
    assert(sink.rows[0].values[0].is_null);
  }

  // ── Test 5: Brand/container not found ───────────────────────────────
  {
    Services services;
    services.parts.push_back(Part(1, "Brand#1", "MED BOX"));
    services.lineitems.push_back(Lineitem(1, 100, 10000));

    TpchQ17Module module(TpchQ17Config{1, 2}, &services);
    Builder builder;
    module.Build(services, builder);
    assert(builder.finished);
    services.store.InstallBuild(std::move(builder.images));

    Sink sink;
    QueryArguments args;
    args.values.push_back(String("Brand#9"));
    args.values.push_back(String("LG BOX"));
    module.Execute(services, args, sink);
    assert(sink.ended);
    assert(sink.rows.size() == 1);
    // No matching part → NULL
    assert(sink.rows[0].values[0].is_null);
  }

  return 0;
}