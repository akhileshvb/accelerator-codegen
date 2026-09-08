#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

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
  virtual void ScanParts(const std::function<void(const TpchQ8PartInput &)> &) = 0;
  virtual void ScanSuppliers(const std::function<void(const TpchQ8SupplierInput &)> &) = 0;
  virtual void ScanCustomers(const std::function<void(const TpchQ8CustomerInput &)> &) = 0;
  virtual void ScanOrders(const std::function<void(const TpchQ8OrdersInput &)> &) = 0;
  virtual void ScanLineitems(const std::function<void(const TpchQ8LineitemInput &)> &) = 0;
  virtual void ScanNations(const std::function<void(const TpchQ8NationInput &)> &) = 0;
  virtual void ScanRegions(const std::function<void(const TpchQ8RegionInput &)> &) = 0;

  virtual bool LookupOrder(int64_t orderkey, TpchQ8OrdersInput &out) {
    (void)orderkey; (void)out; return false;
  }
  virtual bool LookupPart(int64_t partkey, TpchQ8PartInput &out) {
    (void)partkey; (void)out; return false;
  }
  virtual bool LookupSupplier(int64_t suppkey, TpchQ8SupplierInput &out) {
    (void)suppkey; (void)out; return false;
  }
  virtual bool LookupCustomer(int64_t custkey, TpchQ8CustomerInput &out) {
    (void)custkey; (void)out; return false;
  }
};

// ── Cube key ────────────────────────────────────────────────────────────────
struct TpchQ8CubeKey {
  int32_t year = 0;
  int32_t customer_nationkey = 0;
  int32_t supplier_nationkey = 0;
  int32_t part_type_index = 0;

  bool operator==(const TpchQ8CubeKey &other) const {
    return year == other.year &&
           customer_nationkey == other.customer_nationkey &&
           supplier_nationkey == other.supplier_nationkey &&
           part_type_index == other.part_type_index;
  }
};

struct TpchQ8CubeKeyHash {
  std::size_t operator()(const TpchQ8CubeKey &k) const {
    std::size_t h = static_cast<std::size_t>(k.year);
    h = h * 31 + static_cast<std::size_t>(k.customer_nationkey);
    h = h * 31 + static_cast<std::size_t>(k.supplier_nationkey);
    h = h * 31 + static_cast<std::size_t>(k.part_type_index);
    return h;
  }
};

// ── State structures ────────────────────────────────────────────────────────
// The dimension dictionaries are much larger than the annual cube (SF1
// lineitem feeds ~28K cube cells but the reverse order map holds millions of
// rows).  Keep them in a separate shared payload so delta maintenance can
// copy only the mutable cube while sharing the expensive dictionaries.
struct TpchQ8Dimensions {
  std::vector<std::string> part_types;
  std::unordered_map<std::string, int32_t> part_type_to_index;
  std::unordered_map<std::string, int32_t> nation_name_to_key;
  std::unordered_map<int32_t, std::string> nation_region;
  std::unordered_map<std::string, std::vector<int32_t>> region_nations;

  // Reverse maps for maintenance
  std::unordered_map<int64_t, std::string> part_key_to_type;
  std::unordered_map<int64_t, int32_t> suppkey_to_nation;
  std::unordered_map<int64_t, int32_t> custkey_to_nation;

  struct OrderInfo {
    int64_t custkey = 0;
    int32_t orderdate_days = 0;
  };
  std::unordered_map<int64_t, OrderInfo> order_info;
};

struct TpchQ8CubeCell {
  int32_t year = 0;
  int32_t customer_nationkey = 0;
  int32_t supplier_nationkey = 0;
  int32_t part_type_index = 0;
  int64_t nation_volume_raw = 0;
  int64_t total_volume_raw = 0;
};

struct TpchQ8State {
  struct VolumePair {
    int64_t nation_volume_raw = 0;
    int64_t total_volume_raw = 0;
  };

  std::vector<TpchQ8CubeCell> cube;  // sorted by (year, customer_nationkey, supplier_nationkey, part_type_index)
  std::shared_ptr<const TpchQ8Dimensions> dims;

  static constexpr int32_t kYear1995Start = 9131;
  static constexpr int32_t kYear1997Start = 9862;

  static int32_t YearFromDays(int32_t days_since_epoch) {
    if (days_since_epoch >= kYear1995Start && days_since_epoch < 9496) return 1995;
    if (days_since_epoch >= 9496 && days_since_epoch < kYear1997Start) return 1996;
    return 0;
  }

  static constexpr int32_t kYear1996Start_days() { return 9496; }
};

enum class TpchQ8ImageKind : uint8_t { AnnualCube };

struct TpchQ8StateImage {
  TpchQ8ImageKind kind = TpchQ8ImageKind::AnnualCube;
  int32_t year = 0;
  std::shared_ptr<const TpchQ8State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ8OutputRow {
  int64_t o_year = 0;
  double mkt_share = 0.0;
};

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ8LineitemDelta {
  int64_t partkey = 0;
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t volume_raw = 0;
  int8_t sign = 0;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ8AnnualCube(const TpchQ8State &state);
bool DecodeTpchQ8AnnualCube(const std::vector<uint8_t> &bytes,
                            TpchQ8State &state);

// ── Revenue ─────────────────────────────────────────────────────────────────
int64_t TpchQ8VolumeRaw(int64_t extendedprice_raw, int64_t discount_raw);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ8TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ8LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ8LineitemDelta delta);
  void AddOrderInfo(int64_t orderkey, int64_t custkey, int32_t orderdate_days);
  const std::unordered_map<int64_t, TpchQ8Dimensions::OrderInfo> &
  OrderInfoOverlay() const;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ8LineitemDelta> deltas;
  std::unordered_map<int64_t, TpchQ8Dimensions::OrderInfo> order_info_overlay;
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

private:
  TpchQ8State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ8State> &state) const;
  bool ResolveBindings(core::AuxEngineServices &services,
                       const core::QueryArguments &arguments,
                       std::string &nation_name, std::string &region_name,
                       std::string &part_type) const;
  void EmitResult(const TpchQ8State &state,
                  const std::string &nation_name,
                  const std::string &region_name,
                  const std::string &part_type,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;

  TpchQ8Config config;
  TpchQ8InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
