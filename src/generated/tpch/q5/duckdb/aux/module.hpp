#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "tpch/q5/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ5Config {
  core::TableId region_table = 0;
  core::TableId nation_table = 0;
  core::TableId customer_table = 0;
  core::TableId supplier_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ5RegionInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ5NationInput {
  int32_t key = 0;
  std::string name;
  int32_t regionkey = 0;
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

  // Point lookups of the current (post-write, same connection) lineitems of
  // one supplier / a set of orders. Return false when unsupported.
  virtual bool ScanLineitemsBySupplier(
      int64_t suppkey,
      const std::function<void(const TpchQ5LineitemInput &)> &consumer) {
    (void)suppkey;
    (void)consumer;
    return false;
  }
  virtual bool ScanLineitemsByOrder(
      int64_t orderkey,
      const std::function<void(const TpchQ5LineitemInput &)> &consumer) {
    (void)orderkey;
    (void)consumer;
    return false;
  }
};

// ---------------------------------------------------------------------------
// Persistent (copy-on-write) containers.  A published image is never mutated:
// writes copy the 256-entry top array, the touched 256-entry mid arrays and the
// touched leaves (a few hundred entries), everything else stays shared.
// ---------------------------------------------------------------------------
template <typename V> struct TpchQ5Entry {
  int64_t key = 0;
  V val{};
};

// 2^17 shards: 1024 top slots x 128 leaves per mid.
constexpr uint32_t kTpchQ5TopSlots = 1024;
constexpr uint32_t kTpchQ5MidLeaves = 128;
constexpr uint32_t kTpchQ5MidBits = 7;
constexpr uint32_t kTpchQ5ShardCount = kTpchQ5TopSlots * kTpchQ5MidLeaves;

inline uint32_t TpchQ5ShardOf(int64_t key) {
  const uint64_t h = static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ULL;
  return static_cast<uint32_t>(h >> 47);
}

template <typename V> struct TpchQ5ShardIndex {
  using Entry = TpchQ5Entry<V>;
  using Leaf = std::vector<Entry>;
  struct Mid {
    std::array<std::shared_ptr<const Leaf>, kTpchQ5MidLeaves> leaves;
  };
  std::array<std::shared_ptr<const Mid>, kTpchQ5TopSlots> mids;

  static const Entry *FindIn(const Leaf *leaf, int64_t key) {
    if (!leaf)
      return nullptr;
    auto it = std::lower_bound(
        leaf->begin(), leaf->end(), key,
        [](const Entry &e, int64_t k) { return e.key < k; });
    return (it != leaf->end() && it->key == key) ? &*it : nullptr;
  }
  const V *Find(int64_t key) const {
    const auto shard = TpchQ5ShardOf(key);
    const auto &mid = mids[shard >> kTpchQ5MidBits];
    if (!mid)
      return nullptr;
    const auto *e = FindIn(mid->leaves[shard & (kTpchQ5MidLeaves - 1)].get(), key);
    return e ? &e->val : nullptr;
  }
};

