#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "tpch/q1/layout.hpp"
#include "tpch/q1/duckdb/aux/finalize.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ1Config {
  core::TableId lineitem_table = 0;
};

struct TpchQ1LineitemInput {
  int32_t shipdate_days = 0;
  int64_t quantity_raw = 0;
  int32_t quantity_scale = 2;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int64_t tax_raw = 0;
  int32_t tax_scale = 2;
  char returnflag = '\0';
  char linestatus = '\0';
};

// Pre-aggregated (day, returnflag, linestatus) stats for SQL-pushdown builds.
struct TpchQ1DayGroupAggregate {
  int32_t shipdate_days = 0;
  char returnflag = '\0';
  char linestatus = '\0';
  int64_t count = 0;
  int64_t sum_qty_raw = 0;
  int64_t sum_base_price_raw = 0;
  int64_t sum_discount_raw = 0;
  int64_t sum_disc_price_raw = 0; // scale 4
  int64_t sum_charge_raw = 0;     // scale 6
};

class TpchQ1InitialScan {
public:
  virtual ~TpchQ1InitialScan() = default;
  virtual void ScanLineitemsSerial(
      const std::function<void(const TpchQ1LineitemInput &)> &) = 0;

  // Optional: DuckDB GROUP BY day/flag. Return true when the consumer was
  // invoked for the full aggregate; BuildState then skips the per-row scan.
  virtual bool ScanDayGroupAggregates(
      const std::function<void(const TpchQ1DayGroupAggregate &)> &) {
    return false;
  }

  // Optional: same aggregates restricted to the given shipdate days. Used by
  // RebuildPartition so a local rescan does not read the rest of lineitem.
  virtual bool ScanDayGroupAggregatesForDays(
      const std::set<int32_t> &,
      const std::function<void(const TpchQ1DayGroupAggregate &)> &) {
    return false;
  }
};

// One dyadic node. The flat partition lives inside the image (a single
// allocation per published node).
struct TpchQ1StateImage {
  int32_t shipdate_days = 0;
  ::accelerator_codegen::Q1DayPartition partition;
};

struct TpchQ1LineitemDelta {
  int32_t shipdate_days = 0;
  ::accelerator_codegen::Q1GroupKey group_key = 0;
  int64_t quantity_raw = 0;
  int64_t extended_price_raw = 0;
  int64_t discount_raw = 0;
  int64_t tax_raw = 0;
  int8_t sign = 0;
};


int32_t TpchQ1CutoffDays(int32_t day_offset);

class TpchQ1TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ1LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ1LineitemDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ1LineitemDelta> deltas;
};

class TpchQ1Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ1Layout::kAccelerator;

  explicit TpchQ1Module(TpchQ1Config config,
                        TpchQ1InitialScan *initial_scan = nullptr)
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
  std::unordered_map<int32_t, ::accelerator_codegen::Q1DayPartition>
  BuildState() const;
  std::unordered_map<int32_t, ::accelerator_codegen::Q1DayPartition>
  BuildStateForDays(const std::set<int32_t> &days) const;
  std::vector<::accelerator_codegen::Q1OutputRow>
  ComposeOutput(const std::unordered_map<int32_t, ::accelerator_codegen::Q1DayPartition> &,
                int32_t cutoff_days) const;
  void EmitResult(const ::accelerator_codegen::Q1OutputRow *rows,
                  uint32_t row_count, core::AuxRowSink &) const;

  TpchQ1Config config;
  TpchQ1InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
