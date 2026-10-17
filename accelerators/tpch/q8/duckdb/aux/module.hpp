#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tpch/common/duckdb_partitioned_scan.hpp"
#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/sharded_sorted_map.hpp"
#include "prims/sql_date.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ8Layout {
  static constexpr const char *kAccelerator = "tpch_q8_aux";
  static constexpr core::PartitionId kYear1995 = 0;
  static constexpr core::PartitionId kYear1996 = 1;

  static constexpr std::array<core::PartitionId, 2> Partitions() {
    return {kYear1995, kYear1996};
  }
};

struct TpchQ8Config {
  core::TableId part_table = 0;
  core::TableId supplier_table = 0;
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
  core::TableId nation_table = 0;
  core::TableId region_table = 0;
};

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

class TpchQ8InitialScan : public PartitionedTableScan {
public:
  virtual ~TpchQ8InitialScan() = default;
  virtual void ScanNationsSerial(const std::function<void(const TpchQ8NationInput &)> &) = 0;
  virtual void ScanRegionsSerial(const std::function<void(const TpchQ8RegionInput &)> &) = 0;

  virtual bool LookupOrder(int64_t, TpchQ8OrdersInput &) { return false; }
  virtual bool LookupPart(int64_t, TpchQ8PartInput &) { return false; }
  virtual bool LookupSupplier(int64_t, TpchQ8SupplierInput &) { return false; }
  virtual bool LookupCustomer(int64_t, TpchQ8CustomerInput &) { return false; }
  // Point scans of the CURRENT lineitem rows (transaction-visible) that join a
  // given part / supplier / customer (via orders).  Return false if unsupported.
  using LineitemConsumer = std::function<void(const TpchQ8LineitemInput &)>;
  virtual bool ScanLineitemsOfPart(int64_t, const LineitemConsumer &) { return false; }
  virtual bool ScanLineitemsOfSupplier(int64_t, const LineitemConsumer &) { return false; }
  virtual bool ScanLineitemsOfOrder(int64_t, const LineitemConsumer &) { return false; }
  virtual bool ScanLineitemsOfCustomer(int64_t, const LineitemConsumer &) { return false; }
  // The current lineitem rows of every order in `orderkeys`, in as few scans as
  // the source can manage (a refresh touches tens of thousands of orders, and
  // one query per order is what made the delta cost as much as a rebuild).
  // The default asks one order at a time.
  virtual bool ScanLineitemsOfOrders(const std::vector<int64_t> &orderkeys,
                                     const LineitemConsumer &consumer) {
    for (const int64_t orderkey : orderkeys)
      if (!ScanLineitemsOfOrder(orderkey, consumer))
        return false;
    return true;
  }
};

// core::ShardedSortedMap: 2^12 shards (64 x 64) for the small dictionaries and
// 2^16 (256 x 256) for the order table.  Copying a map copies the top pointers;
// a Txn clones the touched mid arrays and shards once per version.
template <class V, unsigned kBits, unsigned kMid>
using TpchQ8ShardedMap =
    core::ShardedSortedKvMap<int64_t, V, core::ShardedSortedMapHash<int64_t>,
                             kBits, kMid>;
// Find returns the {key, value} element; these yield the value (or nullptr).
template <class E> inline const auto *TpchQ8Val(const E *e) {
  return e ? &e->value : nullptr;
}
template <class E> inline auto *TpchQ8ValMut(E *e) {
  return e ? &e->value : nullptr;
}

// The dimension dictionaries are much larger than the annual cube.  They live
// in a separate shared payload built from persistent sharded maps, so delta
// maintenance copies only the shards it touches.
// Small dictionaries, shared between versions; cloned only when a new part
// type appears (never mutated once published).
struct TpchQ8Dict {
  std::vector<std::string> part_types;
  std::unordered_map<std::string, int32_t> part_type_to_index;
  std::unordered_map<std::string, int32_t> nation_name_to_key;
  std::unordered_map<int32_t, std::string> nation_region;
  std::unordered_map<std::string, std::vector<int32_t>> region_nations;
};

struct TpchQ8OrderInfo {
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  // Number of lineitem rows with this order key (exact), or -1 when not
  // tracked.  Lets a delete of an order whose lineitems all went in the same
  // transaction skip the point scan of lineitem.
  int32_t lines = -1;
};
using TpchQ8SmallMap = TpchQ8ShardedMap<int32_t, 12, 6>;
using TpchQ8OrderMap = TpchQ8ShardedMap<TpchQ8OrderInfo, 16, 8>;

struct TpchQ8Dimensions {
  std::shared_ptr<TpchQ8Dict> dict = std::make_shared<TpchQ8Dict>();

