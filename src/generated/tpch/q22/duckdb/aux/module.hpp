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

// ── Q22 layout ──────────────────────────────────────────────────────────────
struct TpchQ22Layout {
  static constexpr const char *kAccelerator = "tpch_q22_aux";
  static constexpr core::PartitionId kFull = 0;

  static constexpr std::array<core::PartitionId, 1> Partitions() {
    return {kFull};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ22Config {
  core::TableId orders_table = 0;
  core::TableId customer_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ22CustomerInput {
  int64_t custkey = 0;
  std::string phone;
  int64_t acctbal_raw = 0;
  int32_t acctbal_scale = 2;
};

struct TpchQ22OrderInput {
  int64_t custkey = 0;
};

class TpchQ22InitialScan {
public:
  virtual ~TpchQ22InitialScan() = default;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ22CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ22OrderInput &)> &) = 0;

  // Optional SQL pushdown for Build: orders per customer
  // (SELECT o_custkey, COUNT(*) ... GROUP BY o_custkey).
  virtual bool ScanOrderCounts(
      const std::function<void(int64_t custkey, int64_t count)> &consumer) {
    (void)consumer;
    return false;
  }
};

// ── State structures ────────────────────────────────────────────────────────
// Per-prefix distribution of zero-order positive-balance customers as sorted
// parallel arrays with suffix aggregates (always consistent: no dirty flag, a
// published aggregate is never mutated).
struct TpchQ22PrefixAggregate {
  std::vector<int64_t> keys;         // distinct balances, ascending
  std::vector<int64_t> counts;       // customers per balance
  std::vector<int64_t> suffix_count; // size n + 1
  std::vector<int64_t> suffix_sum;   // size n + 1

  TpchQ22PrefixAggregate() : suffix_count(1, 0), suffix_sum(1, 0) {}

  void AddBalance(int64_t balance) {
    const auto it = std::lower_bound(keys.begin(), keys.end(), balance);
    const std::size_t idx = static_cast<std::size_t>(it - keys.begin());
    if (it != keys.end() && *it == balance) {
      ++counts[idx];
    } else {
      keys.insert(it, balance);
      counts.insert(counts.begin() + static_cast<std::ptrdiff_t>(idx), 1);
      suffix_count.insert(suffix_count.begin() + static_cast<std::ptrdiff_t>(idx),
                          suffix_count[idx]);
      suffix_sum.insert(suffix_sum.begin() + static_cast<std::ptrdiff_t>(idx),
                        suffix_sum[idx]);
      // The new slot carries the old suffix; the loop below makes it exact.
    }
    for (std::size_t i = 0; i <= idx; ++i) {
      suffix_count[i] += 1;
      suffix_sum[i] += balance;
    }
  }

  bool RemoveBalance(int64_t balance) {
    const auto it = std::lower_bound(keys.begin(), keys.end(), balance);
    if (it == keys.end() || *it != balance)
      return false;
    const std::size_t idx = static_cast<std::size_t>(it - keys.begin());
    for (std::size_t i = 0; i <= idx; ++i) {
      suffix_count[i] -= 1;
      suffix_sum[i] -= balance;
    }
    if (--counts[idx] == 0) {
      keys.erase(it);
      counts.erase(counts.begin() + static_cast<std::ptrdiff_t>(idx));
      suffix_count.erase(suffix_count.begin() + static_cast<std::ptrdiff_t>(idx));
      suffix_sum.erase(suffix_sum.begin() + static_cast<std::ptrdiff_t>(idx));
    }
    return true;
  }

  // Strictly-greater-than lookup: {count, sum}. Read-only.
  std::pair<int64_t, int64_t> QueryGreaterThanConst(int64_t balance) const {
    if (keys.empty())
      return {0, 0};
    const auto it = std::upper_bound(keys.begin(), keys.end(), balance);
    const std::size_t idx = static_cast<std::size_t>(it - keys.begin());
    return {suffix_count[idx], suffix_sum[idx]};
  }
};

struct TpchQ22State {
  // Two-character phone prefix packed as (c0 << 8) | c1; 0 = no prefix.
  using Prefix = uint16_t;

  struct CustomerRecord {
    int64_t balance_raw = 0;
    int64_t order_count = 0;
    Prefix prefix = 0;
    bool present = false;
  };

  // Customers live in fixed-size pages shared between published versions: a
  // write clones only the touched page(s). Keys outside the dense range go to
  // a (rarely used) sparse map. A null page means "no customers".
  static constexpr std::size_t kPageSize = 2048;
  static constexpr int64_t kDenseLimit = int64_t{1} << 26;
  struct Page {
    std::vector<CustomerRecord> recs = std::vector<CustomerRecord>(kPageSize);
  };
  using SparseMap = std::unordered_map<int64_t, CustomerRecord>;
  std::vector<std::shared_ptr<Page>> pages;
  std::shared_ptr<SparseMap> sparse = std::make_shared<SparseMap>();

  const CustomerRecord *FindCustomer(int64_t custkey) const {
    if (custkey >= 0 && custkey < kDenseLimit) {
      const std::size_t page = static_cast<std::size_t>(custkey) / kPageSize;
      if (page < pages.size() && pages[page]) {
        const auto &rec = pages[page]->recs[static_cast<std::size_t>(custkey) % kPageSize];
        return rec.present ? &rec : nullptr;
      }
      return nullptr;
    }
    if (!sparse)
      return nullptr;
    const auto it = sparse->find(custkey);
    return it == sparse->end() || !it->second.present ? nullptr : &it->second;
  }

  // Per-prefix positive-balance totals (AVG numerator / denominator).
  struct PositiveTotals {
    int64_t sum = 0;
    int64_t count = 0;
  };
  std::unordered_map<Prefix, PositiveTotals> positive;

  // Zero-order positive-balance distributions; each shared between versions
  // until a write touches that prefix.
  std::unordered_map<Prefix, std::shared_ptr<const TpchQ22PrefixAggregate>>
      prefix_aggregates;

  // Orphan order counts: custkeys seen in orders but not in customer (shared
  // between versions, cloned on write).
  using OrphanMap = std::unordered_map<int64_t, int64_t>;
  std::shared_ptr<const OrphanMap> orphan_orders =
      std::make_shared<const OrphanMap>();
};

enum class TpchQ22ImageKind : uint8_t { FullState };

struct TpchQ22StateImage {
  TpchQ22ImageKind kind = TpchQ22ImageKind::FullState;
  std::shared_ptr<const TpchQ22State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ22OutputRow {
  std::string cntrycode;
  int64_t numcust = 0;
  int64_t totacctbal_raw = 0;
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ22TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();

  struct CustomerDelta {
    int64_t custkey = 0;
    std::string old_prefix;
    std::string new_prefix;
    int64_t old_balance = 0;
    int64_t new_balance = 0;
    int8_t sign = 0; // 1 insert, -1 delete, 0 update
  };

  struct OrderDelta {
    int64_t custkey = 0;
    int8_t sign = 0; // 1 insert, -1 delete
  };

  std::vector<CustomerDelta> customer_deltas;
  std::vector<OrderDelta> order_deltas;

private:
  bool pending = false;
  bool requires_rebuild = false;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ22Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ22Layout::kAccelerator;

  explicit TpchQ22Module(TpchQ22Config config,
                         TpchQ22InitialScan *initial_scan = nullptr)
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
  TpchQ22State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ22State> &state) const;
  void EmitResult(const TpchQ22State &state,
                  const std::vector<std::string> &country_codes,
                  core::AuxRowSink &sink) const;

  TpchQ22Config config;
  TpchQ22InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
