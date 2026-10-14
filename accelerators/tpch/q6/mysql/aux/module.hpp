#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "tpch/q6/layout.hpp"
#include "tpch/q6/mysql/aux/finalize.hpp"

namespace accelerator_codegen::aux_state::query {

// State size (independent of the scale factor): one Fenwick node per touched
// day-tree node, each a (distinct discount) x (distinct quantity) grid of
// 24 B cells (int128 revenue + int64 count). TPC-H data: ~2.5k days -> ~5k
// nodes x (11 x 50 cells) = ~65 MB at any SF, i.e. 0 B/row amortized (a row
// costs nothing once its cell exists). Dictionaries grow with distinct
// observed values only (off-spec data adds cells, never drops rows).
// Per-write cost: O(log days) node clones per changed row (a node is cloned
// once per write batch). Execute: <= 64 node visits x qualifying cells
// (<= ~550), independent of SF.

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
};

// Pre-aggregated (shipdate, discount, quantity) cell for SQL-pushdown builds.
struct TpchQ6CellAggregate {
  int32_t shipdate_days = 0;
  int64_t discount_raw = 0; // scale 2
  int64_t quantity_raw = 0; // scale 2
  int64_t count = 0;
  ::accelerator_codegen::Q6AccT revenue_raw = 0; // sum(price * discount), scale 4
};

class TpchQ6InitialScan {
public:
  virtual ~TpchQ6InitialScan() = default;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ6LineitemInput &)> &) = 0;

  // GROUP BY (shipdate, discount, quantity) pushed into the engine. The
  // default aggregates the row scan here; engines override it with one SQL
  // aggregate (its output is bounded by days x discounts x quantities).
  virtual void ScanCellAggregates(
      const std::function<void(const TpchQ6CellAggregate &)> &consumer);
};

// The whole state is one image (partition 0).
struct TpchQ6StateImage {
  std::shared_ptr<const ::accelerator_codegen::Q6State> state;
};

struct TpchQ6LineitemDelta {
  int32_t shipdate_days = 0;
  int64_t discount_raw = 0;
  int64_t quantity_raw = 0;
  ::accelerator_codegen::Q6AccT revenue = 0;
  int8_t sign = 0;
};

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
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  // Aggregate scan -> state (one SQL GROUP BY; no row scan).
  std::shared_ptr<::accelerator_codegen::Q6State>
  BuildState(core::AuxEngineServices &) const;
  void Answer(const ::accelerator_codegen::Q6State &, const core::QueryArguments &,
              core::AuxRowSink &) const;
  void EmitResult(const ::accelerator_codegen::Q6Result &,
                  core::AuxRowSink &) const;

  TpchQ6Config config;
  TpchQ6InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
