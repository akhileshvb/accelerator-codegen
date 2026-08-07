#include "accelerator_codegen/aux_state/core/aux_calibration.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include "accelerator_codegen/aux_state/maintenance/json_params.hpp"

namespace accelerator_codegen::aux_state::core {
namespace {

using Clock = std::chrono::steady_clock;

double Median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  if (values.size() % 2 == 0) {
    return (values[middle - 1] + values[middle]) / 2.0;
  }
  return values[middle];
}

double Measure(const std::function<void()> &callback,
               const AuxCalibrationHooks &hooks, std::size_t warmups,
               std::size_t samples) {
  if (!callback) {
    return 0.0;
  }
  for (std::size_t sample = 0; sample < warmups; ++sample) {
    if (hooks.reset) {
      hooks.reset();
    }
    callback();
  }
  std::vector<double> timings;
  timings.reserve(samples);
  for (std::size_t sample = 0; sample < samples; ++sample) {
    if (hooks.reset) {
      hooks.reset();
    }
    const auto start = Clock::now();
    callback();
    const auto finish = Clock::now();
    timings.push_back(
        std::chrono::duration<double, std::micro>(finish - start).count());
  }
  return Median(std::move(timings));
}

void WriteNumber(std::ostream &out, const char *key, double value,
                 bool comma = true) {
  out << "  \"" << key << "\": " << value;
  if (comma) {
    out << ',';
  }
  out << '\n';
}

void WriteUint(std::ostream &out, const char *key, uint64_t value,
               bool comma = true) {
  out << "  \"" << key << "\": " << value;
  if (comma) {
    out << ',';
  }
  out << '\n';
}

} // namespace

LinearCostFit FitLinearCost(
    const std::vector<std::pair<std::size_t, double>> &samples) {
  if (samples.empty()) {
    return {};
  }
  if (samples.size() == 1) {
    return {std::max(0.0, samples.front().second), 0.0};
  }
  const double n = static_cast<double>(samples.size());
  const double sum_x = std::accumulate(
      samples.begin(), samples.end(), 0.0,
      [](double total, const auto &sample) {
        return total + static_cast<double>(sample.first);
      });
  const double sum_y = std::accumulate(
      samples.begin(), samples.end(), 0.0,
      [](double total, const auto &sample) { return total + sample.second; });
  const double mean_x = sum_x / n;
  const double mean_y = sum_y / n;
  double numerator = 0.0;
  double denominator = 0.0;
  for (const auto &[x_value, y_value] : samples) {
    const double x = static_cast<double>(x_value) - mean_x;
    numerator += x * (y_value - mean_y);
    denominator += x * x;
  }
  const double slope = denominator > 0.0 ? numerator / denominator : 0.0;
  const double intercept = mean_y - slope * mean_x;
  return {std::max(0.0, intercept), std::max(0.0, slope)};
}

