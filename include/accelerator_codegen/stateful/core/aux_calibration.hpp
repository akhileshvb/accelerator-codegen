// SCAFFOLDING
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/maintenance/cost_model.hpp"

namespace accelerator_codegen::aux_state::core {

// The callbacks deliberately operate at the shared-interface boundary. An
// engine adapter supplies them using DuckDB or PostgreSQL primitives, while
// the fitting and serialization code remains engine-neutral.
struct AuxCalibrationHooks {
  std::function<void()> reset;
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
  std::vector<std::size_t> delta_row_counts = {1, 16, 128, 1024};
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

maintenance::CostModelParams LoadAuxCostModelJson(
    const std::string &, const maintenance::CostModelParams &fallback = {});

} // namespace accelerator_codegen::aux_state::core
