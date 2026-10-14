#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/sharded_sorted_map.hpp"
#include "tpch/q7/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ7Config {
  core::TableId nation_table = 0;
  core::TableId supplier_table = 0;
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ7NationInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ7SupplierInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ7CustomerInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ7OrderInput {
  int64_t key = 0;
  int64_t custkey = 0;
};

struct TpchQ7LineitemInput {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int32_t shipdate_days = 0;
};

class TpchQ7InitialScan {
public:
  virtual ~TpchQ7InitialScan() = default;
  virtual void ScanNations(
      const std::function<void(const TpchQ7NationInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ7SupplierInput &)> &) = 0;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ7CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ7OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ7LineitemInput &)> &) = 0;

  // Optional fast paths (engine pushes the join + GROUP BY into SQL). Return
  // false when unsupported; BuildState then falls back to ScanLineitems.
  // Row count of the orders table, or -1 if unknown (sizes the page table).
  virtual int64_t CountOrders() { return -1; }
  // revenue(supp_nationkey, cust_nationkey, year, sum of price*(100-disc_raw))
  // over lineitems in [1995-01-01,1996-12-31] joined to supplier/orders/
  // customer; supplier_counts(suppkey, #window lineitems referencing it);
  // customer_counts(custkey, #window lineitems whose order has that custkey).
  virtual bool ScanWindowAggregates(
      const std::function<void(int32_t, int32_t, int32_t, int64_t)> &,
      const std::function<void(int64_t, int64_t)> &,
      const std::function<void(int64_t, int64_t)> &) {
    return false;
  }
  // Point contribution queries for dimension deletes / nation changes whose
  // rows still have in-window lineitems. Rows: (other-side nationkey, year,
  // sum of price_raw*(100-disc_raw)) over the key's window lineitems joined to
  // orders and the opposite dimension, in the current (pre-commit) DB state.
  virtual bool ScanSupplierContribution(
      int64_t /*suppkey*/,
      const std::function<void(int32_t, int32_t, int64_t)> &) {
    return false;
  }
  virtual bool ScanCustomerContribution(
      int64_t /*custkey*/,
      const std::function<void(int32_t, int32_t, int64_t)> &) {
    return false;
  }
};

constexpr int32_t kTpchQ7MinNationSlots = 25;
constexpr int32_t kTpchQ7MinYear = 1995;
constexpr int32_t kTpchQ7MaxYear = 1996;

// Copy-on-write hash-sharded sorted map (core::ShardedSortedMap): sorted
// shards under a two-level directory; a Txn copies the directory, the touched
// mid arrays and the touched shards (a few hundred entries) and shares the
// rest.  Builder(expected) sizes the shard count from the row count.
template <class V> using TpchQ7Map = core::ShardedSortedKvMap<int64_t, V>;
// Find returns the {key, value} element; these yield the value (or nullptr).
template <class E> inline const auto *TpchQ7Val(const E *e) {
  return e ? &e->value : nullptr;
}
template <class E> inline auto *TpchQ7ValMut(E *e) {
  return e ? &e->value : nullptr;
}

// nation key plus the number of in-window (1995-1996) lineitems that reference
// the key. nation < 0 is a tombstone: the dimension row is absent but that many
// lineitems still reference the key (a re-insert must add their revenue back).
// A non-zero count makes delete / nation change fetch the key's contribution
// with one point SQL query.
struct TpchQ7NationCount {
  int32_t nation = 0;
  int32_t window_count = 0;
};
struct TpchQ7OrderCust {
  int64_t custkey = 0;
};

struct TpchQ7State {
  std::unordered_map<int32_t, std::string> nation_names;
  TpchQ7Map<TpchQ7NationCount> supplier_nations;
  TpchQ7Map<TpchQ7NationCount> customer_nations;
  // All orders (orderkey -> custkey), paged copy-on-write.
  TpchQ7Map<TpchQ7OrderCust> order_custkeys;
  std::vector<int64_t> revenue_1995;
  std::vector<int64_t> revenue_1996;
  int32_t num_nation_slots = kTpchQ7MinNationSlots;
};

enum class TpchQ7ImageKind : uint8_t { Side, Revenue };

struct TpchQ7StateImage {
  TpchQ7ImageKind kind = TpchQ7ImageKind::Side;
  std::shared_ptr<const TpchQ7State> state;
};

// One captured write, resolved against the published state at StageMaintenance.
struct TpchQ7Op {
  enum Type : uint8_t { Supplier, Customer, Order, Lineitem };
  Type type = Lineitem;
  // Dimension ops: 0 insert, 1 delete, 2 update. Lineitem: +1 / -1.
  int8_t kind = 0;
  // Dimension: (key,val) old = k0,v0; new = k1,v1 (val = nationkey / custkey).
  // Lineitem: k0 = orderkey, v0 = suppkey, k1 = shipdate_days, v1 = revenue raw.
  int64_t k0 = 0, v0 = 0, k1 = 0, v1 = 0;
};

std::vector<uint8_t> EncodeTpchQ7Side(const TpchQ7State &);
bool DecodeTpchQ7Side(const std::vector<uint8_t> &, TpchQ7State &);
std::vector<uint8_t> EncodeTpchQ7Revenue(const TpchQ7State &);
bool DecodeTpchQ7Revenue(const std::vector<uint8_t> &, TpchQ7State &);

int64_t TpchQ7RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                         int64_t discount_raw, int32_t discount_scale);
int32_t TpchQ7MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ7DaysFromYmd(int32_t year, unsigned month, unsigned day);

class TpchQ7TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ7Op> &Ops() const;
  void MarkRebuild();
  void AddOp(const TpchQ7Op &op);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ7Op> ops;
};

class TpchQ7Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ7Layout::kAccelerator;

  explicit TpchQ7Module(TpchQ7Config config,
                        TpchQ7InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}

  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override;
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override;
  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override;
  void Execute(core::AuxEngineServices &, const core::QueryArguments &,
               core::AuxRowSink &) override;
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ7State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &, const std::string &nation1,
                 const std::string &nation2,
                 std::shared_ptr<const TpchQ7State> &side,
                 std::vector<std::pair<int32_t, std::shared_ptr<const TpchQ7State>>> &months,
                 std::map<core::PartitionId, core::Version> *versions = nullptr) const;
  void EmitResult(const std::shared_ptr<const TpchQ7State> &side,
                  const std::shared_ptr<const TpchQ7State> &revenue,
                  const std::string &nation1, const std::string &nation2,
                  core::AuxRowSink &) const;

  TpchQ7Config config;
  TpchQ7InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