AuxCalibrationResult CalibrateAuxCostModel(
    const AuxCalibrationHooks &hooks, const maintenance::CostModelParams &seed,
    const AuxCalibrationConfig &config) {
  if (config.measurement_samples == 0) {
    throw std::invalid_argument("aux calibration requires at least one sample");
  }
  if (!hooks.build_initial_state || !hooks.execute_auxiliary ||
      !hooks.execute_fallback || !hooks.apply_delta ||
      !hooks.rebuild_partition || !hooks.rebuild_all || !hooks.invalidate) {
    throw std::invalid_argument(
        "aux calibration requires callbacks for every cost component");
  }
  if (config.delta_row_counts.empty()) {
    throw std::invalid_argument(
        "aux calibration requires at least one delta batch size");
  }
  AuxCalibrationResult result;
  result.params = seed;
  result.samples = config.measurement_samples;
  result.build_initial_state_us =
      Measure(hooks.build_initial_state, hooks, config.warmup_samples,
              config.measurement_samples);
  result.aux_hit_us = Measure(hooks.execute_auxiliary, hooks,
                              config.warmup_samples, config.measurement_samples);
  result.fallback_us = Measure(hooks.execute_fallback, hooks,
                               config.warmup_samples, config.measurement_samples);
  result.partition_rebuild_us =
      Measure(hooks.rebuild_partition, hooks, config.warmup_samples,
              config.measurement_samples);
  result.full_rebuild_us =
      Measure(hooks.rebuild_all, hooks, config.warmup_samples,
              config.measurement_samples);
  result.invalidation_overhead_us =
      Measure(hooks.invalidate, hooks, config.warmup_samples,
              config.measurement_samples);

  std::vector<std::pair<std::size_t, double>> delta_samples;
  for (const auto row_count : config.delta_row_counts) {
    const auto timing = Measure(
        [&, row_count] {
          if (hooks.apply_delta) {
            hooks.apply_delta(row_count);
          }
        },
        hooks, config.warmup_samples, config.measurement_samples);
    delta_samples.emplace_back(row_count, timing);
  }
  const auto fit = FitLinearCost(delta_samples);
  result.apply_delta_base_us = fit.intercept_us;
  result.apply_delta_per_row_us = fit.per_row_us;

  result.params.first_query_blocking_us = result.build_initial_state_us;
  result.params.aux_hit_us = result.aux_hit_us;
  result.params.fallback_us = result.fallback_us;
  result.params.partition_rebuild_us = result.partition_rebuild_us;
  result.params.full_rebuild_us = result.full_rebuild_us;
  // The measured delta hook is end-to-end: it includes state materialization,
  // the kernel, serialization/publication preparation, and output refresh.
  // Store its fitted intercept in the policy's fixed overhead term. Keeping
  // apply_delta_base_us at zero avoids counting the same intercept twice now
  // that the selector includes apply_delta_overhead_us.
  result.params.apply_delta_overhead_us = result.apply_delta_base_us;
  result.params.apply_delta_base_us = 0.0;
  result.params.apply_delta_per_row_us = result.apply_delta_per_row_us;
  result.params.invalidation_overhead_us = result.invalidation_overhead_us;
  return result;
}

void WriteAuxCostModelJson(const std::string &path,
                           const AuxCalibrationResult &result,
                           const std::string &engine,
                           const std::string &accelerator) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("unable to write auxiliary cost model: " + path);
  }
  out << "{\n";
  out << "  \"version\": 1,\n";
  if (!engine.empty()) {
    out << "  \"engine\": \"" << engine << "\",\n";
  }
  if (!accelerator.empty()) {
    out << "  \"accelerator\": \"" << accelerator << "\",\n";
  }
  const auto &params = result.params;
  WriteNumber(out, "aux_hit_us", params.aux_hit_us);
  WriteNumber(out, "fallback_us", params.fallback_us);
  WriteNumber(out, "partition_rebuild_us", params.partition_rebuild_us);
  WriteNumber(out, "full_rebuild_us", params.full_rebuild_us);
  WriteNumber(out, "first_query_blocking_us", params.first_query_blocking_us);
  WriteNumber(out, "apply_delta_overhead_us", params.apply_delta_overhead_us);
  WriteNumber(out, "apply_delta_base_us", params.apply_delta_base_us);
  WriteNumber(out, "apply_delta_per_row_us", params.apply_delta_per_row_us);
  WriteNumber(out, "apply_delta_part_lineitem_scan_us",
              params.apply_delta_part_lineitem_scan_us);
  WriteUint(out, "apply_delta_max_rows", params.apply_delta_max_rows);
  WriteUint(out, "apply_delta_max_part_fanout",
            params.apply_delta_max_part_fanout);
  WriteNumber(out, "invalidation_overhead_us", params.invalidation_overhead_us);
  WriteNumber(out, "decision_margin_us", params.decision_margin_us);
  WriteUint(out, "min_samples", params.min_samples);
  WriteNumber(out, "incremental_failure_prob", params.incremental_failure_prob);
  WriteNumber(out, "interference_rebuild_per_query_us",
              params.interference_rebuild_per_query_us);
  WriteNumber(out, "interference_rebuild_scan_byte_us",
              params.interference_rebuild_scan_byte_us);
  WriteNumber(out, "interference_delta_per_query_us",
              params.interference_delta_per_query_us);
  WriteNumber(out, "interference_invalidate_per_query_us",
              params.interference_invalidate_per_query_us);
  WriteNumber(out, "interference_query_slowdown_factor",
              params.interference_query_slowdown_factor);
  WriteNumber(out, "calibration_build_initial_state_us",
              result.build_initial_state_us);
  WriteNumber(out, "calibration_apply_delta_base_us",
              result.apply_delta_base_us);
  WriteNumber(out, "calibration_apply_delta_per_row_us",
              result.apply_delta_per_row_us);
  WriteUint(out, "calibration_samples", result.samples, false);
  out << "}\n";
}