// Copy-on-write writer over a published index.
template <typename V> class TpchQ5ShardTxn {
public:
  using Index = TpchQ5ShardIndex<V>;
  using Entry = typename Index::Entry;
  using Leaf = typename Index::Leaf;
  using Mid = typename Index::Mid;

  explicit TpchQ5ShardTxn(const Index &base) : base_(base) {}

  const V *Find(int64_t key) const {
    return Lookup(TpchQ5ShardOf(key), key);
  }
  void Set(int64_t key, const V &val) {
    auto &leaf = Mutable(TpchQ5ShardOf(key));
    auto it = std::lower_bound(
        leaf.begin(), leaf.end(), key,
        [](const Entry &e, int64_t k) { return e.key < k; });
    if (it != leaf.end() && it->key == key)
      it->val = val;
    else
      leaf.insert(it, Entry{key, val});
  }
  void Erase(int64_t key) {
    const auto shard = TpchQ5ShardOf(key);
    if (!Lookup(shard, key))
      return;
    auto &leaf = Mutable(shard);
    auto it = std::lower_bound(
        leaf.begin(), leaf.end(), key,
        [](const Entry &e, int64_t k) { return e.key < k; });
    if (it != leaf.end() && it->key == key)
      leaf.erase(it);
  }
  bool Dirty() const { return !leaves_.empty(); }
  Index Finish() {
    Index out = base_;
    std::unordered_map<uint32_t, std::shared_ptr<Mid>> mids;
    mids.reserve(leaves_.size());
    for (auto &entry : leaves_) {
      const uint32_t hi = entry.first >> kTpchQ5MidBits;
      const uint32_t lo = entry.first & (kTpchQ5MidLeaves - 1);
      auto &mid = mids[hi];
      if (!mid)
        mid = base_.mids[hi] ? std::make_shared<Mid>(*base_.mids[hi])
                             : std::make_shared<Mid>();
      if (entry.second->empty())
        mid->leaves[lo] = nullptr;
      else
        mid->leaves[lo] = std::move(entry.second);
    }
    for (auto &entry : mids)
      out.mids[entry.first] = std::move(entry.second);
    leaves_.clear();
    return out;
  }

private:
  const Leaf *Current(uint32_t shard) const {
    auto it = leaves_.find(shard);
    if (it != leaves_.end())
      return it->second.get();
    const auto &mid = base_.mids[shard >> kTpchQ5MidBits];
    return mid ? mid->leaves[shard & (kTpchQ5MidLeaves - 1)].get() : nullptr;
  }
  const V *Lookup(uint32_t shard, int64_t key) const {
    const auto *e = Index::FindIn(Current(shard), key);
    return e ? &e->val : nullptr;
  }
  Leaf &Mutable(uint32_t shard) {
    auto it = leaves_.find(shard);
    if (it != leaves_.end())
      return *it->second;
    const auto *cur = Current(shard);
    auto copy = cur ? std::make_shared<Leaf>(*cur) : std::make_shared<Leaf>();
    return *leaves_.emplace(shard, std::move(copy)).first->second;
  }

  const Index &base_;
  std::unordered_map<uint32_t, std::shared_ptr<Leaf>> leaves_;
};

// Bulk builder (initial build): unsorted append, then Sort(), mutable Find(),
// Finish() moves the shards into an immutable index.
template <typename V> class TpchQ5ShardBuilder {
public:
  using Index = TpchQ5ShardIndex<V>;
  using Entry = typename Index::Entry;
  using Leaf = typename Index::Leaf;

  TpchQ5ShardBuilder() : shards_(kTpchQ5ShardCount) {}
  void Add(int64_t key, const V &val) {
    shards_[TpchQ5ShardOf(key)].push_back(Entry{key, val});
  }
  void Sort() {
    for (auto &shard : shards_)
      std::sort(shard.begin(), shard.end(),
                [](const Entry &a, const Entry &b) { return a.key < b.key; });
  }
  V *Find(int64_t key) {
    auto &shard = shards_[TpchQ5ShardOf(key)];
    auto it = std::lower_bound(
        shard.begin(), shard.end(), key,
        [](const Entry &e, int64_t k) { return e.key < k; });
    return (it != shard.end() && it->key == key) ? &it->val : nullptr;
  }
  template <typename F> void ForEach(F &&f) {
    for (auto &shard : shards_)
      for (auto &e : shard)
        f(e);
  }
  Index Finish() {
    Index index;
    for (uint32_t hi = 0; hi < kTpchQ5TopSlots; ++hi) {
      std::shared_ptr<typename Index::Mid> mid;
      for (uint32_t lo = 0; lo < kTpchQ5MidLeaves; ++lo) {
        auto &shard = shards_[(hi << kTpchQ5MidBits) | lo];
        if (shard.empty())
          continue;
        if (!mid)
          mid = std::make_shared<typename Index::Mid>();
        mid->leaves[lo] = std::make_shared<const Leaf>(std::move(shard));
      }
      index.mids[hi] = std::move(mid);
    }
    return index;
  }

private:
  std::vector<Leaf> shards_;
};

