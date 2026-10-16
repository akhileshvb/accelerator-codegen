#include "eval/tests/test_support/db_paths.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_cost_model_path.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::maintenance::CostModelParams;

namespace {

void Check(bool ok, const std::string &what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << "\n";
    std::exit(1);
  }
}

// The lookup's precedence: directories in order, and in each the most
// specific name first; "" or the default path when nothing exists.
void TestCostModelLookup() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "accelerator_codegen_cost_model_lookup_test";
  fs::remove_all(root);
  fs::create_directories(root / "q" / "config");
  fs::create_directories(root / "extra");
  const std::string dir = (root / "q" / "config").string();
  const std::string extra = (root / "extra").string();
  const AuxCostModelLocation location{dir.c_str(), "m"};
  auto Touch = [](const std::string &path) { std::ofstream(path) << "{}"; };
  auto Remove = [](const std::string &path) { fs::remove(path); };

  AuxCostModelLookup plain;
  Check(ResolveAuxCostModelPath(location, plain) == "", "nothing found: empty");
  plain.default_when_missing = true;
  Check(ResolveAuxCostModelPath(location, plain) == dir + "/m.json", "nothing found: default");
  Check(AuxCostModelDefaultPath(location) == dir + "/m.json", "default path");

  AuxCostModelLookup lookup;
  lookup.engine = "postgres";
  lookup.scale = "sf20";
  lookup.dirs_before = {"", extra};
  Touch(dir + "/m.json");
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m.json", "default");
  Touch(dir + "/m_sf20.json");
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_sf20.json", "scale over default");
  Touch(dir + "/m_postgres.json");
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_postgres.json", "engine over scale");
  Touch(dir + "/m_postgres_sf20.json");
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_postgres_sf20.json",
        "engine and scale first");
  Touch(extra + "/m.json");
  Check(ResolveAuxCostModelPath(location, lookup) == extra + "/m.json", "earlier directory first");
  Remove(extra + "/m.json");
  lookup.engine.clear();
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_sf20.json", "no engine: scale");
  lookup.scale.clear();
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m.json", "no engine, no scale");
  Remove(dir + "/m.json");
  lookup.dirs_after = {extra};
  Touch(extra + "/m.json");
  Check(ResolveAuxCostModelPath(location, lookup) == extra + "/m.json", "later directory");
  fs::remove_all(root);
}

// A missing scale falls back to the nearest smaller calibrated scale.
void TestCostModelScaleFallback() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "accelerator_codegen_cost_model_scale_test";
  fs::remove_all(root);
  fs::create_directories(root);
  const std::string dir = root.string();
  const AuxCostModelLocation location{dir.c_str(), "m"};
  for (const char *name : {"m.json", "m_sf1.json", "m_sf20.json", "m_postgres_sf1.json"}) {
    std::ofstream(dir + "/" + name) << "{}";
  }
  AuxCostModelLookup lookup;
  lookup.scale = "sf3000";
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_sf20.json", "sf3000 -> sf20");
  lookup.scale = "sf5";
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_sf1.json", "sf5 -> sf1");
  lookup.scale = "sf1";
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_sf1.json", "sf1 exact");
  lookup.scale = "sf0.5";
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m.json", "below all scales: default");
  lookup.scale = "sf3000";
  lookup.engine = "postgres";
  Check(ResolveAuxCostModelPath(location, lookup) == dir + "/m_postgres_sf1.json",
        "engine's nearest smaller scale beats the engine-less scale");
  fs::remove_all(root);
}

// The selector scales the ApplyDelta cap and rebuild costs with the data size
// relative to the calibrated scale; without a known scale it is unchanged.
void TestMaintenanceScaling() {
  using accelerator_codegen::aux_state::maintenance::MaintenanceAction;
  CostModelParams params;
  params.full_rebuild_us = 1e6;
  params.partition_rebuild_us = 1e5;
  params.apply_delta_overhead_us = 100.0;
  params.apply_delta_per_row_us = 1.0;
  params.apply_delta_max_rows = 100000;
  auto request = [](uint64_t rows, uint64_t data_rows) {
    MaintenanceRequest r{rows, 4, true, true, true, false, false, MaintenanceAction::NoOp, 1.0};
    r.data_rows_hint = data_rows;
    return r;
  };
  const auto action = [&](const MaintenanceRequest &r) {
    return SelectAuxMaintenanceAction(r, params).action;
  };

  // Calibrated scale unknown: today's decisions, whatever the hint.
  Check(action(request(1000, 0)) == MaintenanceAction::ApplyDelta, "small: ApplyDelta");
  Check(action(request(500000, 0)) != MaintenanceAction::ApplyDelta, "over cap: rebuild");
  Check(action(request(500000, 150000000)) != MaintenanceAction::ApplyDelta, "hint alone: unchanged");

  params.calibrated_scale_rows = 1000000;
  Check(action(request(1000, 0)) == MaintenanceAction::ApplyDelta, "at scale, unknown data");
  Check(action(request(1000, 1000000)) == MaintenanceAction::ApplyDelta, "at calibrated scale: small");
  Check(action(request(500000, 1000000)) != MaintenanceAction::ApplyDelta, "at calibrated scale: cap");

  // 150x data: a 150x refresh is under the scaled cap and under the scaled rebuild cost.
  params.apply_delta_per_row_us = 0.5;
  Check(action(request(15000000, 150000000)) == MaintenanceAction::ApplyDelta, "150x refresh: ApplyDelta");
  // Per-row cost over the scaled rebuild cost: rebuild.
  params.apply_delta_per_row_us = 20.0;
  Check(action(request(15000000, 150000000)) != MaintenanceAction::ApplyDelta, "costly rows: rebuild");

  // whole_state_required and low_confidence behave as before at any scale.
  auto whole = request(1000, 150000000);
  whole.whole_state_required = true;
  Check(action(whole) != MaintenanceAction::ApplyDelta, "whole state: no ApplyDelta");
  // low_confidence never picks ApplyDelta, scaled or not.
  auto low = request(1000, 150000000);
  low.low_confidence = true;
  Check(action(low) != MaintenanceAction::ApplyDelta, "low confidence: conservative");
}

} // namespace

