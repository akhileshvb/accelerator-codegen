#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q21 layout ──────────────────────────────────────────────────────────────
struct TpchQ21Layout {
  static constexpr const char *kAccelerator = "tpch_q21_aux";
  static constexpr core::PartitionId kState = 0;

  static constexpr std::array<core::PartitionId, 1> Partitions() {
    return {kState};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ21Config {
  core::TableId supplier_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
  core::TableId nation_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ21SupplierInput {
  int64_t suppkey = 0;
  std::string name;
  int64_t nationkey = 0;
};

struct TpchQ21NationInput {
  int64_t nationkey = 0;
  std::string name;
};

// A lineitem of a fulfilled order ('F'): its supplier and whether it is late
// (receiptdate > commitdate).
struct TpchQ21LineInput {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  bool late = false;
};

class TpchQ21InitialScan {
public:
  virtual ~TpchQ21InitialScan() = default;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ21SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ21NationInput &)> &) = 0;
  // The lineitems of every fulfilled order (join with orders done by the
  // database), grouped by orderkey: all rows of one order are consecutive.
  virtual void ScanFulfilledLineitems(
      const std::function<void(const TpchQ21LineInput &)> &) = 0;
  // The same for the given orders only, as the database holds them after the
  // calling statement (orders that are not fulfilled or have no lineitem
  // report nothing).
  virtual bool LookupFulfilledLineitems(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const TpchQ21LineInput &)> &) = 0;
};

// ── State ───────────────────────────────────────────────────────────────────
// What an order adds to the answer: `count` rows for supplier `suppkey`. An
// order contributes iff it is fulfilled, has lineitems of at least two
// suppliers and exactly one supplier has late ones (that supplier, with its
// number of late lineitems). Orders that do not contribute are not stored, so
// the state is proportional to the contributing orders, not to all orders.
struct TpchQ21Contribution {
  int64_t suppkey = 0;
  int64_t count = 0;
};

struct TpchQ21Supplier {
  std::string name;
  int64_t nationkey = 0;
};

// Memory: ~29 B per contributing order + ~90 B per supplier + ~25 B per
// supplier with waiting orders. At SF100 (1e6 suppliers, ~2-3% of the 1.5e8
// orders contributing) that is ~0.2 GB, at SF1000 ~2 GB; the previous state
// kept a node map per contributing order with a nested map per supplier
// (~250 B each) and built it from a per-order map of ALL orders (~25-40 GB
// transient at SF100). Maintenance clones touched shards only.
struct TpchQ21State {
  // orderkey -> contribution, for contributing orders.
  core::ShardedCowHashMap<TpchQ21Contribution, 12> contributions;
  // suppkey -> sum of its contributions (non-zero entries), whether or not a
  // supplier row exists (a supplier inserted later picks it up).
  core::ShardedCowHashMap<int64_t, 10> numwait;
  core::ShardedCowHashMap<TpchQ21Supplier, 10> suppliers;
  // nationkey -> (suppkey -> numwait) for existing suppliers with numwait != 0.
  std::map<int64_t, core::ShardedCowHashMap<int64_t, 6>> nation_numwait;
  // n_name -> nationkeys (several nations may share a name).
  std::map<std::string, std::vector<int64_t>> nation_keys;

  // Replace the contribution of one order (`has` false: it contributes nothing).
  void SetContribution(int64_t orderkey, bool has,
                       const TpchQ21Contribution &contribution);
  void AdjustNumwait(int64_t suppkey, int64_t delta);
  void PutSupplier(int64_t suppkey, const std::string &name, int64_t nationkey);
  // false: no such supplier.
  bool RemoveSupplier(int64_t suppkey);
  void PutNation(int64_t nationkey, const std::string &name);
  // false: no such nation row.
  bool RemoveNation(int64_t nationkey, const std::string &name);
};

struct TpchQ21StateImage {
  std::shared_ptr<const TpchQ21State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ21OutputRow {
  std::string name;
  int64_t numwait = 0;
};

// ── Deltas ──────────────────────────────────────────────────────────────────
// The new contribution of an order, from the database as the writing
// statement left it (the last one recorded for an order wins).
struct TpchQ21OrderResult {
  int64_t orderkey = 0;
  bool has = false;
  TpchQ21Contribution contribution;
};

// A supplier or nation row change (insert: new only, delete: old only,
// update: both).
struct TpchQ21SupplierDelta {
  bool has_old = false;
  bool has_new = false;
  int64_t old_suppkey = 0;
  TpchQ21SupplierInput new_row;
};

struct TpchQ21NationDelta {
  bool has_old = false;
  bool has_new = false;
  TpchQ21NationInput old_row;
  TpchQ21NationInput new_row;
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ21TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();
  void AddOrderResult(TpchQ21OrderResult result);
  void AddSupplierDelta(TpchQ21SupplierDelta delta);
  void AddNationDelta(TpchQ21NationDelta delta);

  const std::vector<TpchQ21OrderResult> &OrderResults() const;
  const std::vector<TpchQ21SupplierDelta> &SupplierDeltas() const;
  const std::vector<TpchQ21NationDelta> &NationDeltas() const;
  uint64_t Size() const;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ21OrderResult> order_results;
  std::vector<TpchQ21SupplierDelta> supplier_deltas;
  std::vector<TpchQ21NationDelta> nation_deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ21Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ21Layout::kAccelerator;

  explicit TpchQ21Module(TpchQ21Config config,
                         TpchQ21InitialScan *initial_scan = nullptr)
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
  TpchQ21State BuildState() const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ21State> &state,
                 core::Version *version = nullptr) const;
  void EmitRows(std::vector<TpchQ21OutputRow> rows,
                core::AuxRowSink &sink) const;

  TpchQ21Config config;
  TpchQ21InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
