#pragma once

#include <cstdint>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "tpch/common/duckdb_partitioned_scan.hpp"
#include "prims/dense_id_array.hpp"
#include "prims/flat_hash_map.hpp"
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

// Pre-aggregated returned revenue for the SQL-pushdown build path.
struct TpchQ10ReturnedRevenueInput {
  int32_t month_key = 0; // YYYYMM
  int64_t custkey = 0;
  int64_t revenue_raw = 0;
};

struct TpchQ10Customer {
  std::string name;
  int64_t acctbal_raw = 0;
  int32_t nationkey = 0;
  std::string address;
  std::string phone;
  std::string comment;
};

struct TpchQ10Order {
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
};

struct TpchQ10CustomerRank {
  int64_t acctbal_raw = 0;
  int32_t nationkey = 0;
  uint8_t present = 0;
};

class TpchQ10InitialScan : public PartitionedTableScan {
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

  virtual bool ScanReturnedCustomerRevenue(
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer) {
    (void)consumer;
    return false;
  }

  virtual bool ScanReturnedCustomerRevenueForMonths(
      const std::set<int32_t> &months,
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer) {
    if (months.empty())
      return true;
    return ScanReturnedCustomerRevenue(
        [&](const TpchQ10ReturnedRevenueInput &input) {
          if (months.count(input.month_key))
            consumer(input);
        });
  }

  virtual bool LookupOrders(
      const std::vector<int64_t> &orderkeys,
      std::unordered_map<int64_t, TpchQ10Order> &out) {
    (void)orderkeys;
    (void)out;
    return false;
  }

  // Which of `orderkeys` still have returned ('R') lineitems in the base table.
  // Used to confirm an order delete carries all of its lineitems (so the
  // lineitem deltas cancel the order's revenue). false = unsupported.
  virtual bool OrdersWithReturnedLineitems(
      const std::vector<int64_t> &orderkeys, std::set<int64_t> &out) {
    (void)orderkeys;
    (void)out;
    return false;
  }

};

using TpchQ10CustomerMap = std::unordered_map<int64_t, TpchQ10Customer>;

// Immutable (once published) customer side: dense rank array + result strings.
struct TpchQ10SideBase {
  core::DenseIdArray<TpchQ10CustomerRank> ranks;
  TpchQ10CustomerMap customers;

  void SetCustomerRank(int64_t custkey, int64_t acctbal_raw, int32_t nationkey) {
    TpchQ10CustomerRank rank;
    rank.acctbal_raw = acctbal_raw;
    rank.nationkey = nationkey;
    rank.present = 1;
    ranks.Set(custkey, rank);
  }
};

// Small copy-on-write overlay of customer changes on top of a shared base.
struct TpchQ10CustomerSlot {
  uint8_t present = 0;
  TpchQ10Customer customer;
};
using TpchQ10CustomerOverlay = std::unordered_map<int64_t, TpchQ10CustomerSlot>;

// ── Persistent chunked sorted layout ────────────────────────────────────────
// A sorted sequence of (key, val) entries stored as shared immutable chunks
// plus a small top-level index (first entry of each chunk). A commit copies
// only the chunks it touches and the (n / chunk) sized index; every other
// chunk stays shared with the published predecessor.
struct TpchQ10KV {
  int64_t key = 0;
  int64_t val = 0;
};
struct TpchQ10ByKey {
  bool operator()(const TpchQ10KV &a, const TpchQ10KV &b) const {
    return a.key < b.key;
  }
};
// Revenue descending, custkey ascending (the query's ORDER BY).
struct TpchQ10ByRank {
  bool operator()(const TpchQ10KV &a, const TpchQ10KV &b) const {
    return a.val != b.val ? a.val > b.val : a.key < b.key;
  }
};

