// SCAFFOLDING
#include "eval/harness/cost_calibration.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "backends/duckdb/api/duckdb_runtime.hpp"
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

core::AuxCalibrationResult CalibrateSharedQueryCostModel(
    duckdb::Connection &connection, const SharedQueryCatalogEntry &entry, const core::QueryArguments &arguments,
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
    result.params.aux_hit_us = core::MedianTimeUs([&] { runtime.Execute(arguments); }, warmups, samples);
    result.params.fallback_us = core::MedianTimeUs([&] { runtime.ExecuteBaseQuery(arguments); }, warmups, samples);
    result.params.invalidation_overhead_us = core::MedianTimeUs(
        [&] { runtime.InvalidateAll(); }, warmups, samples, [&] { runtime.Build(); });
    runtime.Build();
    aux_engine::duckdb::DuckdbCalibrationEngine engine(runtime, arguments);
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
  const auto probe = FindSharedProbe(connection, entry, arguments);

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
    runtime.Execute(arguments);
  };
  hooks.execute_fallback = [&] { runtime.ExecuteBaseQuery(arguments); };
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
