#include <cassert>
#include <cstdio>
#include <string>
#include <utility>

#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::maintenance::CostModelParams;

int main() {
  const auto fit = FitLinearCost({{1, 12.0}, {10, 30.0}, {100, 210.0}});
  assert(fit.intercept_us >= 0.0);
  assert(fit.per_row_us >= 0.0);
  assert(fit.per_row_us > 1.0);

  std::size_t callback_count = 0;
  AuxCalibrationHooks hooks;
  hooks.reset = [&] { ++callback_count; };
  hooks.build_initial_state = [&] { ++callback_count; };
  hooks.execute_auxiliary = [&] { ++callback_count; };
  hooks.execute_fallback = [&] { ++callback_count; };
  hooks.apply_delta = [&](std::size_t rows) { callback_count += rows; };
  hooks.rebuild_partition = [&] { ++callback_count; };
  hooks.rebuild_all = [&] { ++callback_count; };
  hooks.invalidate = [&] { ++callback_count; };

  AuxCalibrationConfig config;
  config.warmup_samples = 0;
  config.measurement_samples = 1;
  config.delta_row_counts = {1, 2};
  const auto result = CalibrateAuxCostModel(hooks, CostModelParams{}, config);
  assert(result.samples == 1);
  assert(result.params.apply_delta_per_row_us >= 0.0);
  // The fitted intercept is represented exactly once, as the policy's
  // fixed overhead. The legacy base term is retained only for compatibility
  // with old JSON files and is zero for newly calibrated models.
  assert(result.params.apply_delta_base_us == 0.0);
  assert(result.params.apply_delta_overhead_us == result.apply_delta_base_us);
  assert(callback_count > 0);

  CostModelParams policy;
  policy.aux_hit_us = 0.0;
  policy.fallback_us = 10000.0;
  policy.full_rebuild_us = 100000.0;
  policy.invalidation_overhead_us = 10.0;
  policy.apply_delta_overhead_us = 0.0;
  policy.apply_delta_base_us = 0.0;
  policy.apply_delta_per_row_us = 1.0;
  MaintenanceRequest whole_state_request{
      1, 0, false, false, true, true, false,
      accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp,
      1.0};
  const auto read_light = SelectAuxMaintenanceAction(whole_state_request,
                                                       policy);
  whole_state_request.expected_queries_before_next_write = 1000.0;
  const auto read_heavy = SelectAuxMaintenanceAction(whole_state_request,
                                                      policy);
  assert(read_light.action ==
         accelerator_codegen::aux_state::maintenance::MaintenanceAction::
             InvalidateAndFallback);
  assert(read_heavy.action ==
         accelerator_codegen::aux_state::maintenance::MaintenanceAction::
             RebuildAll);

  const std::string path = "/tmp/accelerator_codegen_aux_calibration_test.json";
  WriteAuxCostModelJson(path, result, "test", "test_accelerator");
  const auto loaded = LoadAuxCostModelJson(path, CostModelParams{});
  assert(loaded.apply_delta_max_rows == result.params.apply_delta_max_rows);
  assert(loaded.min_samples == result.params.min_samples);
  std::remove(path.c_str());

  // Official RF-scale batches (~0.1% SF20 ≈ 30k orders / ~120k lineitems)
  // must stay on ApplyDelta for every current TPC-H aux cost model.
  using accelerator_codegen::aux_state::maintenance::MaintenanceAction;
  for (const char *model_path :
       {"config/q1_aux_cost_model.json", "config/q3_aux_cost_model.json",
        "config/q3_aux_cost_model_sf20.json", "config/q6_aux_cost_model.json",
        "config/q9_aux_cost_model.json", "config/q10_aux_cost_model.json",
        "config/q11_aux_cost_model.json", "config/q12_aux_cost_model.json",
        "config/q14_aux_cost_model.json", "config/q17_aux_cost_model.json",
        "config/q18_aux_cost_model.json", "config/q21_aux_cost_model.json"}) {
    const auto params = LoadAuxCostModelJson(model_path, CostModelParams{});
    assert(params.apply_delta_max_rows >= 120000);
    const auto rf = SelectAuxMaintenanceAction(
        {120000, 64, true, true, true, false, false, MaintenanceAction::NoOp,
         1.0},
        params);
    assert(rf.action == MaintenanceAction::ApplyDelta);
  }
  return 0;
}