template <class Less> class TpchQ10Chunked {
public:
  static constexpr std::size_t kTarget = 512;  // fill at build / split
  static constexpr std::size_t kMax = 1024;    // split above this

  struct Chunk {
    std::vector<TpchQ10KV> e;
  };
  std::vector<std::shared_ptr<const Chunk>> chunks;
  std::vector<TpchQ10KV> firsts;
  std::size_t count = 0;

  static TpchQ10Chunked FromSorted(std::vector<TpchQ10KV> &&sorted) {
    TpchQ10Chunked out;
    out.count = sorted.size();
    for (std::size_t i = 0; i < sorted.size(); i += kTarget) {
      const auto n = std::min(kTarget, sorted.size() - i);
      auto chunk = std::make_shared<Chunk>();
      chunk->e.assign(sorted.begin() + static_cast<std::ptrdiff_t>(i),
                      sorted.begin() + static_cast<std::ptrdiff_t>(i + n));
      out.firsts.push_back(chunk->e.front());
      out.chunks.push_back(std::move(chunk));
    }
    return out;
  }

  // Entry whose key equals `probe.key` under Less == ByKey; returns val or 0.
  int64_t FindVal(int64_t key) const {
    if (chunks.empty())
      return 0;
    const TpchQ10KV probe{key, 0};
    const Less less;
    auto it = std::upper_bound(firsts.begin(), firsts.end(), probe, less);
    const std::size_t c = it == firsts.begin()
                              ? 0
                              : static_cast<std::size_t>(it - firsts.begin()) - 1;
    const auto &e = chunks[c]->e;
    auto pos = std::lower_bound(e.begin(), e.end(), probe, less);
    return (pos != e.end() && pos->key == key) ? pos->val : 0;
  }

  template <class Fn> void ForEach(Fn &&fn) const {
    for (const auto &chunk : chunks)
      for (const auto &e : chunk->e)
        fn(e);
  }

  void Flatten(std::vector<TpchQ10KV> &out) const {
    out.clear();
    out.reserve(count);
    for (const auto &chunk : chunks)
      out.insert(out.end(), chunk->e.begin(), chunk->e.end());
  }

  // prev + removals - ... + inserts. `rem` / `ins` are sorted by Less; a
  // removal must match an existing entry exactly (key and val). Returns false
  // (leaving `out` untouched) if it does not, so the caller can rebuild.
  static bool Apply(const TpchQ10Chunked &prev,
                    const std::vector<TpchQ10KV> &rem,
                    const std::vector<TpchQ10KV> &ins, TpchQ10Chunked &out) {
    const Less less;
    TpchQ10Chunked next;
    next.count = prev.count + ins.size();
    if (rem.size() > prev.count)
      return false;
    next.count -= rem.size();
    if (prev.chunks.empty()) {
      if (!rem.empty())
        return false;
      std::vector<TpchQ10KV> copy = ins;
      out = FromSorted(std::move(copy));
      return true;
    }
    const std::size_t n = prev.chunks.size();
    next.chunks.reserve(n + 4);
    next.firsts.reserve(n + 4);
    std::size_t ri = 0, ii = 0;
    std::vector<TpchQ10KV> kept, merged;
    for (std::size_t c = 0; c < n; ++c) {
      std::size_t rj = ri, ij = ii;
      if (c + 1 == n) {
        rj = rem.size();
        ij = ins.size();
      } else {
        while (rj < rem.size() && less(rem[rj], prev.firsts[c + 1]))
          ++rj;
        while (ij < ins.size() && less(ins[ij], prev.firsts[c + 1]))
          ++ij;
      }
      if (rj == ri && ij == ii) {
        next.chunks.push_back(prev.chunks[c]);
        next.firsts.push_back(prev.firsts[c]);
        continue;
      }
      const auto &old = prev.chunks[c]->e;
      kept.clear();
      std::size_t p = ri;
      for (const auto &e : old) {
        if (p < rj && !less(e, rem[p]) && !less(rem[p], e)) {
          if (e.val != rem[p].val)
            return false;
          ++p;
        } else if (p < rj && less(rem[p], e)) {
          return false; // removal not present
        } else {
          kept.push_back(e);
        }
      }
      if (p != rj)
        return false;
      merged.resize(kept.size() + (ij - ii));
      std::merge(kept.begin(), kept.end(),
                 ins.begin() + static_cast<std::ptrdiff_t>(ii),
                 ins.begin() + static_cast<std::ptrdiff_t>(ij), merged.begin(),
                 less);
      ri = rj;
      ii = ij;
      if (merged.empty())
        continue;
      if (merged.size() <= kMax) {
        auto chunk = std::make_shared<Chunk>();
        chunk->e = merged;
        next.firsts.push_back(chunk->e.front());
        next.chunks.push_back(std::move(chunk));
      } else {
        for (std::size_t i = 0; i < merged.size(); i += kTarget) {
          const auto m = std::min(kTarget, merged.size() - i);
          auto chunk = std::make_shared<Chunk>();
          chunk->e.assign(merged.begin() + static_cast<std::ptrdiff_t>(i),
                          merged.begin() + static_cast<std::ptrdiff_t>(i + m));
          next.firsts.push_back(chunk->e.front());
          next.chunks.push_back(std::move(chunk));
        }
      }
    }
    out = std::move(next);
    return true;
  }
};

using TpchQ10KeyIndex = TpchQ10Chunked<TpchQ10ByKey>;
using TpchQ10RankIndex = TpchQ10Chunked<TpchQ10ByRank>;

