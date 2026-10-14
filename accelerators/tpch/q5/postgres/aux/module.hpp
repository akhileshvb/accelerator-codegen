#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/radix_range_tree.hpp"
#include "prims/any_key_cow_array.hpp"
#include "tpch/q5/layout.hpp"

namespace accelerator_codegen::aux_state::query {

// ---------------------------------------------------------------------------
// State layout.  Published as five images; a write batch copies directories /
// small maps and clones only the chunks it touches (never O(state)).
//
//   kOrders      per orderkey (core::AnyKeyCowArray, any int64 key):
//                  order_date   (4 B)  } the order's join tuple, present only
//                  order_nation (4 B)  } when it has a date and its customer
//                                        has a nation
//                8 B per key slot (TPC-H keys are ~25% dense => ~32 B/order:
//                4.8 GB at SF100, 48 GB at SF1000, vs ~86 B/order for the old
//                four unordered_maps: 13 GB / 130 GB).  Dense keys: 8 B/order.
//   kCustomers   custkey -> nationkey (4 B/slot)   ~0.06 GB SF100, 0.6 GB SF1000
//   kSuppliers   suppkey -> nationkey (4 B/slot)   negligible
//   kTree        per nationkey: two core::RadixRangeTree over the order date,
//                revenue (int128, scale 4) and joined lineitem count; ~0.2 MB per
//                nation, independent of SF.  Dates outside the tree window
//                (+-23000 years around 1970) live in a small ordered map.
//   kSide        nation / region dimension rows (tiny).
//
// Execute: region name -> nations (side), then one range query on the revenue
// and count tree of each such nation: O(nations x log D), independent of SF.
// Maintenance is incremental for every table the query reads:
//   region / nation      replayed on the side image (they only name buckets);
//   orders / lineitem    per order key; an order whose join tuple changes (RF1
//                        insert, RF2 delete, update) relocates all its
//                        qualifying lineitems, found by an index lookup;
//   customer / supplier  a changed nation relocates that customer's / supplier's
//                        lineitems, found by index lookups.
// Only a batch that mixes customer/supplier changes with orders/lineitem
// changes is rebuilt (rare; exact).
// ---------------------------------------------------------------------------
struct TpchQ5Parts {
  static constexpr core::PartitionId kSide = core::TpchQ5Layout::kSide;
  static constexpr core::PartitionId kTree = core::TpchQ5Layout::kTreeBase;
  static constexpr core::PartitionId kOrders = 1;
  static constexpr core::PartitionId kCustomers = 2;
  static constexpr core::PartitionId kSuppliers = 3;
};

struct TpchQ5Config {
  core::TableId region_table = 0;
  core::TableId nation_table = 0;
  core::TableId customer_table = 0;
  core::TableId supplier_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

// Scans deliver only rows that can matter: regions with a key and a name,
// nations with a key, customers / suppliers with key and nationkey, orders with
// key, custkey and orderdate, lineitems with orderkey, suppkey, price, discount
// (a NULL anywhere makes the join or SUM skip the row).
struct TpchQ5RegionInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ5NationInput {
  int32_t key = 0;
  std::string name;
  int32_t regionkey = 0;
  bool name_is_null = false;
  bool regionkey_is_null = false;
};

struct TpchQ5CustomerInput {
  int64_t custkey = 0;
  int32_t nationkey = 0;
};

struct TpchQ5SupplierInput {
  int64_t suppkey = 0;
  int32_t nationkey = 0;
};

struct TpchQ5OrderInput {
  int64_t orderkey = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
};

// Prices / discounts as scaled integers (cents: scale 2).
struct TpchQ5LineitemInput {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
};

class TpchQ5InitialScan {
public:
  virtual ~TpchQ5InitialScan() = default;
  virtual void ScanRegions(
      const std::function<void(const TpchQ5RegionInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ5NationInput &)> &) = 0;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ5CustomerInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ5SupplierInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ5OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ5LineitemInput &)> &) = 0;

  // Point lookups against the database as the transaction leaves it (they run
  // while maintenance is staged at pre-commit).  Backends override them with
  // index lookups; the defaults filter a full scan.
  virtual void LookupLineitemsByOrder(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const TpchQ5LineitemInput &)> &consumer) {
    const std::set<int64_t> wanted(orderkeys.begin(), orderkeys.end());
    if (wanted.empty())
      return;
    ScanLineitems([&](const TpchQ5LineitemInput &row) {
      if (wanted.count(row.orderkey))
        consumer(row);
    });
  }
  virtual void LookupLineitemsBySupplier(
      const std::vector<int64_t> &suppkeys,
      const std::function<void(const TpchQ5LineitemInput &)> &consumer) {
    const std::set<int64_t> wanted(suppkeys.begin(), suppkeys.end());
    if (wanted.empty())
      return;
    ScanLineitems([&](const TpchQ5LineitemInput &row) {
      if (wanted.count(row.suppkey))
        consumer(row);
    });
  }
  virtual void LookupOrdersByCustomer(
      const std::vector<int64_t> &custkeys,
      const std::function<void(const TpchQ5OrderInput &)> &consumer) {
    const std::set<int64_t> wanted(custkeys.begin(), custkeys.end());
    if (wanted.empty())
      return;
    ScanOrders([&](const TpchQ5OrderInput &row) {
      if (wanted.count(row.custkey))
        consumer(row);
    });
  }
};

