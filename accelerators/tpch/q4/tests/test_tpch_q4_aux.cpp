#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "tpch/q4/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ4Config;
using accelerator_codegen::aux_state::query::TpchQ4InitialScan;
using accelerator_codegen::aux_state::query::TpchQ4LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ4Module;
using accelerator_codegen::aux_state::query::TpchQ4OrderInput;
using accelerator_codegen::aux_state::query::TpchQ4DaysFromYmd;
using accelerator_codegen::aux_state::query::TpchQ4StateImage;
using accelerator_codegen::aux_state::query::TpchQ4ImageKind;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Date(int32_t days) {
  return AuxValue::Of(AuxType::Date, AuxDate{days});
}

AuxRow Order(int64_t key, int32_t days, const std::string &priority) {
  return {{Int(key), Date(days), String(priority)}};
}

AuxRow Lineitem(int64_t orderkey, int32_t commit_days, int32_t receipt_days) {
  return {{Int(orderkey), Date(commit_days), Date(receipt_days)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q4";
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &[partition, image] : images)
      snapshot.partitions.push_back(image);
    snapshot.available = !images.empty();
    return snapshot;
  }
  StateSnapshot ReadPartitions(const VisibilityToken &visibility,
                               const std::vector<PartitionId> &partitions) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q4";
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &[partition, image] : images) {
      if (std::find(partitions.begin(), partitions.end(), partition) !=
          partitions.end())
        snapshot.partitions.push_back(image);
    }
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == "tpch_q4" && valid
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

class Services final : public AuxEngineServices, public TpchQ4InitialScan {
public:
  void ScanOrders(
      const std::function<void(const TpchQ4OrderInput &)> &consumer) override {
    for (const auto &row : orders)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<AuxDate>(row.values[1].payload).days_since_epoch,
                std::get<std::string>(row.values[2].payload)});
  }
  void ScanLineitems(
      const std::function<void(const TpchQ4LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems)
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<AuxDate>(row.values[1].payload).days_since_epoch,
                std::get<AuxDate>(row.values[2].payload).days_since_epoch});
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> orders;
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
  void Begin(const OutputSchema &schema) override {
    assert(schema.columns.size() == 2);
    assert(schema.columns[0].name == "o_orderpriority");
    assert(schema.columns[1].name == "order_count");
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

}  // namespace

int main() {
  const int32_t start = TpchQ4DaysFromYmd(1995, 1, 1);
  const int32_t q_start = TpchQ4DaysFromYmd(1995, 1, 1);
  const int32_t in_window = start + 10;
  const int32_t outside_window = start - 10;

  Services services;
  // Orders:
  // - order 10: eligible and inside window (2 qualifying lineitems)
  // - order 11: has lineitem but commit >= receipt (excluded by EXISTS)
  // - order 12: no lineitems at all (excluded)
  // - order 13: eligible but outside date window (should not count)
  services.orders.push_back(Order(10, in_window, "1-URGENT"));
  services.orders.push_back(Order(11, in_window, "2-HIGH"));
  services.orders.push_back(Order(12, in_window, "3-MEDIUM"));
  services.orders.push_back(Order(13, outside_window, "1-URGENT"));

  // Lineitems:
  // - order 10: l_commitdate < l_receiptdate → qualifies
  services.lineitems.push_back(Lineitem(10, start, start + 1));
  services.lineitems.push_back(Lineitem(10, start, start + 2));
  // - order 11: l_commitdate >= l_receiptdate → NOT EXISTS, excluded
  services.lineitems.push_back(Lineitem(11, start + 1, start));
  // - order 13: qualifies but outside date range
  services.lineitems.push_back(Lineitem(13, start, start + 1));

  TpchQ4Module module(TpchQ4Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  Sink sink;
  module.Execute(services, QueryArguments{{Date(q_start)}}, sink);
  assert(sink.ended);
  assert(sink.rows.size() == 1);
  assert(std::get<std::string>(sink.rows[0].values[0].payload) == "1-URGENT");
  assert(std::get<int64_t>(sink.rows[0].values[1].payload) == 1);

  // Mutation: insert a qualifying lineitem for order 11, crossing eligibility
  // from excluded to included. The EXISTS predicate should now count it.
  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ4Module>(TpchQ4Config{1, 2}, &services));
  coordinator.AttachServices(services);

  MutationBatch update;
  update.table = 2;
  update.kind = MutationKind::Insert;
  update.new_rows = {Lineitem(11, start, start + 3)};
  services.lineitems.push_back(update.new_rows.front());
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink2;
  module.Execute(services, QueryArguments{{Date(q_start)}}, sink2);
  assert(sink2.ended);
  assert(sink2.rows.size() == 2);
  // Lexical order: 1-URGENT then 2-HIGH
  assert(std::get<std::string>(sink2.rows[0].values[0].payload) == "1-URGENT");
  assert(std::get<int64_t>(sink2.rows[0].values[1].payload) == 1);
  assert(std::get<std::string>(sink2.rows[1].values[0].payload) == "2-HIGH");
  assert(std::get<int64_t>(sink2.rows[1].values[1].payload) == 1);

  // Fallback agrees with fast path after mutation
  Sink fallback;
  module.ExecuteFallback(services, QueryArguments{{Date(q_start)}}, fallback);
  assert(fallback.ended);
  assert(fallback.rows.size() == 2);
  assert(std::get<std::string>(fallback.rows[0].values[0].payload) == "1-URGENT");
  assert(std::get<int64_t>(fallback.rows[0].values[1].payload) == 1);
  assert(std::get<std::string>(fallback.rows[1].values[0].payload) == "2-HIGH");
  assert(std::get<int64_t>(fallback.rows[1].values[1].payload) == 1);

  return 0;
}
