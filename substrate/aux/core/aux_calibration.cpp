// SCAFFOLDING
// Cost-model calibration (hook-timed and write-driven fits) and the cost-model JSON.
#include "substrate/aux/core/aux_calibration.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include "substrate/aux/maintenance/json_params.hpp"

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

// Median time of `callback` over `samples` runs after `warmups` runs. Before
// each run, `hooks.reset` and then `prepare` run untimed.
double Measure(const std::function<void()> &callback,
               const AuxCalibrationHooks &hooks, std::size_t warmups,
               std::size_t samples,
               const std::function<void()> &prepare = {}) {
  if (!callback) {
    return 0.0;
  }
  auto untimed_setup = [&] {
    if (hooks.reset) {
      hooks.reset();
    }
    if (prepare) {
      prepare();
    }
  };
  for (std::size_t sample = 0; sample < warmups; ++sample) {
    untimed_setup();
    callback();
  }
  std::vector<double> timings;
  timings.reserve(samples);
  for (std::size_t sample = 0; sample < samples; ++sample) {
    untimed_setup();
    const auto start = Clock::now();
    callback();
    timings.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - start).count());
  }
  return Median(std::move(timings));
}

// One `"key": value` line of the cost-model JSON.
template <typename T> void WriteField(std::ostream &out, const char *key, T value, bool comma = true) {
  out << "  \"" << key << "\": " << value << (comma ? "," : "") << '\n';
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
              config.measurement_samples, hooks.prepare_maintenance);
  result.full_rebuild_us =
      Measure(hooks.rebuild_all, hooks, config.warmup_samples,
              config.measurement_samples, hooks.prepare_maintenance);
  result.invalidation_overhead_us =
      Measure(hooks.invalidate, hooks, config.warmup_samples,
              config.measurement_samples, hooks.prepare_maintenance);

  std::vector<std::pair<std::size_t, double>> delta_samples;
  for (const auto row_count : config.delta_row_counts) {
    const auto timing = Measure(
        [&, row_count] { hooks.apply_delta(row_count); },
        hooks, config.warmup_samples, config.measurement_samples,
        hooks.prepare_maintenance);
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
  // The delta hook is timed end to end, so its fitted intercept goes in the
  // policy's fixed overhead term; the base term stays 0 (it would count twice).
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
  WriteField(out, "aux_hit_us", params.aux_hit_us);
  WriteField(out, "fallback_us", params.fallback_us);
  WriteField(out, "partition_rebuild_base_us", params.partition_rebuild_base_us);
  WriteField(out, "partition_rebuild_us", params.partition_rebuild_us);
  WriteField(out, "full_rebuild_us", params.full_rebuild_us);
  WriteField(out, "first_query_blocking_us", params.first_query_blocking_us);
  WriteField(out, "apply_delta_overhead_us", params.apply_delta_overhead_us);
  WriteField(out, "apply_delta_base_us", params.apply_delta_base_us);
  WriteField(out, "apply_delta_per_row_us", params.apply_delta_per_row_us);
  WriteField(out, "apply_delta_max_rows", params.apply_delta_max_rows);
  WriteField(out, "calibrated_scale_rows", params.calibrated_scale_rows);
  WriteField(out, "apply_delta_max_part_fanout",
            params.apply_delta_max_part_fanout);
  WriteField(out, "invalidation_overhead_us", params.invalidation_overhead_us);
  WriteField(out, "decision_margin_us", params.decision_margin_us);
  WriteField(out, "min_samples", params.min_samples);
  WriteField(out, "incremental_failure_prob", params.incremental_failure_prob);
  WriteField(out, "interference_rebuild_per_query_us",
              params.interference_rebuild_per_query_us);
  WriteField(out, "interference_rebuild_scan_byte_us",
              params.interference_rebuild_scan_byte_us);
  WriteField(out, "interference_delta_per_query_us",
              params.interference_delta_per_query_us);
  WriteField(out, "interference_invalidate_per_query_us",
              params.interference_invalidate_per_query_us);
  WriteField(out, "interference_query_slowdown_factor",
              params.interference_query_slowdown_factor);
  WriteField(out, "calibration_build_initial_state_us",
              result.build_initial_state_us);
  WriteField(out, "calibration_apply_delta_base_us",
              result.apply_delta_base_us);
  WriteField(out, "calibration_apply_delta_per_row_us",
              result.apply_delta_per_row_us);
  WriteField(out, "calibration_samples", result.samples, false);
  out << "}\n";
}

double MedianTimeUs(const std::function<void()> &run, std::size_t warmups, std::size_t samples,
                    const std::function<void()> &prepare) {
  return Measure(run, AuxCalibrationHooks {}, warmups, std::max<std::size_t>(1, samples), prepare);
}