void TestDecisionRecordsDataScale() {
  const auto entry = ParseMaintenanceDecision("action=ApplyDelta rows=5 partitions=1 maintenance_us=3.5 data_rows=77");
  Check(entry && entry->rows_touched == 5 && entry->data_rows_hint == 77, "decision text carries data_rows");
  Check(ParseMaintenanceDecision("action=ApplyDelta rows=5 partitions=1 maintenance_us=3.5")->data_rows_hint == 0,
        "a decision without data_rows: unknown");
}

// Every shipped Postgres and MySQL model records the scale it was calibrated
// at; a Postgres refresh of 0.2% of the data, at SF3000 relative to the SF20
// calibration, is still applied as a delta where the TPC-H refresh functions
// reach (orders / lineitem).
void TestShippedModelsScaleWithData() {
  using accelerator_codegen::aux_state::maintenance::MaintenanceAction;
  constexpr uint64_t kSf3000OverSf20 = 150;
  const std::string config = std::string(ACCEL_SOURCE_DIR) + "/accelerators/tpch/q";
  for (const char *engine : {"postgres", "mysql"}) {
    for (int q = 1; q <= 22; ++q) {
      const auto path = config + std::to_string(q) + "/config/q" + std::to_string(q) +
                        "_aux_cost_model_" + engine + ".json";
      Check(LoadAuxCostModelJson(path).calibrated_scale_rows != 0, path + " records its scale");
    }
  }
  for (const int q : {1, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22}) {
    const auto params = LoadAuxCostModelJson(config + std::to_string(q) + "/config/q" +
                                             std::to_string(q) + "_aux_cost_model_postgres.json");
    const uint64_t data_rows = params.calibrated_scale_rows * kSf3000OverSf20;
    MaintenanceRequest refresh{data_rows / 500, 4, true, true, true, false, false,
                               MaintenanceAction::NoOp, 1.0};
    refresh.data_rows_hint = data_rows;
    Check(SelectAuxMaintenanceAction(refresh, params).action == MaintenanceAction::ApplyDelta,
          "Q" + std::to_string(q) + " refresh at SF3000 stays incremental");
  }
}

int main() {
  TestCostModelLookup();
  TestCostModelScaleFallback();
  TestMaintenanceScaling();
  TestDecisionRecordsDataScale();
  TestShippedModelsScaleWithData();
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
             InvalidateAndRebuildOnRead);
  assert(read_heavy.action ==
         accelerator_codegen::aux_state::maintenance::MaintenanceAction::
             RebuildAll);

  accelerator_codegen::test_support::ScratchDir scratch("aux_calibration_test");
  const std::string path = scratch.File("aux_cost_model.json");
  WriteAuxCostModelJson(path, result, "test", "test_accelerator");
  const auto loaded = LoadAuxCostModelJson(path, CostModelParams{});
  assert(loaded.apply_delta_max_rows == result.params.apply_delta_max_rows);
  assert(loaded.min_samples == result.params.min_samples);
  std::remove(path.c_str());

  // Official RF-scale batches (~0.1% SF20 ≈ 30k orders / ~120k lineitems)
  // must stay on ApplyDelta for every current TPC-H aux cost model.
  using accelerator_codegen::aux_state::maintenance::MaintenanceAction;
  for (const char *model_path :
       {"accelerators/tpch/q1/config/q1_aux_cost_model.json", "accelerators/tpch/q3/config/q3_aux_cost_model.json",
        "accelerators/tpch/q3/config/q3_aux_cost_model_sf20.json", "accelerators/tpch/q6/config/q6_aux_cost_model.json",
        "accelerators/tpch/q9/config/q9_aux_cost_model.json", "accelerators/tpch/q10/config/q10_aux_cost_model.json",
        "accelerators/tpch/q11/config/q11_aux_cost_model.json", "accelerators/tpch/q12/config/q12_aux_cost_model.json",
        "accelerators/tpch/q14/config/q14_aux_cost_model.json", "accelerators/tpch/q17/config/q17_aux_cost_model.json",
        "accelerators/tpch/q18/config/q18_aux_cost_model.json", "accelerators/tpch/q21/config/q21_aux_cost_model.json"}) {
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
