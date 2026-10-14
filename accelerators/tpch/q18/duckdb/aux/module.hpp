#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q18 layout (inlined here because query_layouts.hpp is not writable) ─────
struct TpchQ18Layout {
  static constexpr const char *kAccelerator = "tpch_q18_aux";
  static constexpr core::PartitionId kOrderQuantitySums = 0;

  static constexpr std::array<core::PartitionId, 1> Partitions() {
    return {kOrderQuantitySums};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ18Config {
  core::TableId lineitem_table = 0;
  core::TableId orders_table = 0;
  core::TableId customer_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ18LineitemInput {
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
};

struct TpchQ18OrderInput {
  int64_t key = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  int64_t totalprice_raw = 0;
};

struct TpchQ18CustomerInput {
  int64_t key = 0;
  std::string name;
};

struct TpchQ18OrderQuantityAggregate {
  int64_t orderkey = 0;
  int64_t quantity_sum_raw = 0;
};

struct TpchQ18SurvivorAggregate {
  int64_t orderkey = 0;
  int64_t quantity_sum_raw = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  int64_t totalprice_raw = 0;
};

class TpchQ18InitialScan {
public:
  virtual ~TpchQ18InitialScan() = default;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ18LineitemInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ18OrderInput &)> &) = 0;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ18CustomerInput &)> &) = 0;

  // Optional: SQL SUM(quantity) GROUP BY orderkey.
  virtual bool ScanOrderQuantityAggregates(
      const std::function<void(const TpchQ18OrderQuantityAggregate &)> &consumer) {
    (void)consumer;
    return false;
  }

  // Optional: orders whose SUM(quantity) already exceeds min_quantity_raw,
  // with order metadata joined in. Avoids an O(orders) C++ hash map at SF20.
  virtual bool ScanSurvivorOrders(
      int64_t min_quantity_raw,
      const std::function<void(const TpchQ18SurvivorAggregate &)> &consumer) {
    (void)min_quantity_raw;
    (void)consumer;
    return false;
  }

  // Point-lookup for write maintenance when a lineitem delta crosses the
  // survivor threshold and order metadata is not yet cached.
  virtual bool LookupOrder(int64_t orderkey, TpchQ18OrderInput &out) {
    (void)orderkey;
    (void)out;
    return false;
  }

  // Point lookup of one customer name.
  virtual bool LookupCustomerName(int64_t custkey, std::string &name) {
    (void)custkey;
    (void)name;
    return false;
  }

  // Current SUM(l_quantity) for one order, including the open transaction.
  virtual bool LookupOrderQuantitySum(int64_t orderkey, int64_t &sum_raw) {
    (void)orderkey;
    (void)sum_raw;
    return false;
  }
};

// ── State structures ────────────────────────────────────────────────────────
struct TpchQ18SurvivorMeta {
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  int64_t totalprice_raw = 0;
  int64_t quantity_sum_raw = 0;
};

struct TpchQ18State {
  // Persist order metadata only for orders whose quantity sum exceeds this
  // threshold (300.00 at scale 2).  TPC-H Q18 uses 312.00, so survivors
  // cover every order that can appear in the default result set.
  static constexpr int64_t kMinPersistQuantityRaw = 30000;

  // The three maps are copy-on-write sharded hash maps: copying the state is
  // pointer copies and a write clones only the touched shard, so a commit
  // costs O(rows changed), not O(survivors).

  // Sparse quantity sums for survivor orders only. Non-survivors are resolved
  // with a point SQL lookup on write so Execute memory stays O(survivors)
  // at SF20 (dense max(orderkey) arrays do not).
  core::ShardedCowHashMap<int64_t, 8> quantity_overflow;

  // Sparse order metadata for high-quantity orders only.
  core::ShardedCowHashMap<TpchQ18SurvivorMeta, 8> survivors;

  // Customer names cached at build time so EmitResult does not rescan
  // the customer table on every hit (dominant cost at SF20).
  core::ShardedCowHashMap<std::string, 8> customer_names;

  // ── Quantity sum accessors ──────────────────────────────────────────────
  void AddOrderQuantity(int64_t orderkey, int64_t delta);
  void SetOrderQuantitySum(int64_t orderkey, int64_t sum);
  int64_t OrderQuantitySum(int64_t orderkey) const;

  // ── Survivor metadata accessors ─────────────────────────────────────────
  void SetOrderMeta(int64_t orderkey, int64_t custkey, int32_t orderdate_days,
                    int64_t totalprice_raw);
  void RemoveSurvivor(int64_t orderkey);
  void SyncSurvivorQuantity(int64_t orderkey);
  bool HasOrderMeta(int64_t orderkey) const;
  int64_t OrderCustkey(int64_t orderkey) const;
  int32_t OrderOrderdate(int64_t orderkey) const;
  int64_t OrderTotalprice(int64_t orderkey) const;
};

enum class TpchQ18ImageKind : uint8_t { OrderQuantitySums };

struct TpchQ18StateImage {
  TpchQ18ImageKind kind = TpchQ18ImageKind::OrderQuantitySums;
  std::shared_ptr<const TpchQ18State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ18OutputRow {
  std::string name;
  int64_t custkey = 0;
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int64_t totalprice_raw = 0;
  int64_t quantity_sum_raw = 0;
};

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ18LineitemDelta {
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
  int8_t sign = 0;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ18OrderQuantitySums(const TpchQ18State &state);
bool DecodeTpchQ18OrderQuantitySums(const std::vector<uint8_t> &bytes,
                                    TpchQ18State &state);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ18TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ18LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ18LineitemDelta delta);
  void AddCustomerName(int64_t custkey, std::string name);
  const std::vector<std::pair<int64_t, std::string>> &CustomerNames() const;
  void NoteOrderInserted(int64_t orderkey);
  void NoteOrderDeleted(int64_t orderkey);
  const std::unordered_set<int64_t> &InsertedOrders() const;
  const std::unordered_set<int64_t> &DeletedOrders() const;

private:
  std::vector<std::pair<int64_t, std::string>> customer_names;
  std::unordered_set<int64_t> inserted_orders;
  std::unordered_set<int64_t> deleted_orders;
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ18LineitemDelta> deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ18Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ18Layout::kAccelerator;

  explicit TpchQ18Module(TpchQ18Config config,
                         TpchQ18InitialScan *initial_scan = nullptr)
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
  TpchQ18State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                  std::shared_ptr<const TpchQ18State> &state) const;
  void EmitResult(const TpchQ18State &state, int64_t threshold_raw,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;

  TpchQ18Config config;
  TpchQ18InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