maintenance::CostModelParams LoadAuxCostModelJson(
    const std::string &path, const maintenance::CostModelParams &fallback) {
  if (path.empty()) {
    return fallback;
  }
  std::ifstream in(path);
  if (!in) {
    return fallback;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  const auto content = buffer.str();
  auto params = fallback;
  using maintenance::ParseJsonDouble;
  using maintenance::ParseJsonUint64;
  params.aux_hit_us = ParseJsonDouble(content, "aux_hit_us", params.aux_hit_us);
  params.fallback_us = ParseJsonDouble(content, "fallback_us", params.fallback_us);
  params.partition_rebuild_us = ParseJsonDouble(
      content, "partition_rebuild_us", params.partition_rebuild_us);
  params.full_rebuild_us = ParseJsonDouble(content, "full_rebuild_us",
                                           params.full_rebuild_us);
  params.first_query_blocking_us = ParseJsonDouble(
      content, "first_query_blocking_us", params.first_query_blocking_us);
  params.apply_delta_overhead_us = ParseJsonDouble(
      content, "apply_delta_overhead_us", params.apply_delta_overhead_us);
  params.apply_delta_base_us = ParseJsonDouble(
      content, "apply_delta_base_us", params.apply_delta_base_us);
  params.apply_delta_per_row_us = ParseJsonDouble(
      content, "apply_delta_per_row_us", params.apply_delta_per_row_us);
  params.apply_delta_part_lineitem_scan_us = ParseJsonDouble(
      content, "apply_delta_part_lineitem_scan_us",
      params.apply_delta_part_lineitem_scan_us);
  params.apply_delta_max_rows = ParseJsonUint64(
      content, "apply_delta_max_rows", params.apply_delta_max_rows);
  params.apply_delta_max_part_fanout = ParseJsonUint64(
      content, "apply_delta_max_part_fanout",
      params.apply_delta_max_part_fanout);
  params.invalidation_overhead_us = ParseJsonDouble(
      content, "invalidation_overhead_us", params.invalidation_overhead_us);
  params.decision_margin_us = ParseJsonDouble(
      content, "decision_margin_us", params.decision_margin_us);
  params.min_samples =
      ParseJsonUint64(content, "min_samples", params.min_samples);
  params.incremental_failure_prob = ParseJsonDouble(
      content, "incremental_failure_prob", params.incremental_failure_prob);
  params.interference_rebuild_per_query_us = ParseJsonDouble(
      content, "interference_rebuild_per_query_us",
      params.interference_rebuild_per_query_us);
  params.interference_rebuild_scan_byte_us = ParseJsonDouble(
      content, "interference_rebuild_scan_byte_us",
      params.interference_rebuild_scan_byte_us);
  params.interference_delta_per_query_us = ParseJsonDouble(
      content, "interference_delta_per_query_us",
      params.interference_delta_per_query_us);
  params.interference_invalidate_per_query_us = ParseJsonDouble(
      content, "interference_invalidate_per_query_us",
      params.interference_invalidate_per_query_us);
  params.interference_query_slowdown_factor = ParseJsonDouble(
      content, "interference_query_slowdown_factor",
      params.interference_query_slowdown_factor);
  return params;
}

} // namespace accelerator_codegen::aux_state::core
