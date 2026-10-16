#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/sorted_count_sum.hpp"
#include "prims/any_key_cow_array.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q22 layout ──────────────────────────────────────────────────────────────
// One published partition holds the whole state, built from copy-on-write
// pieces (a write batch copies pointers plus the pieces it touches):
//   * per custkey (columnar): present flag + phone-prefix id, account balance,
//     and the number of orders of that custkey (also for custkeys without a
//     customer row, so orders may arrive before their customer),
//   * per phone prefix: count and sum of the customers with c_acctbal > 0 (the
//     AVG subquery) and an ordered multiset of the balances of those that have
//     NO orders (the outer query), answering "count and sum of balances above
//     x" exactly.
// Execute: O(codes * (log blocks + blocks)) with no per-customer work; the
// AVG comparison is the exact rational  balance * C > S.
//
// Memory: 16 B per custkey (customers and order-only custkeys) + ~2.5 B of
// chunk overhead, plus 16 B per distinct (prefix, balance) of zero-order
// customers (bounded by the balance domain): ~19 B/customer, 0.3 GB at SF100,
// 2.8 GB at SF1000 (the old unordered_map<custkey, record with a std::string>
// cost ~80 B/customer: 1.2 GB at SF100). Write batch: O(touched custkeys *
// 64-customer chunk + touched prefixes' balance blocks); no table scan.
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

struct TpchQ22OrderCount {
  int64_t custkey = 0;
  int64_t count = 0;
};

class TpchQ22InitialScan {
public:
  virtual ~TpchQ22InitialScan() = default;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ22CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ22OrderInput &)> &) = 0;
  // Optional SQL pre-aggregation: number of orders per o_custkey (non-NULL).
  // Returns false when the engine does not implement it.
  virtual bool ScanOrderCounts(
      const std::function<void(const TpchQ22OrderCount &)> &) {
    return false;
  }
};

// ── State structures ────────────────────────────────────────────────────────
constexpr uint32_t kTpchQ22NoCustomer = 0xffffffffu; // no customer row
constexpr uint32_t kTpchQ22NullPhone = 0xfffffffeu;  // customer, phone NULL
constexpr int64_t kTpchQ22NullBalance = std::numeric_limits<int64_t>::min();

struct TpchQ22CustomerRecord {
  int64_t balance_raw = 0; // kTpchQ22NullBalance when NULL
  int32_t order_count = 0; // orders with o_custkey = this custkey
  uint32_t prefix_id = kTpchQ22NoCustomer;
};

// Phone prefix (first two characters) -> dense id. Exact string equality.
struct TpchQ22PrefixDictionary {
  std::unordered_map<std::string, uint32_t> ids;
};

struct TpchQ22PrefixEntry {
  int64_t positive_count = 0;       // customers with c_acctbal > 0
  __int128 positive_sum_raw = 0;    // their balance sum (x100)
  core::SortedCountSum zero_order;  // balances (>0) of those without orders
};

struct TpchQ22State {
  std::shared_ptr<const TpchQ22PrefixDictionary> dictionary =
      std::make_shared<const TpchQ22PrefixDictionary>();
  core::AnyKeyCowArray<TpchQ22CustomerRecord, 6> customers;
  std::vector<std::shared_ptr<const TpchQ22PrefixEntry>> prefixes;
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

  // One customer row change: the old row (if any) is removed, then the new
  // row (if any) is stored under its own key.
  struct CustomerDelta {
    bool has_old = false;
    bool has_new = false;
    int64_t old_custkey = 0;
    int64_t new_custkey = 0;
    bool new_phone_null = false;
    std::string new_prefix;
    int64_t new_balance = 0; // kTpchQ22NullBalance when NULL
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
  static void EmitRows(const std::vector<TpchQ22OutputRow> &rows,
                       core::AuxRowSink &sink);

  TpchQ22Config config;
  TpchQ22InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
