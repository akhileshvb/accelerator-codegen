#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q18 layout ──────────────────────────────────────────────────────────────
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

// One row per orderkey: SUM(l_quantity) and COUNT(*) over its lineitems.
struct TpchQ18OrderQuantityAggregate {
  int64_t orderkey = 0;
  int64_t quantity_sum_raw = 0;
  int64_t line_count = 0;
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

  // Optional: SQL `SELECT l_orderkey, SUM(l_quantity), COUNT(*) FROM lineitem
  // GROUP BY l_orderkey`, streamed (one row per orderkey). Returns false when
  // the engine has no such scan; the caller then streams ScanLineitems.
  virtual bool ScanOrderQuantityAggregates(
      const std::function<void(const TpchQ18OrderQuantityAggregate &)>
          &consumer) {
    (void)consumer;
    return false;
  }

  // Names of the given customers (customers that do not exist are not
  // reported). Engines override this with a keyed SQL lookup; the default
  // filters a full customer scan.
  virtual void LookupCustomerNames(
      const std::vector<int64_t> &custkeys,
      const std::function<void(const TpchQ18CustomerInput &)> &consumer);
};

// ── State ───────────────────────────────────────────────────────────────────
// One record per orderkey that has an orders row or at least one lineitem.
// The record carries everything the query needs about the order, so a write to
// lineitem or orders is a per-key delta that needs no lookup in the database.
struct TpchQ18OrderRec {
  int64_t sum_raw = 0;        // SUM(l_quantity) * 100 over the order's lineitems
  int64_t totalprice_raw = 0; // orders row (valid when HasOrder)
  int64_t custkey = 0;        // orders row (valid when HasOrder)
  int32_t orderdate_days = 0; // orders row (valid when HasOrder)
  uint32_t flags_lines = 0;   // top bit: orders row present; rest: #lineitems

  static constexpr uint32_t kHasOrder = 0x80000000u;
  bool HasOrder() const { return (flags_lines & kHasOrder) != 0; }
  uint32_t Lines() const { return flags_lines & ~kHasOrder; }
  bool Empty() const { return flags_lines == 0 && sum_raw == 0; }
  // The order can appear in the result for a threshold below its sum.
  bool Qualifies(int64_t threshold_raw) const {
    return HasOrder() && Lines() > 0 && sum_raw > threshold_raw;
  }
};

// Memory: 32 B record + ~9 B hash slot / 0.875 load = ~47 B per order, plus
// ~1 B per hot order and ~80 B per cached customer name (hot orders only).
//   SF100  (1.5e8 orders): ~7 GB      SF1000 (1.5e9 orders): ~70 GB
// The previous state was an unordered_map per survivor (~100 B each) but its
// build transiently held a node map over EVERY order (6 GB at SF100).
// Write cost: a batch of B touched orders clones <= B shards of ~5K-25K
// records (~1 MB each at SF1000); the shard directory is 64K pointers.
struct TpchQ18State {
  // Orders whose sum exceeds this are indexed in `hot` (300.00 at scale 2; the
  // TPC-H threshold range is 312-315). A threshold at or above it enumerates
  // only the hot set; a lower one scans every record (the result is then
  // large anyway: it holds every order above that quantity).
  static constexpr int64_t kHotFloorRaw = 30000;

  core::ShardedCowHashMap<TpchQ18OrderRec, 16> orders;
  core::ShardedCowHashMap<uint8_t, 12> hot;
  // Names of customers of hot orders (a cache: a customer that is missing here
  // is looked up by key when a result needs it).
  core::ShardedCowHashMap<std::string, 10> customer_names;

  // Record maintenance. `Reindex` re-derives the hot-set membership of one
  // order and drops its record when nothing is left of it.
  TpchQ18OrderRec &MutableRec(int64_t orderkey) { return orders[orderkey]; }
  void Reindex(int64_t orderkey);
  // false: the delta would leave a negative lineitem count (state mismatch).
  bool AddLineitem(int64_t orderkey, int64_t quantity_raw, int sign);
  void SetOrder(int64_t orderkey, int64_t custkey, int32_t orderdate_days,
                int64_t totalprice_raw);
  // false: there was no orders row for the key.
  bool ClearOrder(int64_t orderkey);
};

struct TpchQ18StateImage {
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

// ── Deltas ──────────────────────────────────────────────────────────────────
struct TpchQ18LineitemDelta {
  int64_t orderkey = 0;
  int64_t quantity_raw = 0;
  int8_t sign = 0;
};

struct TpchQ18OrderDelta {
  int8_t sign = 0; // +1: orders row inserted, -1: deleted
  TpchQ18OrderInput order;
};

// A customer row change. `has_old` / `has_new`: the row before / after the
// write (insert: new only, delete: old only, update: both).
struct TpchQ18CustomerDelta {
  bool has_old = false;
  bool has_new = false;
  int64_t old_key = 0;
  TpchQ18CustomerInput new_row;
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ18TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();
  void AddLineitem(TpchQ18LineitemDelta delta);
  void AddOrder(TpchQ18OrderDelta delta);
  void AddCustomer(TpchQ18CustomerDelta delta);

  const std::vector<TpchQ18LineitemDelta> &Lineitems() const;
  const std::vector<TpchQ18OrderDelta> &Orders() const;
  const std::vector<TpchQ18CustomerDelta> &Customers() const;
  uint64_t Size() const;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ18LineitemDelta> lineitems;
  std::vector<TpchQ18OrderDelta> orders;
  std::vector<TpchQ18CustomerDelta> customers;
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
  TpchQ18State BuildState() const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ18State> &state) const;
  // Customer names for the rows (cache first, then a keyed lookup), then the
  // ORDER BY and the output. Rows whose customer does not exist are dropped.
  void EmitRows(std::vector<TpchQ18OutputRow> rows,
                const TpchQ18State *state, core::AuxRowSink &sink) const;

  TpchQ18Config config;
  TpchQ18InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