// Per-order record. Exists only for orders whose customer exists.
// `rev` is the revenue (scale 4) this order currently contributes to the tree
// (lineitems whose supplier nation equals the customer nation).
struct TpchQ5OrderRec {
  int32_t nationkey = 0;
  int32_t ordinal = 0;
  int64_t custkey = 0;
  int64_t rev = 0;
};
struct TpchQ5SuppRec {
  int32_t nationkey = 0;
  int32_t line_count = 0; // lineitems referencing this suppkey (post-state)
  // false: no supplier row exists, but lineitems reference the key (they
  // contribute nothing until the supplier is inserted).
  bool present = true;
};
struct TpchQ5CustRec {
  int32_t nationkey = 0;
  int32_t order_count = 0; // orders referencing this customer
};
using TpchQ5OrderIndex = TpchQ5ShardIndex<TpchQ5OrderRec>;
using TpchQ5SuppIndex = TpchQ5ShardIndex<TpchQ5SuppRec>;
using TpchQ5CustIndex = TpchQ5ShardIndex<TpchQ5CustRec>;

// Immutable dimension metadata (shared by every version until a rebuild).
struct TpchQ5Meta {
  int32_t min_orderdate_days = 0;
  int32_t max_orderdate_days = 0;
  uint32_t tree_size = 0;
  std::unordered_map<int32_t, std::string> nation_name;
  std::unordered_map<int32_t, int32_t> nation_region;
  std::unordered_map<std::string, int32_t> region_name_to_key;
  std::unordered_map<int32_t, std::vector<int32_t>> region_nations;
  std::vector<int32_t> nation_keys;                 // column -> nationkey
  std::vector<int32_t> column_region;               // column -> regionkey (-1 none)
  std::vector<std::string> column_name;             // column -> n_name
  std::unordered_map<int32_t, int32_t> nation_index; // nationkey -> column
  int32_t NationColumn(int32_t nationkey) const {
    auto it = nation_index.find(nationkey);
    return it == nation_index.end() ? -1 : it->second;
  }
};

// Segment tree of per-nation revenue, stored in pages of 32 nodes x stride
// int64 (stride = number of nations). Pages are immutable once published.
constexpr uint32_t kTpchQ5PageNodes = 32;
struct TpchQ5Tree {
  uint32_t tree_size = 0;
  uint32_t stride = 0;
  std::vector<std::shared_ptr<const std::vector<int64_t>>> pages;
  const int64_t *Node(uint32_t node) const {
    return pages[node / kTpchQ5PageNodes]->data() +
           static_cast<std::size_t>(node % kTpchQ5PageNodes) * stride;
  }
  bool Valid(uint32_t node) const {
    return node / kTpchQ5PageNodes < pages.size() && stride > 0;
  }
};

struct TpchQ5State {
  std::shared_ptr<const TpchQ5Meta> meta;
  TpchQ5OrderIndex orders;
  TpchQ5SuppIndex supps;
  TpchQ5CustIndex custs;
  TpchQ5Tree tree; // only populated by BuildState (moved into the tree image)
};

struct TpchQ5StateImage {
  bool is_side = false;
  uint32_t node_index = 0;
  std::shared_ptr<const TpchQ5State> state;
  std::shared_ptr<const TpchQ5Tree> tree;
};

