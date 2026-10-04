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

// ── Persistent sharded map ──────────────────────────────────────────────────
// Fixed shard count; each shard is an immutable-once-published hash map held
// by shared_ptr.  Copying the map copies only the shard-pointer table (cheap);
// a mutation copies just the touched shard (copy-on-write at shard granularity).
// A map that has been published (held through a const pointer) must never be
// mutated; copy it first (the copy owns none of the shards yet).
template <class K, class V, std::size_t kShards>
class TpchQ8ShardedMap {
  static_assert((kShards & (kShards - 1)) == 0, "power of two");
  static constexpr unsigned kShift = []() {
    unsigned lg = 0;
    while ((std::size_t(1) << lg) < kShards)
      ++lg;
    return 64u - lg;
  }();

public:
  using Shard = std::unordered_map<K, V>;
  TpchQ8ShardedMap() : shards_(kShards) {}
  TpchQ8ShardedMap(const TpchQ8ShardedMap &o)
      : shards_(o.shards_), size_(o.size_) {}
  TpchQ8ShardedMap &operator=(const TpchQ8ShardedMap &o) {
    if (this != &o) {
      shards_ = o.shards_;
      size_ = o.size_;
      owned_.clear();
      mut_.clear();
    }
    return *this;
  }
  TpchQ8ShardedMap(TpchQ8ShardedMap &&) = default;
  TpchQ8ShardedMap &operator=(TpchQ8ShardedMap &&) = default;

  const V *Find(const K &k) const {
    const auto &s = shards_[Index(k)];
    if (!s)
      return nullptr;
    const auto it = s->find(k);
    return it == s->end() ? nullptr : &it->second;
  }
  std::size_t Size() const { return size_; }

  template <class F> void ForEach(F &&f) const {
    for (const auto &s : shards_) {
      if (!s)
        continue;
      for (const auto &kv : *s)
        f(kv.first, kv.second);
    }
  }

  void Set(const K &k, V v) {
    auto &s = Mutable(Index(k));
    const auto r = s.try_emplace(k);
    if (r.second)
      ++size_;
    r.first->second = std::move(v);
  }
  bool Erase(const K &k) {
    if (!Find(k))
      return false;
    Mutable(Index(k)).erase(k);
    --size_;
    return true;
  }

private:
  static std::size_t Index(const K &k) {
    return static_cast<std::size_t>(
        (static_cast<uint64_t>(std::hash<K>{}(k)) * 0x9E3779B97F4A7C15ull) >>
        kShift);
  }
  Shard &Mutable(std::size_t i) {
    if (owned_.empty()) {
      owned_.assign(kShards, 0);
      mut_.assign(kShards, nullptr);
    }
    if (!owned_[i]) {
      std::shared_ptr<Shard> fresh = shards_[i]
                                         ? std::make_shared<Shard>(*shards_[i])
                                         : std::make_shared<Shard>();
      mut_[i] = fresh.get();
      shards_[i] = std::move(fresh);
      owned_[i] = 1;
    }
    return *mut_[i];
  }

  std::vector<std::shared_ptr<const Shard>> shards_;
  std::vector<uint8_t> owned_;
  std::vector<Shard *> mut_;
  std::size_t size_ = 0;
};

// ── State structures ────────────────────────────────────────────────────────
// The dimension dictionaries are much larger than the annual cube.  They live
// in a separate shared payload built from persistent sharded maps, so delta
// maintenance copies only the shards it touches.
struct TpchQ8Dimensions {
  std::vector<std::string> part_types;
  std::unordered_map<std::string, int32_t> part_type_to_index;
  std::unordered_map<std::string, int32_t> nation_name_to_key;
  std::unordered_map<int32_t, std::string> nation_region;
  std::unordered_map<std::string, std::vector<int32_t>> region_nations;

  // Reverse maps for maintenance (part value = index into part_types)
  TpchQ8ShardedMap<int64_t, int32_t, 2048> part_key_to_type;
  TpchQ8ShardedMap<int64_t, int32_t, 2048> suppkey_to_nation;
  TpchQ8ShardedMap<int64_t, int32_t, 2048> custkey_to_nation;

  struct OrderInfo {
    int64_t custkey = 0;
    int32_t orderdate_days = 0;
  };
  TpchQ8ShardedMap<int64_t, OrderInfo, 16384> order_info;

  // Largest key present when the state was last (re)built.  Dimension keys
  // above this bound are "fresh": no pre-existing lineitem can depend on them.
  int64_t part_build_max = 0;
  int64_t supp_build_max = 0;
  int64_t cust_build_max = 0;
};

struct TpchQ8CubeCell {
  int32_t year = 0;
  int32_t customer_nationkey = 0;
  int32_t supplier_nationkey = 0;
  int32_t part_type_index = 0;
  int64_t nation_volume_raw = 0;
  int64_t total_volume_raw = 0;
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
  struct VolumePair {
    int64_t nation_volume_raw = 0;
    int64_t total_volume_raw = 0;
  };

  int32_t nat_dim = 1;
  // index = part_type_index * 2 + (year - 1995); null page = all zero.
  std::vector<std::shared_ptr<const TpchQ8CubePage>> pages;
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
