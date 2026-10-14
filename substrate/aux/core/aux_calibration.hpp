// SCAFFOLDING
#pragma once
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include <optional>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/maintenance/cost_model.hpp"

namespace accelerator_codegen::aux_state::core {

// The callbacks deliberately operate at the shared-interface boundary. An
// engine adapter supplies them using DuckDB or PostgreSQL primitives, while
// the fitting and serialization code remains engine-neutral.
struct AuxCalibrationHooks {
  // Before every sample, untimed.
  std::function<void()> reset;
  // Before every maintenance sample (apply_delta, rebuild_partition,
  // rebuild_all, invalidate), untimed: puts a fresh state in place so the
  // sample times only the action.
  std::function<void()> prepare_maintenance;
  std::function<void()> build_initial_state;
  std::function<void()> execute_auxiliary;
  std::function<void()> execute_fallback;
  std::function<void(std::size_t)> apply_delta;
  std::function<void()> rebuild_partition;
  std::function<void()> rebuild_all;
  std::function<void()> invalidate;
};

struct AuxCalibrationConfig {
  std::size_t warmup_samples = 2;
  std::size_t measurement_samples = 7;
  std::vector<std::size_t> delta_row_counts = {1, 256, 4096, 65536};
};

struct AuxCalibrationResult {
  maintenance::CostModelParams params;
  double build_initial_state_us = 0.0;
  double aux_hit_us = 0.0;
  double fallback_us = 0.0;
  double apply_delta_base_us = 0.0;
  double apply_delta_per_row_us = 0.0;
  double partition_rebuild_us = 0.0;
  double full_rebuild_us = 0.0;
  double invalidation_overhead_us = 0.0;
  std::size_t samples = 0;
};

struct LinearCostFit {
  double intercept_us = 0.0;
  double per_row_us = 0.0;
};

LinearCostFit FitLinearCost(
    const std::vector<std::pair<std::size_t, double>> &samples);

AuxCalibrationResult CalibrateAuxCostModel(
    const AuxCalibrationHooks &, const maintenance::CostModelParams &seed = {},
    const AuxCalibrationConfig & = {});

// Writes the same flat JSON shape consumed by the runtime policy loader. The
// metadata fields are informational and may be omitted by callers.
void WriteAuxCostModelJson(const std::string &, const AuxCalibrationResult &,
                           const std::string &engine = {},
                           const std::string &accelerator = {});

// Median time of `run` in microseconds over `samples` runs (after
// `warmups`); `prepare` runs untimed before each.
double MedianTimeUs(const std::function<void()> &run, std::size_t warmups, std::size_t samples,
                    const std::function<void()> &prepare = {});

// ---- Calibration from real writes (engine-agnostic) ----
//
// The engine side: runs one write transaction, with maintenance forced to
// `forced` (or the cost model's choice), then one accelerator query, and
// times both together, so an action is charged what it costs wherever the
// engine pays it (at commit, or on the next read). Each engine's runtime
// backend (or its SQL client, for server engines) implements it.
class CalibrationEngine {
public:
  virtual ~CalibrationEngine() = default;
  struct Timed {
    double us = 0.0;      // write + read, end to end (informational)
    double read_us = 0.0; // the accelerator query alone
    // The write's maintenance decision: action, the rows and partitions the
    // module reported, and the coordinator's maintenance time.
    std::optional<MaintenanceDecisionLogEntry> decision;
  };
  virtual Timed WriteThenRead(const std::string &sql,
                              std::optional<maintenance::MaintenanceAction> forced) = 0;
};

// A query's calibration writes (what AuxQueryModule::CalibrationWrites
// returns), registered from the query's own directory so engine tools that
// have no module instance (e.g. a server engine's calibrate binary) can find
// them by (workload, query id).
using CalibrationWritesFn = std::vector<CalibrationWrite> (*)();
void RegisterCalibrationWrites(const std::string &workload, int query, CalibrationWritesFn writes);
// Empty if the query registered none.
std::vector<CalibrationWrite> CalibrationWritesFor(const std::string &workload, int query);
struct CalibrationWritesRegistrar {
  CalibrationWritesRegistrar(const std::string &workload, int query, CalibrationWritesFn writes) {
    RegisterCalibrationWrites(workload, query, writes);
  }
};

struct WriteCalibrationSample {
  std::string name;
  uint64_t rows = 0;       // as the module reported them to the cost model
  uint64_t partitions = 0;
  double control_us = 0.0;  // the control write + read, end to end
  double hit_read_us = 0.0; // a read after the control write (a hit)
  double apply_delta_us = 0.0;      // net of control_us
  double rebuild_partition_us = 0.0;
  double rebuild_all_us = 0.0;
};

struct WriteCalibrationResult {
  maintenance::CostModelParams params;
  std::vector<WriteCalibrationSample> samples;
  // Costs no write measured (kept from the seed): "apply_delta",
  // "rebuild_partition", "rebuild_all".
  std::vector<std::string> unfitted;
};

// For each write: one unforced run (the sizes the module reports), the
// control write (its reads are the hit baseline), then ApplyDelta /
// RebuildPartition / RebuildAll forced, each apply followed by its revert
// (same shape), `repeats` times, medians. An action's cost is the
// coordinator's maintenance time for the commit plus what the first read
// after it costs over a hit (a rebuild deferred to the read): the capture
// of the changed rows and the engine's own commit are the same for every
// action and are not charged. Fits ApplyDelta = overhead + per_row * rows,
// RebuildPartition = base + per_partition * partitions, RebuildAll = mean.
// A write the module did not react to (no decision) is skipped; ApplyDelta
// is fitted over writes with rows, RebuildPartition over writes with
// partitions (a whole-state change reports none); a fit without samples
// keeps `seed`'s value, as do the other parameters.
WriteCalibrationResult CalibrateFromWrites(const std::vector<CalibrationWrite> &writes,
                                           CalibrationEngine &engine,
                                           const maintenance::CostModelParams &seed,
                                           int repeats = 2);

maintenance::CostModelParams LoadAuxCostModelJson(
    const std::string &, const maintenance::CostModelParams &fallback = {});

} // namespace accelerator_codegen::aux_state::core