class TpchQ5TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();

  struct LineitemDelta {
    TpchQ5LineitemInput old_row;
    TpchQ5LineitemInput new_row;
    bool is_delete = false;
    bool is_insert = false;
  };
  void AddLineitemMutation(const LineitemDelta &d);
  const std::vector<LineitemDelta> &LineitemMutations() const;
  bool HasLineitemMutations() const;

  struct OrderDelta {
    int64_t orderkey = 0;
    int64_t custkey = 0;
    int32_t orderdate_days = 0;
    int8_t sign = 0;
  };
  void AddOrderMutation(const OrderDelta &d);
  const std::vector<OrderDelta> &OrderMutations() const;

  // Customer / supplier changes (projected row: key, nationkey).
  struct DimDelta {
    enum class Kind : uint8_t { Insert, Delete, UpdateNation };
    Kind kind = Kind::Insert;
    int64_t key = 0;
    int32_t nationkey = 0;     // Insert/Delete: the row's nation; Update: new
    int32_t old_nationkey = 0; // UpdateNation only
  };
  void AddCustomerMutation(const DimDelta &d);
  void AddSupplierMutation(const DimDelta &d);
  const std::vector<DimDelta> &CustomerMutations() const;
  const std::vector<DimDelta> &SupplierMutations() const;
  // A mutation that provably does not change the state (keeps the buffer
  // "pending" so the coordinator does not escalate to a full invalidation).
  void MarkNoop();
  bool HasAnyDelta() const;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<LineitemDelta> lineitem_mutations;
  std::vector<OrderDelta> order_mutations;
  std::vector<DimDelta> customer_mutations;
  std::vector<DimDelta> supplier_mutations;
};

// Fast emit: `revenue[c]` per nation column (meta.column_*), zero columns and
// columns outside `target_regionkey` skipped; rows by revenue desc, name asc.
void TpchQ5FinalizeColumns(const TpchQ5Meta &meta, int32_t target_regionkey,
                           const int64_t *revenue, std::size_t columns,
                           core::AuxRowSink &sink);

int32_t TpchQ5DaysFromYmd(int32_t year, unsigned month, unsigned day);
int32_t TpchQ5AddMonths(int32_t days_since_epoch, int32_t months);
int64_t TpchQ5RevenueRaw(int64_t extended_price_raw, int64_t discount_raw);

void TpchQ5FinalizeAndEmit(
    const std::unordered_map<int32_t, std::string> &nation_name,
    const std::unordered_map<int32_t, int32_t> &nation_region,
    int32_t target_regionkey,
    const std::unordered_map<int32_t, int64_t> &revenue_by_nation,
    core::AuxRowSink &sink);

inline int32_t TpchQ5DateOrdinal(int32_t orderdate_days,
                                 int32_t min_orderdate_days) {
  return orderdate_days - min_orderdate_days;
}

inline uint32_t TpchQ5LeafNode(uint32_t tree_size, int32_t ordinal) {
  return tree_size + static_cast<uint32_t>(ordinal);
}

// Canonical cover of [start, end) into tree node indices.
std::vector<uint32_t> TpchQ5TreeCover(uint32_t tree_size, int32_t start_ordinal,
                                      int32_t end_ordinal);

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
  // One ReadPartitions for both partitions: a consistent (side, tree) pair.
  bool LoadSnapshot(core::AuxEngineServices &,
                    std::shared_ptr<const TpchQ5State> &side,
                    std::shared_ptr<const TpchQ5Tree> &tree) const;
  int32_t ResolveRegion(const std::string &region_name, const TpchQ5Meta &,
                        std::vector<int32_t> &eligible_nations) const;
  void EmitResult(core::AuxEngineServices &, const TpchQ5Meta &,
                  int32_t target_regionkey,
                  const std::unordered_map<int32_t, int64_t> &revenue_by_nation,
                  core::AuxRowSink &) const;
  core::StagedMaintenanceOutcome
  StageIncrementalMaintenance(core::AuxEngineServices &,
                              const core::InvalidationPlan &,
                              TpchQ5TxnBuffer &);

  TpchQ5Config config;
  TpchQ5InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
