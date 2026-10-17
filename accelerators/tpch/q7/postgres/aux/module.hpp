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
#include "prims/pg_numeric.hpp"
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
  // Partitioned scans of the big tables: sink(partition, row), one thread per
  // partition at a time.
  virtual void ScanSuppliersPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ7SupplierInput &)> &) = 0;
  virtual void ScanCustomersPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ7CustomerInput &)> &) = 0;
  virtual void ScanOrdersPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ7OrderInput &)> &) = 0;
  virtual void ScanLineitemsPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ7LineitemInput &)> &) = 0;
};

// The shipping years of the template (l_shipdate BETWEEN 1995-01-01 AND
// 1996-12-31) are constants of the query, not parameters.
constexpr int32_t kTpchQ7MinYear = 1995;
constexpr int32_t kTpchQ7MaxYear = 1996;

// Cube cell: result group (supplier nation key, customer nation key, year).
// Nation KEYS, not names: the nation table is applied at query time, so
// nation inserts / renames / deletes never touch the cube.
struct TpchQ7CubeKey {
  int32_t supp_nationkey = 0;
  int32_t cust_nationkey = 0;
  int32_t year = 0;
  bool operator<(const TpchQ7CubeKey &o) const {
    if (supp_nationkey != o.supp_nationkey)
      return supp_nationkey < o.supp_nationkey;
    if (cust_nationkey != o.cust_nationkey)
      return cust_nationkey < o.cust_nationkey;
    return year < o.year;
  }
};
struct TpchQ7CubeCell {
  ::accelerator_codegen::pg_numeric::Int128 revenue = 0; // SUM(volume) at scale 4, exact
  int64_t rows = 0;                  // joined lineitems (group exists iff > 0)
};

// ---- State (immutable once published; copies share chunks) ----------------
//
// Everything the result depends on, in compact columnar form:
//   supplier_nation[suppkey]  nation key (4 B/supplier)
//   customer_nation[custkey]  nation key (4 B/customer)
//   supplier_refs[suppkey]    in-window lineitems of the supplier (4 B)
//   customer_refs[custkey]    orders of the customer (4 B)
//   orders[orderkey]          ONE blob per order: [flags][custkey?][lineitem
//                             records of the order whose l_shipdate is in the
//                             template window]; ~1 B + 4 B offset + records
//                             (<= ~9 B each: suppkey|year, exact volume)
//   cube                      the answer, by nation keys (<= 25*25*2 cells)
//   nations                   nation key -> name
// The cube is the sum over records that currently join (supplier row, order
// row, customer row all present); every maintenance step keeps that invariant,
// so nation / supplier / customer / orders / lineitem writes are all exact and
// incremental. Memory (SF100 / SF1000, ~30% of lineitems are in the window,
// 1.5e8 / 1.5e9 orders): ~(5+1.2*9)=~16 B per order + 8 B per supplier/
// customer = about 2.5 GB / 25 GB; the old state was ~8 GB transient + maps.
// Per write batch: O(touched orders' chunks) (a chunk is ~10 KB), plus a copy
// of the 3 small directories (O(#directory pages)).
struct TpchQ7State {
  static constexpr int32_t kAbsent = INT32_MIN;

  std::map<int32_t, std::string> nations;
  core::AnyKeyCowArray<int32_t> supplier_nation{kAbsent};
  core::AnyKeyCowArray<int32_t> customer_nation{kAbsent};
  core::AnyKeyCowArray<uint32_t> supplier_refs{0};
  core::AnyKeyCowArray<uint32_t> customer_refs{0};
  core::ChunkedBlobMap orders;
  std::map<TpchQ7CubeKey, TpchQ7CubeCell> cube;
};

struct TpchQ7StateImage {
  std::shared_ptr<const TpchQ7State> state;
};

class TpchQ7TxnBuffer final : public core::AuxTxnBuffer {
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
  // Private copy-on-write working state, forked from the published state at
  // the first write of the transaction; later writes of the same transaction
  // apply on top of it in statement order (so they see each other).
  std::shared_ptr<TpchQ7State> work;
  core::Version base_version = 0;
  uint64_t rows = 0;
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
  std::shared_ptr<TpchQ7State> BuildState(const core::AuxEngineServices &) const;
  void EmitResult(const TpchQ7State &, const core::QueryArguments &,
                  core::AuxRowSink &) const;

  TpchQ7Config config;
  TpchQ7InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