// Customer side + order metadata (Side / Orders partitions).
struct TpchQ10State {
  std::shared_ptr<const TpchQ10SideBase> side_base =
      std::make_shared<const TpchQ10SideBase>();
  std::shared_ptr<const TpchQ10CustomerOverlay> customer_overlay; // nullable
  std::unordered_map<int32_t, std::string> nations;
  // Order metadata (write maintenance only): copy-on-write shards, so a commit
  // clones the shards it touches, not the whole map.
  core::ShardedCowHashMap<TpchQ10Order, 14> orders;

  bool HasCustomer(int64_t custkey) const { return RankOf(custkey).present != 0; }
  TpchQ10CustomerRank RankOf(int64_t custkey) const {
    if (customer_overlay) {
      const auto it = customer_overlay->find(custkey);
      if (it != customer_overlay->end()) {
        TpchQ10CustomerRank rank;
        if (it->second.present) {
          rank.acctbal_raw = it->second.customer.acctbal_raw;
          rank.nationkey = it->second.customer.nationkey;
          rank.present = 1;
        }
        return rank;
      }
    }
    return side_base->ranks.Get(custkey);
  }
  const TpchQ10Customer *CustomerOf(int64_t custkey) const {
    if (customer_overlay) {
      const auto it = customer_overlay->find(custkey);
      if (it != customer_overlay->end())
        return it->second.present ? &it->second.customer : nullptr;
    }
    const auto it = side_base->customers.find(custkey);
    return it == side_base->customers.end() ? nullptr : &it->second;
  }
};

// Everything BuildState produces (month tables are key-sorted, nonzero).
struct TpchQ10BuildData {
  TpchQ10State state;
  std::map<int32_t, std::vector<TpchQ10KV>> months;
};

// Partitions:
//   0 Side   customer dimensions + nations
//   2 Orders order metadata (optional)
//   4 Meta   month range [lo, hi] that has month / window images
//   kFirstMonth + YYYYMM          Month   custkey -> returned revenue (by key)
//   kFirstWindow + YYYYMM(start)  Window  3-month sum, ranked (rev desc, key)
enum class TpchQ10ImageKind : uint8_t { Side, Orders, Month, Window, Meta };

struct TpchQ10StateImage {
  TpchQ10ImageKind kind = TpchQ10ImageKind::Side;
  int32_t month = 0; // Month: month key; Window: start month key
  std::shared_ptr<const TpchQ10State> state; // Side / Orders
  TpchQ10KeyIndex by_key;                    // Month
  TpchQ10RankIndex ranked;                   // Window
  int32_t lo_month = 0, hi_month = -1;       // Meta (YYYYMM, inclusive)
};

struct TpchQ10CustomerDelta {
  int64_t custkey = 0;
  int8_t sign = 0; // +1 insert/update-new, -1 delete/update-old
  TpchQ10Customer customer;
};

struct TpchQ10LineitemDelta {
  int64_t orderkey = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
};

struct TpchQ10OrderDelta {
  int64_t orderkey = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  int8_t sign = 0; // +1 insert/update-new, -1 delete/update-old
};

int32_t TpchQ10MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ10MonthStartDays(int32_t month_key);
int32_t TpchQ10AddMonths(int32_t days_since_epoch, int32_t months);
int64_t TpchQ10RevenueRaw(double extended_price, double discount);

// TPC-H Q10 returns the top 20 customers by revenue.
constexpr std::size_t kTpchQ10ResultLimit = 20;

// First `limit` entries of a ranked window whose customer exists and whose
// revenue is positive: (custkey, revenue), in result order.
std::vector<std::pair<int64_t, int64_t>>
TpchQ10WindowTop(const TpchQ10State &side, const TpchQ10RankIndex &window,
                 std::size_t limit = kTpchQ10ResultLimit);
void TpchQ10EmitRows(const TpchQ10State &side,
                     const std::vector<std::pair<int64_t, int64_t>> &ranked,
                     core::AuxRowSink &sink);

class TpchQ10TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ10LineitemDelta> &Deltas() const;
  const std::vector<TpchQ10OrderDelta> &OrderDeltas() const;
  const std::vector<TpchQ10CustomerDelta> &CustomerDeltas() const;
  void MarkRebuild();
  void AddCustomerDelta(TpchQ10CustomerDelta delta);
  void AddDelta(TpchQ10LineitemDelta delta);
  void AddOrderDelta(TpchQ10OrderDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ10LineitemDelta> deltas;
  std::vector<TpchQ10OrderDelta> order_deltas;
  std::vector<TpchQ10CustomerDelta> customer_deltas;
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
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ10BuildData BuildState(core::AuxEngineServices &) const;
  void EmitResult(const TpchQ10BuildData &, int32_t start_days,
                  core::AuxRowSink &) const;

  TpchQ10Config config;
  TpchQ10InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