using TpchQ5Money = __int128;  // revenue, scale 4

constexpr int32_t kTpchQ5NoNation = std::numeric_limits<int32_t>::min();
constexpr int32_t kTpchQ5NoDate = std::numeric_limits<int32_t>::min();

struct TpchQ5Nation {
  std::string name;
  bool name_is_null = false;
  int32_t regionkey = 0;
  bool has_region = false;
};

// Revenue and joined lineitem count of one nation, over order-date keys.
struct TpchQ5NationTrees {
  core::RadixRangeTree<TpchQ5Money> revenue;
  core::RadixRangeTree<int64_t> rows;
};

struct TpchQ5Bucket {
  TpchQ5Money revenue = 0;
  int64_t rows = 0;
};

// One struct holds every member; each image publishes the ones of its kind
// (the others stay empty), and a maintenance step works on one private copy.
struct TpchQ5State {
  // kSide
  std::unordered_map<int32_t, TpchQ5Nation> nations;
  std::unordered_map<int32_t, std::string> region_names;  // regions with a name
  // kTree: date key = days + kDayBias; (day, nation) -> bucket outside the window
  static constexpr int64_t kDayBias = int64_t{1} << 23;
  std::unordered_map<int32_t, TpchQ5NationTrees> trees;
  std::map<std::pair<int64_t, int32_t>, TpchQ5Bucket> outliers;
  // kOrders / kCustomers / kSuppliers
  core::AnyKeyCowArray<int32_t, 15> order_date{kTpchQ5NoDate};
  core::AnyKeyCowArray<int32_t, 15> order_nation{kTpchQ5NoNation};
  core::AnyKeyCowArray<int32_t, 15> cust_nation{kTpchQ5NoNation};
  core::AnyKeyCowArray<int32_t, 15> supp_nation{kTpchQ5NoNation};
};

enum class TpchQ5ImageKind : uint8_t {
  Side,
  Tree,
  Orders,
  Customers,
  Suppliers,
};

struct TpchQ5StateImage {
  TpchQ5ImageKind kind = TpchQ5ImageKind::Side;
  std::shared_ptr<const TpchQ5State> state;
};

class TpchQ5TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();

  // Qualifying lineitem insert (+1) / delete (-1); revenue at scale 4.
  struct LineitemDelta {
    int64_t orderkey = 0;
    int64_t suppkey = 0;
    TpchQ5Money revenue = 0;
    int8_t sign = 0;
  };
  // Order row insert / delete.  has_tuple is false for an inserted order that
  // can never join (NULL custkey or orderdate).
  struct OrderDelta {
    int64_t orderkey = 0;
    int64_t custkey = 0;
    int32_t orderdate_days = 0;
    bool has_tuple = true;
    int8_t sign = 0;
  };
  // Customer / supplier row; nationkey is kTpchQ5NoNation for NULL.
  struct KeyNationDelta {
    int64_t key = 0;
    int32_t nationkey = kTpchQ5NoNation;
    int8_t sign = 0;
  };
  struct NationDelta {
    int32_t key = 0;
    TpchQ5Nation nation;
    int8_t sign = 0;
  };
  struct RegionDelta {
    int32_t key = 0;
    std::string name;
    bool has_name = false;
    int8_t sign = 0;
  };

  void Add(const LineitemDelta &d);
  void Add(const OrderDelta &d);
  void AddCustomer(const KeyNationDelta &d);
  void AddSupplier(const KeyNationDelta &d);
  void Add(const NationDelta &d);
  void Add(const RegionDelta &d);

  const std::vector<LineitemDelta> &Lineitems() const { return lineitems; }
  const std::vector<OrderDelta> &Orders() const { return orders; }
  const std::vector<KeyNationDelta> &Customers() const { return customers; }
  const std::vector<KeyNationDelta> &Suppliers() const { return suppliers; }
  const std::vector<NationDelta> &Nations() const { return nations; }
  const std::vector<RegionDelta> &Regions() const { return regions; }

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<LineitemDelta> lineitems;
  std::vector<OrderDelta> orders;
  std::vector<KeyNationDelta> customers, suppliers;
  std::vector<NationDelta> nations;
  std::vector<RegionDelta> regions;
};

int32_t TpchQ5DaysFromYmd(int32_t year, unsigned month, unsigned day);
int64_t TpchQ5AddMonths(int64_t days_since_epoch, int64_t months);
// extendedprice * (1 - discount) at scale 4, from scale-2 raw values.
TpchQ5Money TpchQ5Revenue(int64_t extended_price_raw, int64_t discount_raw);

// One result group (n_name); revenue at scale 4.
struct TpchQ5Group {
  std::string name;
  bool name_is_null = false;
  TpchQ5Money revenue = 0;
};
void TpchQ5FinalizeAndEmit(std::vector<TpchQ5Group> groups,
                           core::AuxRowSink &sink);

class TpchQ5Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ5Layout::kAccelerator;

  explicit TpchQ5Module(TpchQ5Config config,
                        TpchQ5InitialScan *initial_scan = nullptr)
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
  TpchQ5State BuildState(core::AuxEngineServices &) const;
  TpchQ5Config config;
  TpchQ5InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
