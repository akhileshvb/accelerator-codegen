#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/chunked_blob_map.hpp"
#include "prims/chunked_cow_array.hpp"
#include "prims/any_key_cow_array.hpp"
#include "prims/pg_numeric.hpp"
#include "prims/string_blob.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q8 layout ──────────────────────────────────────────────────────────────
struct TpchQ8Layout {
  static constexpr const char *kAccelerator = "tpch_q8_aux";
  static constexpr core::PartitionId kYear1995 = 0;
  static constexpr core::PartitionId kYear1996 = 1;

  static constexpr std::array<core::PartitionId, 2> Partitions() {
    return {kYear1995, kYear1996};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ8Config {
  core::TableId part_table = 0;
  core::TableId supplier_table = 0;
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
  core::TableId nation_table = 0;
  core::TableId region_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ8PartInput {
  int64_t key = 0;
  std::string type;
};

struct TpchQ8SupplierInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ8CustomerInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ8OrdersInput {
  int64_t key = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
};

struct TpchQ8LineitemInput {
  int64_t partkey = 0;
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t extendedprice_raw = 0;
  int64_t discount_raw = 0;
};

struct TpchQ8NationInput {
  int32_t key = 0;
  std::string name;
  int32_t regionkey = 0;
};

struct TpchQ8RegionInput {
  int32_t key = 0;
  std::string name;
};

class TpchQ8InitialScan {
public:
  virtual ~TpchQ8InitialScan() = default;
  // Partitioned scans of the big tables: sink(partition, row), one thread per
  // partition at a time.
  virtual void ScanPartsPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ8PartInput &)> &) = 0;
  virtual void ScanSuppliersPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ8SupplierInput &)> &) = 0;
  virtual void ScanCustomersPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ8CustomerInput &)> &) = 0;
  virtual void ScanOrdersPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ8OrdersInput &)> &) = 0;
  virtual void ScanLineitemsPartitioned(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ8LineitemInput &)> &) = 0;
  virtual void ScanNations(const std::function<void(const TpchQ8NationInput &)> &) = 0;
  virtual void ScanRegions(const std::function<void(const TpchQ8RegionInput &)> &) = 0;
};

// The years of the template (o_orderdate BETWEEN 1995-01-01 AND 1996-12-31)
// are constants of the query, not parameters.
constexpr int32_t kTpchQ8MinYear = 1995;
constexpr int32_t kTpchQ8MaxYear = 1996;

struct TpchQ8Nation {
  std::string name;
  int32_t regionkey = 0;
};

// One cube cell: the joined lineitems of one part type, grouped by order year
// and the customer / supplier nation KEYS (names and regions are applied at
// query time, so nation / region writes never touch the cube).
struct TpchQ8CubeCell {
  int32_t year = 0;
  int32_t customer_nationkey = 0;
  int32_t supplier_nationkey = 0;
  ::accelerator_codegen::pg_numeric::Int128 volume = 0; // SUM(volume), scale 4
  int64_t rows = 0; // joined lineitems (a group exists iff > 0)
};
// Sorted by (year, customer_nationkey, supplier_nationkey); at most
// 2 * #nations^2 cells (1250 for TPC-H).
using TpchQ8TypeCube = std::vector<TpchQ8CubeCell>;

// ---- State (immutable once published; copies share chunks) ----------------
//
// The cube answers any (nation, region, type) in O(#cells of one type),
// independent of the scale factor. Everything needed to keep it exact under
// writes to all seven tables is kept in compact columnar form:
//
//   part_type[partkey]        dictionary id of p_type (4 B/part)
//   part_refs / supplier_refs / customer_refs
//                             (4 B each) how many retained lineitems / orders
//                             reference the key: a dimension write that no
//                             retained row references needs no pass at all
//   supplier_nation, customer_nation
//                             nation key per supplier / customer (4 B each)
//   orders[orderkey]          ONE blob per order. In-window orders (o_orderdate
//                             in 1995-1996, ~30% of orders) hold
//                             [flags][custkey][date] + their lineitems
//                             (partkey, suppkey, volume: ~13 B each);
//                             other orders only a lineitem COUNT (the
//                             lineitems cannot contribute)
//   cubes[type id]            the cube (above)
//   nations, regions          small maps
// A lineitem / orders / supplier / customer / part write moves exactly the
// affected contributions between cube cells (a dimension write that touches
// referenced keys makes one pass over the retained orders: it is O(retained
// rows) and only needed for updates / deletes of referenced dimension rows).
// The single case that needs a rebuild is an order moving INTO the window
// while it already has lineitems: the lineitems of out-of-window orders are
// not retained.
//
// Memory: ~12 B/order + 13 B per in-window lineitem (30% of lineitems) + 20 B
// per part + 12 B per supplier/customer ~= 5.5 GB at SF100, 55 GB at SF1000
// (before: ~13 GB at SF100 + 2 GB transient, string per part).
// Write batch: O(touched chunks) (1 KB-40 KB each) + the small maps.
struct TpchQ8State {
  static constexpr int32_t kAbsent = INT32_MIN;
  static constexpr uint32_t kNoType = 0xFFFFFFFFu;

  std::map<int32_t, TpchQ8Nation> nations;
  std::map<int32_t, std::string> regions;
  std::shared_ptr<core::StringDict> types = std::make_shared<core::StringDict>();
  core::AnyKeyCowArray<uint32_t> part_type{kNoType};
  core::AnyKeyCowArray<uint32_t> part_refs{0};
  core::AnyKeyCowArray<int32_t> supplier_nation{kAbsent};
  core::AnyKeyCowArray<uint32_t> supplier_refs{0};
  core::AnyKeyCowArray<int32_t> customer_nation{kAbsent};
  core::AnyKeyCowArray<uint32_t> customer_refs{0};
  core::ChunkedBlobMap orders;
  core::ChunkedCowArray<std::shared_ptr<TpchQ8TypeCube>> cubes{nullptr};
};

struct TpchQ8StateImage {
  std::shared_ptr<const TpchQ8State> state;
};

class TpchQ8TxnBuffer final : public core::AuxTxnBuffer {
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
  std::shared_ptr<TpchQ8State> work;
  core::Version base_version = 0;
  uint64_t rows = 0;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ8Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ8Layout::kAccelerator;

  explicit TpchQ8Module(TpchQ8Config config,
                        TpchQ8InitialScan *initial_scan = nullptr)
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
  std::shared_ptr<TpchQ8State> BuildState(const core::AuxEngineServices &) const;
  void EmitResult(const TpchQ8State &, const core::QueryArguments &,
                  core::AuxRowSink &) const;

  TpchQ8Config config;
  TpchQ8InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
