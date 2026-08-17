#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "tpch/q13/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ13Config;
using accelerator_codegen::aux_state::query::TpchQ13InitialScan;
using accelerator_codegen::aux_state::query::TpchQ13Module;
using accelerator_codegen::aux_state::query::TpchQ13State;
using accelerator_codegen::aux_state::query::TpchQ13OutputRow;

namespace {

AuxValue Str(const std::string &s) { return AuxValue::Of(AuxType::String, s); }
AuxValue Int(int64_t v) { return AuxValue::Of(AuxType::Int64, v); }

AuxRow CustomerRow(int64_t key) { return {{Int(key)}}; }
AuxRow OrderRow(int64_t custkey, const std::string &comment) {
  return {{Int(custkey), Str(comment)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "tpch_q13_aux";
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
    return key.accelerator == "tpch_q13_aux" && valid
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

class Services final : public AuxEngineServices, public TpchQ13InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const accelerator_codegen::aux_state::query::TpchQ13CustomerInput &)> &consumer) override {
    for (const auto &row : customers)
      consumer({std::get<int64_t>(row.values[0].payload)});
  }
  void ScanOrders(
      const std::function<void(const accelerator_codegen::aux_state::query::TpchQ13OrderInput &)> &consumer) override {
    for (const auto &row : orders) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload)});
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  Store store;
  std::vector<AuxRow> customers;
  std::vector<AuxRow> orders;
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

std::vector<std::pair<int64_t, int64_t>> AsPairs(const Sink &sink) {
  std::vector<std::pair<int64_t, int64_t>> out;
  for (const auto &row : sink.rows) {
    out.emplace_back(std::get<int64_t>(row.values[0].payload),
                     std::get<int64_t>(row.values[1].payload));
  }
  return out;
}

} // namespace

int main() {
  Services services;
  services.customers.push_back(CustomerRow(1));
  services.customers.push_back(CustomerRow(2));
  services.customers.push_back(CustomerRow(3));

  // Customer 1 has one usual account and one normal order.
  services.orders.push_back(OrderRow(1, "usual account"));
  services.orders.push_back(OrderRow(1, "normal order"));
  // Customer 2 has only a normal order.
  services.orders.push_back(OrderRow(2, "normal order"));
  // Customer 3 has only an excluded usual account.
  services.orders.push_back(OrderRow(3, "usual account"));

  TpchQ13Module module(TpchQ13Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  // Hot path: '%usual%account%' should exclude usual account orders.
  Sink sink;
  module.Execute(services, QueryArguments{{Str("%usual%account%")}}, sink);
  assert(sink.ended);
  auto rows = AsPairs(sink);
  // Expected: c1 has 1 surviving order; c2 has 1; c3 has 0.
  assert(rows.size() == 2);
  assert((rows[0] == std::make_pair<int64_t, int64_t>(1, 2)));
  assert((rows[1] == std::make_pair<int64_t, int64_t>(0, 1)));

  // Fallback should agree for the same binding.
  Sink fallback_sink;
  module.ExecuteFallback(services, QueryArguments{{Str("%usual%account%")}}, fallback_sink);
  assert(fallback_sink.ended);
  assert(AsPairs(fallback_sink) == rows);

  // Mutation: update customer 3's excluded order to a normal order.
  MutationBatch update;
  update.table = 2;
  update.kind = MutationKind::Update;
  update.updated_columns = {1};
  update.old_rows = {OrderRow(3, "usual account")};
  update.new_rows = {OrderRow(3, "normal order")};
  services.orders.back() = update.new_rows.front();

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ13Module>(TpchQ13Config{1, 2}, &services));
  coordinator.AttachServices(services);
  coordinator.OnMutation(update);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink2;
  module.Execute(services, QueryArguments{{Str("%usual%account%")}}, sink2);
  assert(sink2.ended);
  auto rows2 = AsPairs(sink2);
  // Now all three customers have 1 surviving order.
  assert(rows2.size() == 1);
  assert((rows2[0] == std::make_pair<int64_t, int64_t>(1, 3)));

  // Broad pattern with no useful literal should still produce correct output
  // by falling back to the typed initial scan. '%' matches every comment, so
  // o_comment NOT LIKE '%' excludes all orders → every customer has c_count 0.
  Sink broad_sink;
  module.Execute(services, QueryArguments{{Str("%")}}, broad_sink);
  assert(broad_sink.ended);
  auto broad_rows = AsPairs(broad_sink);
  assert(broad_rows.size() == 1);
  assert((broad_rows[0] == std::make_pair<int64_t, int64_t>(0, 3)));

  return 0;
}
