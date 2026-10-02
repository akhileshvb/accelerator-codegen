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
};

// ── State structures ────────────────────────────────────────────────────────
struct TpchQ22State {
  struct CustomerRecord {
    std::string prefix;
    int64_t balance_raw = 0;
    int64_t order_count = 0;
  };

  // Per-prefix positive-balance aggregates for AVG computation
  std::unordered_map<std::string, int64_t> prefix_positive_sum;
  std::unordered_map<std::string, int64_t> prefix_positive_count;

  // custkey → customer record. Shared base + overlay so order-only RF
  // deltas do not copy the SF-scale customer table.
  using CustomerTable = std::unordered_map<int64_t, CustomerRecord>;
  std::shared_ptr<CustomerTable> customers = std::make_shared<CustomerTable>();
  CustomerTable customer_overlay;
  std::unordered_set<int64_t> customer_erased;

  const CustomerRecord *FindCustomer(int64_t custkey) const {
    if (customer_erased.find(custkey) != customer_erased.end())
      return nullptr;
    const auto overlay_it = customer_overlay.find(custkey);
    if (overlay_it != customer_overlay.end())
      return &overlay_it->second;
    if (!customers)
      return nullptr;
    const auto it = customers->find(custkey);
    return it == customers->end() ? nullptr : &it->second;
  }

  CustomerRecord &MutableCustomer(int64_t custkey) {
    customer_erased.erase(custkey);
    const auto overlay_it = customer_overlay.find(custkey);
    if (overlay_it != customer_overlay.end())
      return overlay_it->second;
    if (customers) {
      const auto it = customers->find(custkey);
      if (it != customers->end())
        return customer_overlay[custkey] = it->second;
    }
    return customer_overlay[custkey];
  }

  void EraseCustomer(int64_t custkey) {
    customer_overlay.erase(custkey);
    customer_erased.insert(custkey);
  }

  // Per-prefix distribution of zero-order positive-balance customers
  struct PrefixAggregate {
    // balance_counts maps a scaled integer balance to the number of
    // zero-order customers with that balance in this prefix.
    std::map<int64_t, int64_t> balance_counts;

    // Sorted arrays built from balance_counts before querying.
    std::vector<int64_t> keys;
    std::vector<int64_t> counts;
    std::vector<int64_t> suffix_count;
    std::vector<int64_t> suffix_sum;
    bool dirty = true;

    void RebuildSuffixes() {
      keys.clear();
      counts.clear();
      suffix_count.clear();
      suffix_sum.clear();
      keys.reserve(balance_counts.size());
      counts.reserve(balance_counts.size());
      for (const auto &entry : balance_counts) {
        keys.push_back(entry.first);
        counts.push_back(entry.second);
      }
      const size_t n = keys.size();
      suffix_count.assign(n + 1, 0);
      suffix_sum.assign(n + 1, 0);
      for (size_t i = n; i-- > 0;) {
        suffix_count[i] = suffix_count[i + 1] + counts[i];
        suffix_sum[i] = suffix_sum[i + 1] + keys[i] * counts[i];
      }
      dirty = false;
    }

    void MarkDirty() { dirty = true; }

    bool AddBalance(int64_t balance) {
      auto it = balance_counts.find(balance);
      if (it == balance_counts.end()) {
        balance_counts.emplace(balance, 1);
      } else {
        it->second++;
      }
      MarkDirty();
      return true;
    }

    bool RemoveBalance(int64_t balance) {
      auto it = balance_counts.find(balance);
      if (it == balance_counts.end() || it->second <= 0) {
        return false;
      }
      if (--it->second == 0) {
        balance_counts.erase(it);
      }
      MarkDirty();
      return true;
    }

    // Strictly-greater-than lookup. Returns {count,sum}.
    std::pair<int64_t, int64_t> QueryGreaterThan(int64_t balance) {
      if (dirty) {
        RebuildSuffixes();
      }
      if (keys.empty()) {
        return {0, 0};
      }
      // upper_bound gives the first key strictly greater than `balance`.
      auto it = std::upper_bound(keys.begin(), keys.end(), balance);
      const size_t idx = static_cast<size_t>(it - keys.begin());
      return {suffix_count[idx], suffix_sum[idx]};
    }
  };

  std::unordered_map<std::string, PrefixAggregate> prefix_aggregates;

  // Orphan order counts: custkeys seen in orders but not in customer yet
  std::unordered_map<int64_t, int64_t> orphan_orders;
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