  // Reverse maps for maintenance (part value = index into part_types)
  TpchQ8SmallMap part_key_to_type;
  TpchQ8SmallMap suppkey_to_nation;
  TpchQ8SmallMap custkey_to_nation;

  using OrderInfo = TpchQ8OrderInfo;
  TpchQ8OrderMap order_info;

  // Largest key present when the state was last (re)built.  Dimension keys
  // above this bound are "fresh": no pre-existing lineitem can depend on them.
  int64_t part_build_max = 0;
  int64_t supp_build_max = 0;
  int64_t cust_build_max = 0;
  int64_t order_build_max = 0;
  // Order keys deleted while lineitems still referenced them; a re-insert of
  // such a key needs a point scan.
  std::unordered_set<int64_t> deleted_orders;
  // Fresh keys (> build max) that some lineitem delta has referenced: their
  // later change needs a point scan like an original key.
  std::unordered_set<int64_t> fresh_part_ref, fresh_supp_ref, fresh_cust_ref;
};

// One page per (part_type_index, year): a dense (customer nation x supplier
// nation) volume matrix plus a roll-up over supplier nation per customer
// nation.  Axis size is nat_dim + 1 (last slot = "other / negative key").
// (nation_volume == total_volume in every cell, so one matrix suffices.)
struct TpchQ8CubePage {
  std::vector<int64_t> cell;  // [customer_slot * axis + supplier_slot]
  std::vector<int64_t> roll;  // [customer_slot]
};

struct TpchQ8State {
  int32_t nat_dim = 1;
  // index = part_type_index * 2 + (year - 1995); null page = all zero.
  std::vector<std::shared_ptr<const TpchQ8CubePage>> pages;
  std::shared_ptr<const TpchQ8Dimensions> dims;

  static constexpr int32_t kYear1995Start = core::sql_date::DaysFromCivil(1995, 1, 1);
  static constexpr int32_t kYear1996Start = core::sql_date::DaysFromCivil(1996, 1, 1);
  static constexpr int32_t kYear1997Start = core::sql_date::DaysFromCivil(1997, 1, 1);

  // The order year, or 0 outside 1995-1996.
  static int32_t YearFromDays(int32_t days_since_epoch) {
    if (days_since_epoch >= kYear1995Start && days_since_epoch < kYear1996Start) return 1995;
    if (days_since_epoch >= kYear1996Start && days_since_epoch < kYear1997Start) return 1996;
    return 0;
  }
};

enum class TpchQ8ImageKind : uint8_t { AnnualCube };

struct TpchQ8StateImage {
  TpchQ8ImageKind kind = TpchQ8ImageKind::AnnualCube;
  int32_t year = 0;
  std::shared_ptr<const TpchQ8State> state;
};

struct TpchQ8OutputRow {
  int64_t o_year = 0;
  double mkt_share = 0.0;
};

struct TpchQ8LineitemDelta {
  int64_t partkey = 0;
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t volume_raw = 0;
  int8_t sign = 0;
};

// Net per-key change of a dimension row inside one transaction.
struct TpchQ8DimNet {
  bool present = false;
  int32_t nationkey = 0;  // supplier / customer
  std::string type;       // part
};

class TpchQ8TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ8LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ8LineitemDelta delta);
  void AddOrderInfo(int64_t orderkey, int64_t custkey, int32_t orderdate_days);
  void EraseOrder(int64_t orderkey);
  const std::unordered_map<int64_t, TpchQ8Dimensions::OrderInfo> &
  OrderInfoOverlay() const;
  const std::unordered_set<int64_t> &OrderErase() const;

  void SetPart(int64_t key, bool present, std::string type);
  void SetSupplier(int64_t key, bool present, int32_t nationkey);
  void SetCustomer(int64_t key, bool present, int32_t nationkey);
  const std::unordered_map<int64_t, TpchQ8DimNet> &PartNet() const;
  const std::unordered_map<int64_t, TpchQ8DimNet> &SupplierNet() const;
  const std::unordered_map<int64_t, TpchQ8DimNet> &CustomerNet() const;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ8LineitemDelta> deltas;
  std::unordered_map<int64_t, TpchQ8Dimensions::OrderInfo> order_info_overlay;
  std::unordered_set<int64_t> order_erase;
  std::unordered_map<int64_t, TpchQ8DimNet> part_net, supplier_net,
      customer_net;
};

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
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ8State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ8State> &state) const;
  bool ResolveBindings(const core::QueryArguments &arguments,
                       std::string &nation_name, std::string &region_name,
                       std::string &part_type) const;
  void EmitResult(const TpchQ8State &state,
                  const std::string &nation_name,
                  const std::string &region_name,
                  const std::string &part_type,
                  core::AuxRowSink &sink) const;

  TpchQ8Config config;
  TpchQ8InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
