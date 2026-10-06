#pragma once

#include <algorithm>
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
  // Point scans of the CURRENT lineitem rows (transaction-visible) that join a
  // given part / supplier / customer (via orders).  Return false if unsupported.
  using LineitemConsumer = std::function<void(const TpchQ8LineitemInput &)>;
  virtual bool ScanLineitemsOfPart(int64_t partkey, const LineitemConsumer &) {
    (void)partkey; return false;
  }
  virtual bool ScanLineitemsOfSupplier(int64_t suppkey,
                                       const LineitemConsumer &) {
    (void)suppkey; return false;
  }
  virtual bool ScanLineitemsOfOrder(int64_t orderkey,
                                    const LineitemConsumer &) {
    (void)orderkey; return false;
  }
  virtual bool ScanLineitemsOfCustomer(int64_t custkey,
                                       const LineitemConsumer &) {
    (void)custkey; return false;
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

// ── Persistent two-level sharded map ────────────────────────────────────────
// kTop x kMid immutable-once-published shards, each a key-sorted flat vector
// (binary-search lookups, memcpy clones).  The top array holds
// shared_ptr<const Mid>; a Mid holds shared_ptr<const Shard>.  Copying the map
// copies only the kTop top pointers (cheap); a mutation clones the touched Mid
// (kMid pointers) and the touched shard, each once per staging owner.  A map
// that has been published (held through a const pointer) must never be
// mutated; copy it first (the copy owns none of the parts yet).
template <class K, class V, std::size_t kTop, std::size_t kMid>
class TpchQ8ShardedMap {
  static_assert((kTop & (kTop - 1)) == 0 && (kMid & (kMid - 1)) == 0,
                "power of two");
  static constexpr std::size_t kShards = kTop * kMid;
  static constexpr unsigned kShift = []() {
    unsigned lg = 0;
    while ((std::size_t(1) << lg) < kShards)
      ++lg;
    return 64u - lg;
  }();
  static constexpr unsigned kMidBits = []() {
    unsigned lg = 0;
    while ((std::size_t(1) << lg) < kMid)
      ++lg;
    return lg;
  }();

public:
  struct Entry {
    K first;
    V second;
  };
  using Shard = std::vector<Entry>;
  struct Mid {
    std::array<std::shared_ptr<const Shard>, kMid> s;
    std::array<Shard *, kMid> own{};  // non-null: private to the staging owner
    Mid() = default;
    Mid(const Mid &o) : s(o.s) {}     // a copy owns nothing
  };
  TpchQ8ShardedMap() = default;
  TpchQ8ShardedMap(const TpchQ8ShardedMap &o) : top_(o.top_), size_(o.size_) {}
  TpchQ8ShardedMap &operator=(const TpchQ8ShardedMap &o) {
    if (this != &o) {
      top_ = o.top_;
      size_ = o.size_;
      own_mid_.fill(nullptr);
    }
    return *this;
  }
  TpchQ8ShardedMap(TpchQ8ShardedMap &&) = default;
  TpchQ8ShardedMap &operator=(TpchQ8ShardedMap &&) = default;

  const V *Find(const K &k) const {
    const std::size_t g = Index(k);
    const auto &m = top_[g >> kMidBits];
    if (!m)
      return nullptr;
    const auto &s = m->s[g & (kMid - 1)];
    if (!s)
      return nullptr;
    const auto it = Lower(*s, k);
    return (it != s->end() && it->first == k) ? &it->second : nullptr;
  }
  std::size_t Size() const { return size_; }

  template <class F> void ForEach(F &&f) const {
    for (const auto &m : top_) {
      if (!m)
        continue;
      for (const auto &s : m->s) {
        if (!s)
          continue;
        for (const auto &kv : *s)
          f(kv.first, kv.second);
      }
    }
  }

  void Set(const K &k, V v) {
    auto &s = Mutable(Index(k));
    if (s.empty() || s.back().first < k) {
      s.push_back({k, std::move(v)});
      ++size_;
      return;
    }
    const auto it = Lower(s, k);
    if (it != s.end() && it->first == k) {
      it->second = std::move(v);
      return;
    }
    s.insert(it, Entry{k, std::move(v)});
    ++size_;
  }
  // Mutable access to an existing value (nullptr if absent); clones the shard.
  V *FindMutable(const K &k) {
    if (!Find(k))
      return nullptr;
    auto &s = Mutable(Index(k));
    return &Lower(s, k)->second;
  }
  bool Erase(const K &k) {
    if (!Find(k))
      return false;
    auto &s = Mutable(Index(k));
    s.erase(Lower(s, k));
    --size_;
    return true;
  }

  // Bulk loading (Build only, before the map is published): Append adds an
  // entry without ordering it; Seal() sorts the touched shards (the last
  // value of a duplicated key wins).  The map must not be read in between.
  void Append(const K &k, V v) {
    Mutable(Index(k)).push_back({k, std::move(v)});
    ++size_;
  }
  void Seal() {
    size_ = 0;
    for (std::size_t hi = 0; hi < kTop; ++hi) {
      Mid *m = own_mid_[hi];
      if (!m)
        continue;
      for (std::size_t lo = 0; lo < kMid; ++lo) {
        Shard *s = m->own[lo];
        if (s) {
          std::stable_sort(s->begin(), s->end(),
                           [](const Entry &a, const Entry &b) {
                             return a.first < b.first;
                           });
          // Dedupe, keeping the last value of each key.
          std::size_t w = 0;
          for (std::size_t r = 0; r < s->size(); ++r) {
            if (w > 0 && (*s)[w - 1].first == (*s)[r].first)
              (*s)[w - 1] = (*s)[r];
            else
              (*s)[w++] = (*s)[r];
          }
          s->resize(w);
          s->shrink_to_fit();
        }
        if (m->s[lo])
          size_ += m->s[lo]->size();
      }
    }
    for (std::size_t hi = 0; hi < kTop; ++hi)
      if (!own_mid_[hi] && top_[hi])
        for (const auto &s : top_[hi]->s)
          if (s)
            size_ += s->size();
  }

private:
  static typename Shard::iterator Lower(Shard &s, const K &k) {
    return std::lower_bound(s.begin(), s.end(), k,
                            [](const Entry &e, const K &key) {
                              return e.first < key;
                            });
  }
  static typename Shard::const_iterator Lower(const Shard &s, const K &k) {
    return std::lower_bound(s.begin(), s.end(), k,
                            [](const Entry &e, const K &key) {
                              return e.first < key;
                            });
  }
  static std::size_t Index(const K &k) {
    return static_cast<std::size_t>(
        (static_cast<uint64_t>(std::hash<K>{}(k)) * 0x9E3779B97F4A7C15ull) >>
        kShift);
  }
  Shard &Mutable(std::size_t g) {
    const std::size_t hi = g >> kMidBits, lo = g & (kMid - 1);
    Mid *m = own_mid_[hi];
    if (!m) {
      std::shared_ptr<Mid> fresh = top_[hi] ? std::make_shared<Mid>(*top_[hi])
                                            : std::make_shared<Mid>();
      m = fresh.get();
      own_mid_[hi] = m;
      top_[hi] = std::move(fresh);
    }
    if (m->own[lo])
      return *m->own[lo];
    std::shared_ptr<Shard> fresh = m->s[lo]
                                       ? std::make_shared<Shard>(*m->s[lo])
                                       : std::make_shared<Shard>();
    m->own[lo] = fresh.get();
    m->s[lo] = std::move(fresh);
    return *m->own[lo];
  }

  std::array<std::shared_ptr<const Mid>, kTop> top_{};
  std::array<Mid *, kTop> own_mid_{};
  std::size_t size_ = 0;
};

// ── State structures ────────────────────────────────────────────────────────
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

struct TpchQ8Dimensions {
  std::shared_ptr<TpchQ8Dict> dict = std::make_shared<TpchQ8Dict>();

  // Reverse maps for maintenance (part value = index into part_types)
  TpchQ8ShardedMap<int64_t, int32_t, 64, 64> part_key_to_type;
  TpchQ8ShardedMap<int64_t, int32_t, 64, 64> suppkey_to_nation;
  TpchQ8ShardedMap<int64_t, int32_t, 64, 64> custkey_to_nation;

  struct OrderInfo {
    int64_t custkey = 0;
    int32_t orderdate_days = 0;
    // Number of lineitem rows with this order key (exact), or -1 when not
    // tracked.  Lets a delete of an order whose lineitems all went in the same
    // transaction skip the point scan of lineitem.
    int32_t lines = -1;
  };
  TpchQ8ShardedMap<int64_t, OrderInfo, 256, 256> order_info;

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
