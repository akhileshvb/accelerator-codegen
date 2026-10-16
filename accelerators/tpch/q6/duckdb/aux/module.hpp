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
#include "tpch/q6/layout.hpp"
#include "tpch/q6/duckdb/aux/finalize.hpp"

namespace accelerator_codegen::aux_state::query {

int32_t TpchQ6MonthKeyFromDays(int32_t days_since_epoch);

struct TpchQ6Config {
  core::TableId lineitem_table = 0;
};

struct TpchQ6LineitemInput {
  int32_t shipdate_days = 0;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int64_t quantity_raw = 0;
  int32_t quantity_scale = 2;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  // l_discount or l_quantity is NULL (the row is outside the matrix domain).
  bool null_cell = false;
};

class TpchQ6InitialScan {
public:
  virtual ~TpchQ6InitialScan() = default;
  // Partitioned scan for builds and the fallback: rows of one partition arrive
  // in table order on one thread; partitions run concurrently on the pool.
  virtual void ScanLineitems(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ6LineitemInput &)> &) = 0;
  // Pre-aggregated scan of the given months: one input per (month, discount,
  // quantity) group; shipdate_days is any day of the month, extended_price_raw
  // is the SUM of the group (the matrix contribution is linear in the price).
  virtual void ScanLineitemsForMonths(
      const std::set<int32_t> &month_keys,
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) = 0;
};

enum class TpchQ6PartitionKind : uint8_t { Day, Month };

struct TpchQ6StateImage {
  TpchQ6PartitionKind kind = TpchQ6PartitionKind::Day;
  int32_t key = 0;
  // The matrix (64-byte aligned prefix-sum block) lives in the image itself:
  // one allocation per published partition, no pointer chase on reads.
  ::accelerator_codegen::Q6RevenueMatrix matrix;
  // The month holds a row outside the matrix domain (Q6InDomain, or a NULL
  // discount or quantity): the matrix cannot answer for it, so queries that
  // read this partition fall back to SQL. Cleared only by a rebuild.
  bool declined = false;
};

struct TpchQ6LineitemDelta {
  int32_t shipdate_days = 0;
  int32_t month_key = 0;
  int8_t discount_cent = 0;
  int64_t quantity_scaled = 0;
  ::accelerator_codegen::Q6AccT contribution =
      static_cast<::accelerator_codegen::Q6AccT>(0);
  int8_t sign = 0;
  // The row is outside the matrix's domain: it is in no cell, and adding one
  // declines its month.
  bool out_of_domain = false;
};

std::vector<uint8_t> EncodeTpchQ6Matrix(const ::accelerator_codegen::Q6RevenueMatrix &);
bool DecodeTpchQ6Matrix(const std::vector<uint8_t> &,
                        ::accelerator_codegen::Q6RevenueMatrix &);

int32_t TpchQ6MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ6StartOfMonthDays(int32_t days_since_epoch);
int32_t TpchQ6StartOfNextMonthDays(int32_t days_since_epoch);
int32_t TpchQ6ExclusiveEndDays(int32_t start_days);
int32_t TpchQ6AddMonths(int32_t days_since_epoch, int32_t months);

struct TpchQ6QueryParams {
  int32_t start_days = 0;
  int32_t end_days = 0;
  int64_t discount_lower_raw = 0;
  int64_t discount_upper_raw = 0;
  int64_t quantity_upper_raw = 0;
};

bool ParseTpchQ6QueryParams(const core::QueryArguments &, TpchQ6QueryParams &);
core::ScaledDecimal TpchQ6RevenueOutputValue(::accelerator_codegen::Q6AccT raw);

class TpchQ6TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ6LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ6LineitemDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ6LineitemDelta> deltas;
};

class TpchQ6Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ6Layout::kAccelerator;

  explicit TpchQ6Module(TpchQ6Config config,
                        TpchQ6InitialScan *initial_scan = nullptr)
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
  // The raw month matrices of a build scan and the months that must decline.
  struct MonthMatrices {
    std::unordered_map<core::PartitionId, ::accelerator_codegen::Q6RevenueMatrix> matrices;
    std::set<core::PartitionId> declined;
  };
  MonthMatrices BuildMatrices(core::AuxEngineServices &) const;
  // Matrices are accumulated raw per partition; PrefixInPlace() before publish.
  // A row outside the matrix domain adds its partition to `declined`.
  void ApplyLineitemInput(const TpchQ6LineitemInput &input,
                          std::unordered_map<core::PartitionId,
                                             ::accelerator_codegen::Q6RevenueMatrix> &matrices,
                          std::set<core::PartitionId> &declined) const;
  void CollectQueryPartitions(const TpchQ6QueryParams &params,
                              std::vector<core::PartitionId> &partitions) const;
  void EmitResult(::accelerator_codegen::Q6AccT revenue,
                  core::AuxRowSink &) const;

  TpchQ6Config config;
  TpchQ6InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
