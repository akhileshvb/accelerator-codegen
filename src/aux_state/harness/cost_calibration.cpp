// SCAFFOLDING
#include "accelerator_codegen/aux_state/harness/cost_calibration.hpp"

#include <stdexcept>
#include <utility>

#include "accelerator_codegen/aux_state/engine/duckdb/duckdb_runtime.hpp"

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

std::string SharedCostModelConfigPath(SharedWorkloadKind workload, int query_id) {
  if (workload == SharedWorkloadKind::Tpch) {
    return "config/q" + std::to_string(query_id) + "_aux_cost_model.json";
  }
  return "config/sec_edgar_q" + std::to_string(query_id) + "_aux_cost_model.json";
}

core::AuxCalibrationResult CalibrateSharedQueryCostModel(
    duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
    const core::AuxCalibrationConfig &config) {
  const auto probe = FindSharedProbe(connection, entry);
  DuckdbRuntime runtime(connection, entry, 10.0);

  core::AuxCalibrationHooks hooks;
  hooks.reset = [&] { runtime.ResetCalibrationState(); };
  hooks.build_initial_state = [&] {
    runtime.ResetCalibrationState();
    runtime.Build();
  };
  hooks.execute_auxiliary = [&] {
    runtime.ResetCalibrationState();
    runtime.Build();
    runtime.Execute();
  };
  hooks.execute_fallback = [&] { runtime.ExecuteBaseQuery(); };
  hooks.apply_delta = [&](std::size_t row_count) {
    runtime.ResetCalibrationState();
    runtime.Build();
    runtime.ApplyMutationBatch(ScaleMutation(entry, probe, row_count));
  };
  hooks.rebuild_partition = [&] {
    runtime.ResetCalibrationState();
    runtime.Build();
    runtime.ApplyMutationBatch(ScaleMutation(entry, probe, 1));
  };
  hooks.rebuild_all = [&] {
    runtime.ResetCalibrationState();
    runtime.Build();
    runtime.InvalidateAll();
    runtime.Build();
  };
  hooks.invalidate = [&] {
    runtime.ResetCalibrationState();
    runtime.Build();
    runtime.InvalidateAll();
  };

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
