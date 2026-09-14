#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <cstdlib>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "tpch/q22/duckdb/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ22Config;
using accelerator_codegen::aux_state::query::TpchQ22CustomerInput;
using accelerator_codegen::aux_state::query::TpchQ22InitialScan;
using accelerator_codegen::aux_state::query::TpchQ22Layout;
using accelerator_codegen::aux_state::query::TpchQ22Module;
using accelerator_codegen::aux_state::query::TpchQ22OrderInput;
using accelerator_codegen::aux_state::query::TpchQ22State;
using accelerator_codegen::aux_state::query::TpchQ22StateImage;
using accelerator_codegen::aux_state::query::TpchQ22TxnBuffer;
using accelerator_codegen::aux_state::query::TpchQ22ImageKind;

namespace {

AuxValue String(std::string value) {
  return AuxValue::Of(AuxType::String, std::move(value));
}
AuxValue Int(int64_t value) { return AuxValue::Of(AuxType::Int64, value); }
AuxValue Decimal(int64_t raw, int32_t scale) {
  return AuxValue::Of(AuxType::ScaledDecimal, ScaledDecimal{raw, scale});
}

AuxRow Customer(int64_t custkey, const std::string &phone, int64_t acctbal_raw) {
  return {{Int(custkey), String(phone), Decimal(acctbal_raw, 2)}};
}

AuxRow Order(int64_t custkey) {
  return {{Int(custkey)}};
}

class Store final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = TpchQ22Layout::kAccelerator;
    snapshot.visibility = visibility;
    if (!valid)
      return snapshot;
    for (const auto &image : images)
      snapshot.partitions.push_back({image});
    snapshot.available = !images.empty();
    return snapshot;
  }
  StateSnapshot ReadPartitions(
      const VisibilityToken &visibility,
      const std::vector<PartitionId> &partitions) const override {
    auto snapshot = Read(visibility);
    snapshot.partitions.erase(
        std::remove_if(snapshot.partitions.begin(), snapshot.partitions.end(),
                       [&](const StateImage &image) {
                         return std::find(partitions.begin(), partitions.end(),
                                          image.partition) == partitions.end();
                       }),
        snapshot.partitions.end());
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }
  ValidityResult CheckValidity(const QueryKey &key,
                               const VisibilityToken &) const override {
    return key.accelerator == TpchQ22Layout::kAccelerator && valid
               ? ValidityResult{ValidityCode::Valid, version}
               : ValidityResult{ValidityCode::Invalidated, 0};
  }
  void Invalidate(const InvalidationPlan &) override { valid = false; }
  bool TryPublish(const StagedMaintenanceOutcome &outcome, Version) override {
    for (const auto &image : outcome.updates) {
      auto it = std::find_if(images.begin(), images.end(),
                             [&](const StateImage &existing) {
                               return existing.partition == image.partition;
                             });
      if (!valid || it == images.end() ||
          (image.version != 0 && image.version != it->version)) {
        valid = false;
        return false;
      }
    }
    for (const auto &image : outcome.updates) {
      auto it = std::find_if(images.begin(), images.end(),
                             [&](const StateImage &existing) {
                               return existing.partition == image.partition;
                             });
      if (it != images.end()) {
        *it = image;
        it->version++;
      } else {
        auto published = image;
        published.version = 1;
        images.push_back(published);
      }
    }
    valid = true;
    version++;
    return true;
  }
  void InstallBuild(std::vector<StateImage> built) {
    images.clear();
    for (auto &image : built) {
      image.version = 1;
      images.push_back(std::move(image));
    }
    valid = !images.empty();
    version = 1;
  }
  std::vector<StateImage> images;
  Version version = 0;
  bool valid = false;
};

