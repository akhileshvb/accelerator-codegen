#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_blob_map.hpp"
#include "prims/any_key_cow_array.hpp"
#include "tpch/q10/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ10Config {
  core::TableId customer_table = 0;
  core::TableId nation_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

// Query-specific initial-scan contract.  Engine adapters implement this
// using their native column/vector APIs; no generic variant row crosses the
// bulk-scan boundary.
struct TpchQ10CustomerInput {
  int64_t key = 0;
  std::string name;
  int64_t acctbal_raw = 0;
  int32_t nationkey = 0;
  std::string address;
  std::string phone;
  std::string comment;
};

struct TpchQ10NationInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ10OrderInput {
  int64_t key = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
};

struct TpchQ10LineitemInput {
  int64_t orderkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  bool returned = false;
};

class TpchQ10InitialScan {
public:
  virtual ~TpchQ10InitialScan() = default;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ10CustomerInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ10NationInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ10OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ10LineitemInput &)> &) = 0;
};

// TPC-H Q10 returns the top 20 customers by revenue.
constexpr std::size_t kTpchQ10ResultLimit = 20;
// Revenue = price * (1 - discount): numeric(15,2) * numeric(15,2) -> scale 4.
constexpr int32_t kTpchQ10RevenueScale = 4;

// ---- State (immutable once published; copies share chunks) ----------------
//
// The result for ANY window [date, date + 3 months) is
//   top 20 customers by (revenue DESC, c_custkey) of the returned-lineitem
//   revenue of orders dated in the window, joined to customer and nation.
// The state is the revenue by (order date, customer), kept as a COW blob map
// bucketed so that one customer always lands in the same bucket:
//
//   revenue[day * buckets + custkey % buckets]
//       -> list of (custkey, #returned lineitems, revenue at scale 4)
//
// A window is then answered exactly, for any start date (month aligned or not),
// by merging the lists of the window's days bucket by bucket (the same
// customer's rows of all days are in the same bucket: a small local merge, no
// global hash over customers) and keeping the best 20. Whole-month sums are
// not needed: the work is proportional to the window's rows either way.
//
//   orders[orderkey]    [flags][custkey][orderdate] + the order's returned
//                       lineitem count and revenue sum, so an order update /
//                       delete moves its revenue exactly, whatever the order
//                       of the statements (lineitems may precede their order)
//   customers[custkey]  acctbal, nation key and the four result strings, so
//                       Execute answers without querying the database
//   customer_nation     nation key per customer (4 B), the filter at query time
//   nations             nation key -> name
//
// Memory: ~18 B per order (orders blob) + ~4 B per order (revenue lists; one
// row of ~12 B per customer-day with returned revenue) + ~140 B per customer
// = about 5.4 GB at SF100 and 54 GB at SF1000 (the old state: 6 GB customer
// map + 8 GB order map + month maps at SF100). A write batch copies O(touched
// chunks) (one chunk is ~1024 keys, 5-40 KB). Customer / nation writes never
// touch the revenue lists (the join with them is applied at query time).
struct TpchQ10State {
  static constexpr int32_t kAbsent = INT32_MIN;

  std::map<int32_t, std::string> nations;
  core::AnyKeyCowArray<int32_t> customer_nation{kAbsent};
  core::ChunkedBlobMap customers;
  core::ChunkedBlobMap orders;
  core::ChunkedBlobMap revenue;
  uint32_t buckets = 1; // power of two, fixed at build time
};

struct TpchQ10StateImage {
  std::shared_ptr<const TpchQ10State> state;
};

class TpchQ10TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override { return pending; }
  void Clear() override {
    pending = false;
    requires_rebuild = false;
    work.reset();
    rows = 0;
  }
  bool RequiresRebuild() const { return requires_rebuild; }
  void MarkRebuild() {
    pending = true;
    requires_rebuild = true;
    work.reset();
  }

  bool pending = false;
  bool requires_rebuild = false;
  // Private copy-on-write working state of this transaction (see Q7).
  std::shared_ptr<TpchQ10State> work;
  core::Version base_version = 0;
  uint64_t rows = 0;
};

class TpchQ10Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ10Layout::kAccelerator;

  explicit TpchQ10Module(TpchQ10Config config,
                         TpchQ10InitialScan *initial_scan = nullptr)
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
  std::shared_ptr<TpchQ10State> BuildState() const;
  void EmitResult(const TpchQ10State &, const core::QueryArguments &,
                  const core::AuxEngineServices &, core::AuxRowSink &) const;

  TpchQ10Config config;
  TpchQ10InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