WriteCalibrationResult CalibrateFromWrites(const std::vector<CalibrationWrite> &writes,
                                           CalibrationEngine &engine,
                                           const maintenance::CostModelParams &seed,
                                           int repeats) {
  using maintenance::MaintenanceAction;
  if (writes.empty()) {
    throw std::invalid_argument("calibration needs at least one write");
  }
  repeats = std::max(1, repeats);
  // Median over `repeats` of (cost(apply) + cost(revert)) / 2.
  auto timed_pair = [&](const std::string &apply, const std::string &revert,
                        std::optional<MaintenanceAction> forced,
                        const std::function<double(const CalibrationEngine::Timed &)> &cost) {
    std::vector<double> runs;
    for (int i = 0; i < repeats; ++i) {
      const double a = cost(engine.WriteThenRead(apply, forced));
      const double b = cost(engine.WriteThenRead(revert, forced));
      runs.push_back((a + b) / 2.0);
    }
    return Median(std::move(runs));
  };

  WriteCalibrationResult result;
  result.params = seed;
  result.params.calibrated_scale_rows = 0;
  const char *once_env = std::getenv("ACCEL_AUX_CALIBRATION_REBUILD_ONCE");
  const bool rebuild_once = once_env && std::string(once_env) == "1";
  std::vector<std::pair<std::size_t, double>> delta, partition;
  std::vector<double> all_runs;
  for (const auto &write : writes) {
    WriteCalibrationSample sample;
    sample.name = write.name;
    // The sizes the module reports to the cost model (an UPDATE is two delta
    // rows: old and new), from an unforced write.
    const auto unforced = engine.WriteThenRead(write.apply, std::nullopt);
    engine.WriteThenRead(write.revert, std::nullopt);
    if (!unforced.decision || !unforced.decision->valid) {
      continue; // the accelerator does not depend on this write
    }
    sample.rows = unforced.decision->rows_touched;
    result.params.calibrated_scale_rows =
        std::max(result.params.calibrated_scale_rows, unforced.decision->data_rows_hint);
    sample.partitions = unforced.decision->affected_partition_count;
    sample.control_us = timed_pair(write.control, write.control_revert, std::nullopt,
                                   [](const CalibrationEngine::Timed &t) { return t.us; });
    sample.hit_read_us = timed_pair(write.control, write.control_revert, std::nullopt,
                                    [](const CalibrationEngine::Timed &t) { return t.read_us; });
    auto net = [&](MaintenanceAction action) {
      return timed_pair(write.apply, write.revert, action,
                        [&](const CalibrationEngine::Timed &t) {
                          const double maintenance = t.decision ? t.decision->maintenance_us : 0.0;
                          return maintenance + std::max(0.0, t.read_us - sample.hit_read_us);
                        });
    };
    sample.apply_delta_us = net(MaintenanceAction::ApplyDelta);
    // ACCEL_AUX_CALIBRATION_REBUILD_ONCE=1: an accelerator whose rebuild does
    // not depend on the write and has one partition (a Verus kit) measures
    // its rebuild on the first write only (it is a full rebuild either way).
    if (rebuild_once && !result.samples.empty()) {
      sample.rebuild_partition_us = result.samples.front().rebuild_partition_us;
      sample.rebuild_all_us = result.samples.front().rebuild_all_us;
    } else {
      sample.rebuild_all_us = net(MaintenanceAction::RebuildAll);
      sample.rebuild_partition_us = rebuild_once ? sample.rebuild_all_us : net(MaintenanceAction::RebuildPartition);
    }
    if (sample.rows > 0) {
      delta.emplace_back(sample.rows, sample.apply_delta_us);
    }
    if (sample.partitions > 0) {
      partition.emplace_back(sample.partitions, sample.rebuild_partition_us);
    }
    all_runs.push_back(sample.rebuild_all_us);
    result.samples.push_back(sample);
  }
  if (delta.empty()) {
    result.unfitted.push_back("apply_delta");
  } else {
    const auto fit = FitLinearCost(delta);
    result.params.apply_delta_overhead_us = fit.intercept_us;
    result.params.apply_delta_base_us = 0.0;
    result.params.apply_delta_per_row_us = fit.per_row_us;
  }
  if (partition.empty()) {
    result.unfitted.push_back("rebuild_partition");
  } else {
    const auto fit = FitLinearCost(partition);
    result.params.partition_rebuild_base_us = fit.intercept_us;
    result.params.partition_rebuild_us = fit.per_row_us;
  }
  if (result.samples.empty()) {
    result.unfitted.push_back("rebuild_all");
  } else {
    result.params.full_rebuild_us = Median(std::move(all_runs));
  }
  return result;
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
  params.partition_rebuild_base_us = ParseJsonDouble(
      content, "partition_rebuild_base_us", params.partition_rebuild_base_us);
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
  params.apply_delta_max_rows = ParseJsonUint64(
      content, "apply_delta_max_rows", params.apply_delta_max_rows);
  params.calibrated_scale_rows = ParseJsonUint64(
      content, "calibrated_scale_rows", params.calibrated_scale_rows);
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

namespace {

struct CalibrationWritesRegistry {
  std::mutex mutex;
  std::map<std::pair<std::string, int>, CalibrationWritesFn> writes;
};

CalibrationWritesRegistry &TheCalibrationWrites() {
  static CalibrationWritesRegistry registry;
  return registry;
}

} // namespace

void RegisterCalibrationWrites(const std::string &workload, int query, CalibrationWritesFn writes) {
  auto &registry = TheCalibrationWrites();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.writes[{workload, query}] = writes;
}

std::vector<CalibrationWrite> CalibrationWritesFor(const std::string &workload, int query) {
  auto &registry = TheCalibrationWrites();
  CalibrationWritesFn writes = nullptr;
  {
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.writes.find({workload, query});
    if (it != registry.writes.end()) {
      writes = it->second;
    }
  }
  return writes ? writes() : std::vector<CalibrationWrite>{};
}

} // namespace accelerator_codegen::aux_state::core