class Services final : public AuxEngineServices, public TpchQ22InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const TpchQ22CustomerInput &)> &consumer)
      override {
    for (const auto &row : customers) {
      consumer({std::get<int64_t>(row.values[0].payload),
                std::get<std::string>(row.values[1].payload),
                std::get<ScaledDecimal>(row.values[2].payload).value,
                std::get<ScaledDecimal>(row.values[2].payload).scale});
    }
  }
  void ScanOrders(
      const std::function<void(const TpchQ22OrderInput &)> &consumer)
      override {
    for (const auto &row : orders) {
      consumer({std::get<int64_t>(row.values[0].payload)});
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

// Helper to extract a column value from result
int64_t GetInt(const AuxRow &row, std::size_t idx) {
  assert(idx < row.values.size());
  assert(!row.values[idx].is_null);
  assert(std::holds_alternative<int64_t>(row.values[idx].payload));
  return std::get<int64_t>(row.values[idx].payload);
}

std::string GetString(const AuxRow &row, std::size_t idx) {
  assert(idx < row.values.size());
  assert(!row.values[idx].is_null);
  assert(std::holds_alternative<std::string>(row.values[idx].payload));
  return std::get<std::string>(row.values[idx].payload);
}

int64_t GetDecimal(const AuxRow &row, std::size_t idx) {
  assert(idx < row.values.size());
  assert(!row.values[idx].is_null);
  assert(std::holds_alternative<ScaledDecimal>(row.values[idx].payload));
  return std::get<ScaledDecimal>(row.values[idx].payload).value;
}

} // namespace

int main() {
  // ── Test 1: Happy path ────────────────────────────────────────────────────
  // customer 1: phone 13-xxx, balance 1000, no orders → qualifies if > AVG
  // customer 2: phone 13-xxx, balance 500, no orders → does not qualify (below AVG)
  // customer 3: phone 30-xxx, balance 2000, no orders → qualifies  if > AVG
  // customer 4: phone 13-xxx, balance 5000, HAS order → excluded by NOT EXISTS
  // AVG of positive balances in (13,30): (1000 + 500 + 2000 + 5000) / 4 = 2125
  // Zero-order above 2125: none → 0 results

  Services services;
  services.customers.push_back(Customer(1, "13-111-111-1111", 100000));   // 1000.00
  services.customers.push_back(Customer(2, "13-222-222-2222", 50000));    // 500.00
  services.customers.push_back(Customer(3, "30-333-333-3333", 200000));   // 2000.00
  services.customers.push_back(Customer(4, "13-444-444-4444", 500000));   // 5000.00
  services.orders.push_back(Order(4));  // customer 4 has an order

  TpchQ22Module module(TpchQ22Config{1, 2}, &services);
  Builder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  // AVG = (1000+500+2000)/4 = 875 (wait, I need to recompute)
  // Actually: positive balances: 1000, 500, 2000, 5000 all > 0
  // AVG = 8500/4 = 2125
  // Zero-order above 2125: none
  // Actually wait: customer 4 has order so is excluded from zero-order set
  // Zero-order: customers 1(1000), 2(500), 3(2000)
  // AVG of all positive: (1000+500+2000+5000)/4 = 2125
  // acctbal > 2125 AND zero-order: none
  Sink sink;
  module.Execute(services, QueryArguments{{String("13"), String("30"), String("99"), String("98"),
                                            String("97"), String("96"), String("95")}}, sink);
  assert(sink.ended);
  assert(sink.rows.empty());

  // ── Test 2: Adjust to produce results ──────────────────────────────────────
  // Add customer 5: phone 13-xxx, balance 3000, no orders
  // AVG = (1000+500+2000+5000+3000)/5 = 2300
  // Zero-order above 2300: customer 3 (2000) — no, that's below
  // customer 5 (3000) — yes
  // customer 1 (1000) — no

  services.customers.push_back(Customer(5, "13-555-555-5555", 300000));  // 3000.00

  // Rebuild
  Builder builder2;
  module.Build(services, builder2);
  assert(builder2.finished);
  services.store.InstallBuild(std::move(builder2.images));

  Sink sink2;
  module.Execute(services, QueryArguments{{String("13"), String("30"), String("99"), String("98"),
                                            String("97"), String("96"), String("95")}}, sink2);
  assert(sink2.ended);
  // customer 5: prefix 13, balance 3000, zero-order, AVG=2300 → qualifies
  // customer 3: prefix 30, balance 2000, zero-order → below AVG → excluded
  // Expected: 1 row, cntrycode=13, numcust=1, totacctbal=3000.00
  assert(sink2.rows.size() == 1);
  assert(GetString(sink2.rows[0], 0) == "13");
  assert(GetInt(sink2.rows[0], 1) == 1);
  assert(GetDecimal(sink2.rows[0], 2) == 300000);

  // ── Test 3: Anti-join works ────────────────────────────────────────────────
  // customer 1 had no order → zero-order
  // Add an order for customer 5 → now customer 5 is excluded
  services.orders.push_back(Order(5));

  Builder builder3;
  module.Build(services, builder3);
  assert(builder3.finished);
  services.store.InstallBuild(std::move(builder3.images));

  Sink sink3;
  module.Execute(services, QueryArguments{{String("13"), String("30"), String("99"), String("98"),
                                            String("97"), String("96"), String("95")}}, sink3);
  assert(sink3.ended);
  // Only zero-order above 2300: none (c5 now has order)
  // customer 1: 1000 < 2300, customer 2: 500 < 2300
  // customer 3: 2000 < 2300
  assert(sink3.rows.empty());

  // ── Test 4: Incremental maintenance (insert customer) ──────────────────────
  // Reset to base state without customer 5's order, with customer 5
  services.orders.clear();
  services.orders.push_back(Order(4));

  // Rebuild
  Builder builder4;
  module.Build(services, builder4);
  assert(builder4.finished);
  services.store.InstallBuild(std::move(builder4.images));

  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ22Module>(TpchQ22Config{1, 2}, &services));
  coordinator.AttachServices(services);

  // Add a new customer with balance 4000, phone 30-xxx, no orders
  MutationBatch insert;
  insert.table = 2; // customer
  insert.kind = MutationKind::Insert;
  insert.new_rows = {Customer(6, "30-666-666-6666", 400000)}; // 4000.00
  services.customers.push_back(insert.new_rows.front());
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink4a;
  // AVG = (1000+500+2000+5000+3000+4000)/6 = 2583.33...
  // Zero-order above AVG: customer 5 (3000, cntry 13) and customer 6 (4000, cntry 30)
  module.Execute(services, QueryArguments{{String("13"), String("30"), String("99"), String("98"),
                                            String("97"), String("96"), String("95")}}, sink4a);
  assert(sink4a.ended);
  assert(sink4a.rows.size() == 2);
  assert(GetString(sink4a.rows[0], 0) == "13");
  assert(GetInt(sink4a.rows[0], 1) == 1);
  assert(GetDecimal(sink4a.rows[0], 2) == 300000);
  assert(GetString(sink4a.rows[1], 0) == "30");
  assert(GetInt(sink4a.rows[1], 1) == 1);
  assert(GetDecimal(sink4a.rows[1], 2) == 400000);

  // ── Test 5: Fallback agrees with hot path ──────────────────────────────────
  Sink sink5_fallback;
  module.ExecuteFallback(services, QueryArguments{{String("13"), String("30"), String("99"), String("98"),
                                                    String("97"), String("96"), String("95")}}, sink5_fallback);
  assert(sink5_fallback.ended);
  assert(sink5_fallback.rows.size() == sink4a.rows.size());
  assert(GetString(sink5_fallback.rows[0], 0) == GetString(sink4a.rows[0], 0));
  assert(GetInt(sink5_fallback.rows[0], 1) == GetInt(sink4a.rows[0], 1));
  assert(GetDecimal(sink5_fallback.rows[0], 2) == GetDecimal(sink4a.rows[0], 2));
  assert(GetString(sink5_fallback.rows[1], 0) == GetString(sink4a.rows[1], 0));
  assert(GetInt(sink5_fallback.rows[1], 1) == GetInt(sink4a.rows[1], 1));
  assert(GetDecimal(sink5_fallback.rows[1], 2) == GetDecimal(sink4a.rows[1], 2));

  // ── Test 6: Delete customer via maintenance ────────────────────────────────
  // Delete customer 5 → only customer 6 should remain above AVG (4000 > 2400)
  MutationBatch del;
  del.table = 2; // customer
  del.kind = MutationKind::Delete;
  del.old_rows = {Customer(5, "13-555-555-5555", 300000)};
  coordinator.OnMutation(del);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink6;
  // AVG = (1000+500+2000+5000+4000)/5 = 2500
  // threshold_raw = 2500
  // Customer 6: 30-xxx, 4000 > 2500, zero-order → qualifies
  // Customer 3: 30-xxx, 2000 < 2500 → excluded
  module.Execute(services, QueryArguments{{String("13"), String("30"), String("99"), String("98"),
                                            String("97"), String("96"), String("95")}}, sink6);
  assert(sink6.ended);
  assert(sink6.rows.size() == 1);
  assert(GetString(sink6.rows[0], 0) == "30");
  assert(GetInt(sink6.rows[0], 1) == 1);
  assert(GetDecimal(sink6.rows[0], 2) == 400000);

  // ── Test 7: Order insert removes customer from results ──────────────────────
  MutationBatch order_ins;
  order_ins.table = 1; // orders
  order_ins.kind = MutationKind::Insert;
  order_ins.new_rows = {Order(6)}; // Give customer 6 an order
  services.orders.push_back(order_ins.new_rows.front());
  coordinator.OnMutation(order_ins);
  coordinator.PreCommit();
  coordinator.Commit();

  Sink sink7;
  module.Execute(services, QueryArguments{{String("13"), String("30"), String("99"), String("98"),
                                            String("97"), String("96"), String("95")}}, sink7);
  assert(sink7.ended);
  assert(sink7.rows.empty());

  std::cout << "test_tpch_q22_aux passed\n";
  return 0;
}
