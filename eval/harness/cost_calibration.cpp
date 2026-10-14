// SCAFFOLDING
#include "eval/harness/cost_calibration.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "substrate/aux/core/aux_cost_model_path.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace core = accelerator_codegen::aux_state::core;
namespace aux_engine = ::accelerator_codegen::aux_state::engine;
using aux_engine::SharedWorkloadKind;
using aux_engine::SharedQueryCatalogEntry;
using aux_engine::SharedProbe;
using aux_engine::FindSharedProbe;
using aux_engine::BuildSharedMutation;
using aux_engine::duckdb::DuckdbRuntime;

namespace {

core::MutationBatch ScaleMutation(const SharedQueryCatalogEntry &entry,
                                  const SharedProbe &probe,
                                  std::size_t row_count) {
  if (row_count == 0) {
    throw std::invalid_argument("calibration row count must be > 0");
  }
  auto mutation = BuildSharedMutation(entry, probe);
  mutation.old_rows.clear();
  mutation.new_rows.clear();
  mutation.old_rows.reserve(row_count);
  mutation.new_rows.reserve(row_count);
  for (std::size_t index = 0; index < row_count; ++index) {
    if (mutation.kind == core::MutationKind::Insert) {
      mutation.new_rows.push_back(probe.new_row);
    } else if (mutation.kind == core::MutationKind::Delete) {
      mutation.old_rows.push_back(probe.old_row);
    } else {
      mutation.old_rows.push_back(probe.old_row);
      mutation.new_rows.push_back(probe.new_row);
    }
  }
  (void)entry;
  return mutation;
}

} // namespace

std::string SharedCostModelConfigPath(const SharedQueryCatalogEntry &entry) {
  const char *override_path =
      std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL");
  if (override_path && *override_path) {
    return override_path;
  }
  core::AuxCostModelLookup lookup;
  // ACCEL_AUX_BENCH_DB_LABEL=<workload>_sf<X> (e.g. tpch_sf20): the models
  // calibrated at that scale, <stem>_sf<X>.json, when they exist; otherwise
  // the default <stem>.json
  const char *db_label = std::getenv("ACCEL_AUX_BENCH_DB_LABEL");
  const std::string label = db_label ? db_label : "";
  const std::string prefix = std::string(entry.workload_name ? entry.workload_name : "") + "_";
  if (prefix.size() > 1 && label.rfind(prefix + "sf", 0) == 0 && label.size() > prefix.size() + 2) {
    lookup.scale = label.substr(prefix.size());
  }
  // ACCEL_AUX_COST_MODEL_DIR: a subdirectory of the query's config dir with
  // models of the same names (e.g. verus: <bench>/q<N>/config/verus for the
  // Verus kits), searched first; a query it has no model
  // for falls back to its own directory
  if (const char *dir = std::getenv("ACCEL_AUX_COST_MODEL_DIR"); dir && *dir) {
    const std::string own = entry.cost_model.dir ? entry.cost_model.dir : "";
    lookup.dirs_before.push_back(own.empty() ? std::string(dir) : own + "/" + dir);
  }
  lookup.default_when_missing = true;
  return core::ResolveAuxCostModelPath(entry.cost_model, lookup);
}

core::AuxCalibrationResult CalibrateSharedQueryCostModel(
    duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
    const core::AuxCalibrationConfig &config) {
  DuckdbRuntime runtime(connection, entry, 10.0);
  const auto writes = runtime.CalibrationWrites();
  if (!writes.empty()) {
    // Read side measured directly; maintenance from real captured writes
    // (core::CalibrateFromWrites).
    core::AuxCalibrationResult result;
    result.params = core::LoadAuxCostModelJson(SharedCostModelConfigPath(entry));
    const auto warmups = config.warmup_samples, samples = config.measurement_samples;
    result.params.first_query_blocking_us = core::MedianTimeUs(
        [&] { runtime.Build(); }, warmups, samples, [&] { runtime.ResetCalibrationState(); });
    runtime.Build();
    result.params.aux_hit_us = core::MedianTimeUs([&] { runtime.Execute(); }, warmups, samples);
    result.params.fallback_us = core::MedianTimeUs([&] { runtime.ExecuteBaseQuery(); }, warmups, samples);
    result.params.invalidation_overhead_us = core::MedianTimeUs(
        [&] { runtime.InvalidateAll(); }, warmups, samples, [&] { runtime.Build(); });
    runtime.Build();
    runtime.EnableMutationCapture();
    aux_engine::duckdb::DuckdbCalibrationEngine engine(runtime);
    const auto fitted = core::CalibrateFromWrites(writes, engine, result.params);
    result.params = fitted.params;
    for (const auto &name : fitted.unfitted) {
      std::cerr << "  not calibrated (no write measured it; kept): " << name << "\n";
    }
    for (const auto &sample : fitted.samples) {
      std::cerr << "  write " << sample.name << ": rows=" << sample.rows
                << " partitions=" << sample.partitions << " delta=" << sample.apply_delta_us
                << "us partition=" << sample.rebuild_partition_us << "us all=" << sample.rebuild_all_us
                << "us\n";
    }
    return result;
  }

  // No calibration writes: synthetic mutations from the query's probe.
  const auto probe = FindSharedProbe(connection, entry);

  core::AuxCalibrationHooks hooks;
  // Measure() calls reset before every sample. Keep it a no-op so aux_hit
  // samples stay on a warm published image; destructive hooks reset themselves.
  hooks.reset = [&] {};
  hooks.build_initial_state = [&] {
    runtime.ResetCalibrationState();
    runtime.Build();
  };
  hooks.execute_auxiliary = [&] {
    // Hit path only — build_initial_state already published state.
    runtime.Execute();
  };
  hooks.execute_fallback = [&] { runtime.ExecuteBaseQuery(); };
  // A fresh published state before each maintenance sample (untimed).
  hooks.prepare_maintenance = [&] {
    runtime.ResetCalibrationState();
    runtime.Build();
  };
  hooks.apply_delta = [&](std::size_t row_count) {
    core::SetForcedMaintenanceActionName("ApplyDelta");
    runtime.ApplyMutationBatch(ScaleMutation(entry, probe, row_count));
    core::SetForcedMaintenanceActionName("");
  };
  hooks.rebuild_partition = [&] {
    core::SetForcedMaintenanceActionName("RebuildPartition");
    runtime.ApplyMutationBatch(ScaleMutation(entry, probe, 1));
    core::SetForcedMaintenanceActionName("");
  };
  hooks.rebuild_all = [&] {
    core::SetForcedMaintenanceActionName("RebuildAll");
    runtime.ApplyMutationBatch(ScaleMutation(entry, probe, 1));
    core::SetForcedMaintenanceActionName("");
  };
  hooks.invalidate = [&] { runtime.InvalidateAll(); };
  return core::CalibrateAuxCostModel(hooks, {}, config);
}

void WriteSharedQueryCostModel(const std::string &path,
                               const core::AuxCalibrationResult &result,
                               const SharedQueryCatalogEntry &entry) {
  const char *workload =
      entry.workload == SharedWorkloadKind::Tpch ? "tpch" : "sec_edgar";
  core::WriteAuxCostModelJson(path, result, "duckdb",
                              std::string(workload) + "_q" +
                                  std::to_string(entry.query_id));
}

} // namespace accelerator_codegen::aux_state::harness
