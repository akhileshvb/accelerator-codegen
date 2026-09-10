#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q11 layout ──────────────────────────────────────────────────────────────
struct TpchQ11Layout {
  static constexpr const char *kAccelerator = "tpch_q11_aux";
  static constexpr core::PartitionId kMetadata = 0;
  // FNV-1a-hashed namespace for parameter-keyed fast-path reads
  static constexpr core::PartitionId kNationNameBase = 2000000;
  static constexpr core::PartitionId kNationNameSlots = 1048576;

  static constexpr std::array<core::PartitionId, 1> FixedPartitions() {
    return {kMetadata};
  }
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ11Config {
  core::TableId partsupp_table = 0;
  core::TableId supplier_table = 0;
  core::TableId nation_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ11PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t availqty = 0;
  int64_t supplycost_raw = 0;
  int32_t supplycost_scale = 2;
};

struct TpchQ11SupplierInput {
  int64_t suppkey = 0;
  int32_t nationkey = 0;
};

struct TpchQ11NationInput {
  int32_t nationkey = 0;
  std::string name;
};

class TpchQ11InitialScan {
public:
  virtual ~TpchQ11InitialScan() = default;
  virtual void ScanPartsupp(
      const std::function<void(const TpchQ11PartsuppInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ11SupplierInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ11NationInput &)> &) = 0;
};

// ── Contribution value types ────────────────────────────────────────────────
#if defined(__SIZEOF_INT128__)
using Q11AccT = __int128_t;
#else
using Q11AccT = int64_t;
#endif

struct Q11PartContribution {
  Q11AccT value = 0;
  int64_t row_count = 0;
};

// ── Output row ──────────────────────────────────────────────────────────────
struct TpchQ11OutputRow {
  int64_t partkey = 0;
  Q11AccT value = 0;
};

// ── State structures ────────────────────────────────────────────────────────
struct TpchQ11State {
  std::unordered_map<std::string, int32_t> nation_name_to_key;
  std::unordered_map<int32_t, std::vector<int64_t>> nationkey_to_suppliers;
  std::unordered_map<int64_t, int32_t> supplier_to_nation;

  // Build-time per-supplier contributions (merged into nation maps after build)
  std::unordered_map<int64_t, std::unordered_map<int64_t, Q11PartContribution>>
      supplier_part_contributions;

  // Per-nation pre-aggregated and pre-sorted rows
  std::unordered_map<int32_t, std::vector<TpchQ11OutputRow>> nation_sorted_rows;
  std::unordered_map<int32_t, Q11AccT> nation_total_value;
};

enum class TpchQ11ImageKind : uint8_t {
  Metadata,
  NationPreAggregated
};

struct TpchQ11StateImage {
  TpchQ11ImageKind kind = TpchQ11ImageKind::Metadata;
  std::shared_ptr<const TpchQ11State> state;

  // For NationPreAggregated: key and collision guard
  int32_t nationkey = 0;
  std::string nation_name;
  Q11AccT total_value = 0;
  // Pre-sorted by value DESC, partkey ASC
  std::vector<TpchQ11OutputRow> sorted_rows;
};

// ── Deltas ──────────────────────────────────────────────────────────────────
struct TpchQ11PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  Q11AccT value = 0;
  int8_t sign = 0;
};

struct TpchQ11SupplierDelta {
  int64_t suppkey = 0;
  int32_t old_nationkey = 0;
  int32_t new_nationkey = 0;
};

struct TpchQ11NationDelta {
  int32_t nationkey = 0;
  std::string old_name;
  std::string new_name;
};

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ11Metadata(const TpchQ11State &state);
std::vector<uint8_t> EncodeTpchQ11NationPartition(
    int32_t nationkey, const std::string &nation_name,
    Q11AccT total_value,
    const std::vector<TpchQ11OutputRow> &sorted_rows);
bool DecodeTpchQ11Metadata(const std::vector<uint8_t> &bytes,
                           TpchQ11State &state);
bool DecodeTpchQ11NationPartition(
    const std::vector<uint8_t> &bytes,
    int32_t &nationkey, std::string &nation_name,
    Q11AccT &total_value,
    std::vector<TpchQ11OutputRow> &sorted_rows);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ11TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ11PartsuppDelta> &PartsuppDeltas() const;
  const std::vector<TpchQ11SupplierDelta> &SupplierDeltas() const;
  const std::vector<TpchQ11NationDelta> &NationDeltas() const;
  void MarkRebuild();
  void AddPartsuppDelta(TpchQ11PartsuppDelta delta);
  void AddSupplierDelta(TpchQ11SupplierDelta delta);
  void AddNationDelta(TpchQ11NationDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ11PartsuppDelta> partsupp_deltas;
  std::vector<TpchQ11SupplierDelta> supplier_deltas;
  std::vector<TpchQ11NationDelta> nation_deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ11Module final : public core::AuxQueryModule {
public:
  explicit TpchQ11Module(TpchQ11Config config,
                         TpchQ11InitialScan *initial_scan = nullptr)
      : config(std::move(config)), initial_scan(initial_scan) {}
  ~TpchQ11Module() override = default;

  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &, core::AuxEngineServices &,
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
  TpchQ11State BuildState(core::AuxEngineServices &services) const;
  void EmitResult(const std::vector<TpchQ11OutputRow> &rows,
                  core::AuxRowSink &sink) const;

  TpchQ11Config config;
  TpchQ11InitialScan *initial_scan = nullptr;
};

// ── Helpers ─────────────────────────────────────────────────────────────────
core::PartitionId NationNamePartition(const std::string &name);

// ── Finalize ────────────────────────────────────────────────────────────────
std::vector<TpchQ11OutputRow> FinalizeQ11FromSorted(
    const std::vector<TpchQ11OutputRow> &sorted_rows,
    Q11AccT total_value, double fraction);

} // namespace accelerator_codegen::aux_state::query