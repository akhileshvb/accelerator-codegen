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
#include "tpch/q15/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ15Config;
using accelerator_codegen::aux_state::query::TpchQ15InitialScan;
using accelerator_codegen::aux_state::query::TpchQ15LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ15Module;
using accelerator_codegen::aux_state::query::TpchQ15SupplierInput;
using accelerator_codegen::aux_state::query::TpchQ15DaysFromYmd;
using accelerator_codegen::aux_state::query::TpchQ15IsFirstOfMonth;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}

AuxRow Supplier(int64_t key, const std::string &name,
                const std::string &address, const std::string &phone) {
  return {{Int(key), String(name), String(address), String(phone)}};
}
AuxRow Lineitem(int64_t suppkey, int64_t price_raw, int64_t discount_raw,
                int32_t ship_days) {
  return {{Int(suppkey), Decimal(price_raw, 2), Decimal(discount_raw, 2),
           Date(ship_days)}};
}

// Helper: convert YYYY-MM-DD to epoch days
int32_t YmdDays(int y, int m, int d) {
  return TpchQ15DaysFromYmd(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q15";
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == "tpch_q15" && valid
               ? ValidityResult{ValidityCode::Valid, version}
               : ValidityResult{ValidityCode::Invalidated, 0};
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

class Services final : public AuxEngineServices, public TpchQ15InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const TpchQ15LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems) {
      const auto &price = std::get<ScaledDecimal>(row.values[1].payload);
      const auto &discount = std::get<ScaledDecimal>(row.values[2].payload);
      consumer({std::get<AuxDate>(row.values[3].payload).days_since_epoch,
                std::get<int64_t>(row.values[0].payload), price.value,
                price.scale, discount.value, discount.scale});
    }
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ15SupplierInput &)> &consumer) override {
    for (const auto &row : suppliers) {
      TpchQ15SupplierInput input;
      input.suppkey = std::get<int64_t>(row.values[0].payload);
      input.name = std::get<std::string>(row.values[1].payload);
      input.address = std::get<std::string>(row.values[2].payload);
      input.phone = std::get<std::string>(row.values[3].payload);
      consumer(input);
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> suppliers;
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
  // happy-path: two suppliers, one with higher revenue, verify correct winner
  // SQL: WHERE l_shipdate >= '1994-11-01' AND l_shipdate < '1995-02-01'
  const auto start = YmdDays(1994, 11, 1); // first of month
  assert(TpchQ15IsFirstOfMonth(start));

  Services services;
  services.suppliers.push_back(Supplier(1, "Supplier#1", "Addr1", "111-111"));
  services.suppliers.push_back(Supplier(2, "Supplier#2", "Addr2", "222-222"));

  // Lineitem: suppkey=1 shipdate within window, discount=0.05 revenue=10000*(100-5)=950000
  services.lineitems.push_back(Lineitem(1, 10000, 5, start + 10));
  // Lineitem: suppkey=2, discount=0.01 revenue=20000*(100-1)=1980000
  services.lineitems.push_back(Lineitem(2, 20000, 1, start + 20));

  TpchQ15Module module(TpchQ15Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  Sink sink;
  module.Execute(services, QueryArguments{{Date(start)}}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(!sink.rows[0].values[0].is_null);
  assert(std::get<int64_t>(sink.rows[0].values[0].payload) == 2);
  assert(std::get<std::string>(sink.rows[0].values[1].payload) == "Supplier#2");

  // Test: non-first-of-month falls back
  const auto non_first = YmdDays(1994, 11, 5);
  assert(!TpchQ15IsFirstOfMonth(non_first));
  Sink sink_fb;
  module.Execute(services, QueryArguments{{Date(non_first)}}, sink_fb);
  assert(sink_fb.ended);
  assert(sink_fb.rows.size() == 1);
  assert(std::get<int64_t>(sink_fb.rows[0].values[0].payload) == 2);

  // Test: fallback and hot path agree
  Sink sink_fb2;
  module.ExecuteFallback(services, QueryArguments{{Date(start)}}, sink_fb2);
  assert(sink_fb2.ended);
  assert(sink_fb2.rows.size() == sink.rows.size());
  assert(std::get<int64_t>(sink_fb2.rows[0].values[0].payload) ==
         std::get<int64_t>(sink.rows[0].values[0].payload));

  // Test: mutation path (insert lineitem changing winner)
  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ15Module>(TpchQ15Config{1, 2}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 1;
  update.kind = MutationKind::Insert;
  update.new_rows = {Lineitem(1, 50000, 2, start + 15)};
  services.lineitems.push_back(update.new_rows.front());
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink3;
  module.Execute(services, QueryArguments{{Date(start)}}, sink3);
  assert(sink3.ended);
  assert(sink3.rows.size() == 1);
  // Supplier 1 now wins (revenue = 9500 + 49000 = 58500 > 19800)
  assert(std::get<int64_t>(sink3.rows[0].values[0].payload) == 1);

  // Test: empty result when no lineitems
  Services empty_svc;
  empty_svc.suppliers.push_back(Supplier(1, "S1", "A1", "P1"));
  TpchQ15Module empty_mod(TpchQ15Config{1, 2}, &empty_svc);
  Builder empty_builder;
  empty_mod.Build(empty_svc, empty_builder);
  assert(empty_builder.finished);
  empty_svc.store.InstallBuild(std::move(empty_builder.images));

  Sink empty_sink;
  empty_mod.Execute(empty_svc, QueryArguments{{Date(start)}}, empty_sink);
  assert(empty_sink.ended);
  assert(empty_sink.rows.size() == 0);

  return 0;
}
