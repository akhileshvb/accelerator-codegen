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

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q20 layout ──────────────────────────────────────────────────────────────
struct TpchQ20Layout {
  static constexpr const char *kAccelerator = "tpch_q20_aux";
  static constexpr core::PartitionId kFullState = 0;

  static constexpr std::array<core::PartitionId, 1> Partitions() {
    return {kFullState};
  }
};

struct TpchQ20Config {
  core::TableId lineitem_table = 0;
  core::TableId part_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId supplier_table = 0;
  core::TableId nation_table = 0;
};

// ── Typed bulk-scan contract ───────────────────────────────────────────────
struct TpchQ20LineitemInput {
  int32_t shipdate_days = 0;
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t quantity_raw = 0;
};

struct TpchQ20PartInput {
  int64_t key = 0;
  std::string name;
};

struct TpchQ20PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t availqty = 0;
};

struct TpchQ20SupplierInput {
  int64_t key = 0;
  std::string name;
  std::string address;
  int32_t nationkey = 0;
};

struct TpchQ20NationInput {
  int32_t key = 0;
  std::string name;
};

class TpchQ20InitialScan {
public:
  virtual ~TpchQ20InitialScan() = default;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ20LineitemInput &)> &) = 0;
  virtual void ScanParts(
      const std::function<void(const TpchQ20PartInput &)> &) = 0;
  virtual void ScanPartsupps(
      const std::function<void(const TpchQ20PartsuppInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ20SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ20NationInput &)> &) = 0;
};

// ── State structures ────────────────────────────────────────────────────────
struct TpchQ20DateQuantity {
  int32_t shipdate_days = 0;
  int64_t quantity_raw = 0;
};

// One (partkey, suppkey) temporal aggregate. Dates are kept sorted so
// one-year-window probes are O(log n) per pair rather than O(n) for the
// whole lineitem table.
struct TpchQ20PairSeries {
  std::vector<TpchQ20DateQuantity> entries;
  int64_t total_quantity_raw = 0;
  bool dirty = false;
};

struct TpchQ20PartRecord {
  std::string name;
  // active partsupp adjacency (suppkey -> availqty)
  std::unordered_map<int64_t, int64_t> partsupps;
};

struct TpchQ20SupplierRecord {
  std::string name;
  std::string address;
  int32_t nationkey = 0;
};

struct TpchQ20NationRecord {
  std::string name;
  std::vector<int64_t> supplier_keys;
};

struct TpchQ20State {
  // Keyed by (partkey << 20) | suppkey. Only pairs that actually appear in
  // lineitem are materialized, keeping state proportional to fact cardinality
  // (no SF-hardcoded dense arrays).
  std::unordered_map<uint64_t, TpchQ20PairSeries> pair_series;
  // Maintenance chain: if non-null, lookups fall back to this published
  // state for keys not found in the local pair_series delta.
  std::shared_ptr<const TpchQ20State> parent_state;

  // Part name -> set of partkeys. Built from the ordered part scan and used
  // for exact trailing-'%' prefix matching at query time (no other wildcards).
  std::map<std::string, std::vector<int64_t>> part_name_index;
  std::unordered_map<int64_t, TpchQ20PartRecord> parts;

  // ── Evaluate fast-path structures ─────────────────────────────────────────
  // Pointer into pair_series for each partsupp entry. Stored alongside the
  // flat partsupp key in a merged struct so the evaluate loop avoids a
  // per-entry hash lookup in pair_series. Built once at build time (the state
  // is immutable after build) and invalidated via clear() on overlay states.
  struct FlatPartsuppEntry {
    uint64_t pair_key = 0;
    int64_t availqty = 0;
    const TpchQ20PairSeries *series = nullptr;
  };
  std::vector<FlatPartsuppEntry> flat_partsupp_keys;
  bool flat_partsupp_keys_sorted = false;

  // Dimension data, keyed naturally.
  std::unordered_map<int64_t, TpchQ20SupplierRecord> suppliers;
  std::unordered_map<int32_t, TpchQ20NationRecord> nations;

  // Nation name -> key set for exact name lookups.
  std::unordered_map<std::string, std::vector<int32_t>> nation_name_index;

  void AddPairEntry(uint64_t pair_key, int32_t shipdate_days,
                    int64_t quantity_raw);
  void RemovePairEntry(uint64_t pair_key, int32_t shipdate_days,
                       int64_t quantity_raw);
  bool HasPairQuantityInWindow(uint64_t pair_key, int32_t start_date_days,
                               int32_t end_date_days) const;
  int64_t SumPairQuantity(uint64_t pair_key, int32_t start_date_days,
                          int32_t end_date_days) const;
  // Fused query: returns {sum_qty, has_entries}. Single sorted scan with
  // early termination. Returns {-1, false} when pair_key is absent.
  std::pair<int64_t, bool> QueryWindowSum(uint64_t pair_key,
                                          int32_t start_date_days,
                                          int32_t end_date_days) const;
  // Sort all pair-series entries by shipdate_days. Called after build and
  // after delta application so read-path probes are single-pass.
  void SortPairEntries();
  void BuildFlatPartsuppKeys();
  const TpchQ20SupplierRecord *FindSupplier(int64_t suppkey) const;
  const TpchQ20PartRecord *FindPart(int64_t partkey) const;
  const TpchQ20PairSeries *FindPairSeries(uint64_t pair_key) const;
  std::vector<int64_t> FindPartKeysByPrefix(const std::string &prefix) const;
  std::vector<int32_t> FindNationKeysByName(const std::string &name) const;
};

enum class TpchQ20ImageKind : uint8_t { Full };

struct TpchQ20StateImage {
  TpchQ20ImageKind kind = TpchQ20ImageKind::Full;
  std::shared_ptr<const TpchQ20State> state;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ20OutputRow {
  std::string name;
  std::string address;
};

// ── Delta types ─────────────────────────────────────────────────────────────
struct TpchQ20LineitemDelta {
  int32_t shipdate_days = 0;
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t quantity_raw = 0;
  int8_t sign = 0;
};

struct TpchQ20PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t availqty = 0;
  int8_t sign = 0;
};

struct TpchQ20PartNameDelta {
  int64_t partkey = 0;
  std::string old_name;
  std::string new_name;
  int8_t sign = 0;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ20State(const TpchQ20State &state);
bool DecodeTpchQ20State(const std::vector<uint8_t> &bytes, TpchQ20State &state);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ20TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();

  const std::vector<TpchQ20LineitemDelta> &LineitemDeltas() const;
  const std::vector<TpchQ20PartsuppDelta> &PartsuppDeltas() const;
  const std::vector<TpchQ20PartNameDelta> &PartNameDeltas() const;

  void AddLineitemDelta(TpchQ20LineitemDelta delta);
  void AddPartsuppDelta(TpchQ20PartsuppDelta delta);
  void AddPartNameDelta(TpchQ20PartNameDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ20LineitemDelta> lineitem_deltas;
  std::vector<TpchQ20PartsuppDelta> partsupp_deltas;
  std::vector<TpchQ20PartNameDelta> part_name_deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ20Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ20Layout::kAccelerator;

  explicit TpchQ20Module(TpchQ20Config config,
                         TpchQ20InitialScan *initial_scan = nullptr)
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

private:
  TpchQ20State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ20State> &state) const;
  void EmitResult(const TpchQ20State &state, int32_t start_date_days,
                  int32_t end_date_days, const std::string &part_name_pattern,
                  const std::string &nation_name,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;

  TpchQ20Config config;
  TpchQ20InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
