#include "backends/duckdb/tests/harness/common.hpp"

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "eval/bench/driver/bench_cli.hpp"
#include "substrate/aux/core/aux_calibration.hpp"

#include <atomic>

namespace h = accelerator_codegen::duckdb_harness;
namespace driver = accelerator_codegen::benchmarks::driver;

int main(int argc, char **argv) {
  try {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
      std::cout << "Usage: calibrate_duckdb_<benchmark>_aux_cost_model --query <N> <cost_model.json>\n";
      return 0;
    }
    driver::Args args(argc, argv);
    h::Require(!args.positional.empty(), "usage: --query <N> <cost_model.json>");
    const auto &entry = h::Lookup(args.query);
    auto database = h::OpenDatabase();
    auto connection = h::Connect(*database);
    accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime runtime(*connection, entry, 1.0);
    runtime.Build();
    const std::string query_sql = h::SqlPair(entry).second;
    std::vector<double> isolated;
    const int samples = std::max(1, driver::EnvInt("ACCEL_INTERFERENCE_ISOLATED_SAMPLES", 5));
    for (int i = 0; i < samples; ++i) {
      const auto start = driver::Clock::now();
      h::Query(*connection, query_sql);
      isolated.push_back(driver::ElapsedUs(start, driver::Clock::now()));
    }
    const double isolated_us = driver::Median(isolated);
    for (int i = 0; i < 2; ++i) {
      runtime.InvalidateAll();
      runtime.Build();
    }
    const int readers = std::max(1, driver::EnvInt("ACCEL_MULTI_CONN_TARGET_READERS", 3));
    const int duration = std::max(1, driver::EnvInt("ACCEL_INTERFERENCE_CALIB_SEC", 2));
    std::atomic<bool> stop{false};
    std::atomic<int> sample_phase{0};
    std::atomic<std::uint64_t> rebuild_epoch{0};
    std::atomic<int> active_queries{0};
    std::mutex sample_mutex;
    std::vector<double> baseline_query_samples, rebuild_query_samples;
    std::vector<std::thread> workers;
    for (int i = 0; i < readers; ++i) {
      workers.emplace_back([&] {
        auto worker = h::Connect(*database);
        while (!stop.load(std::memory_order_relaxed)) {
          const int phase = sample_phase.load(std::memory_order_relaxed);
          const auto epoch = rebuild_epoch.load(std::memory_order_relaxed);
          active_queries.fetch_add(1, std::memory_order_relaxed);
          const auto start = driver::Clock::now();
          h::Query(*worker, query_sql);
          const double elapsed = driver::ElapsedUs(start, driver::Clock::now());
          active_queries.fetch_sub(1, std::memory_order_relaxed);
          const bool overlapped_rebuild =
              phase == 1 || sample_phase.load(std::memory_order_relaxed) == 1 ||
              epoch != rebuild_epoch.load(std::memory_order_relaxed);
          if (overlapped_rebuild || phase == 0) {
            std::lock_guard<std::mutex> lock(sample_mutex);
            auto &phase_samples = overlapped_rebuild ? rebuild_query_samples : baseline_query_samples;
            if (phase_samples.size() < 8192) phase_samples.push_back(elapsed);
          }
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sample_phase.store(2, std::memory_order_relaxed);
    std::vector<double> concurrent;
    for (int i = 0; i < samples; ++i) {
      const auto start = driver::Clock::now();
      h::Query(*connection, query_sql);
      concurrent.push_back(driver::ElapsedUs(start, driver::Clock::now()));
    }
    const double concurrent_us = driver::Median(concurrent);
    std::vector<double> rebuild_cost_per_query;
    for (int i = 0; i < 2; ++i) {
      sample_phase.store(1, std::memory_order_relaxed);
      rebuild_epoch.fetch_add(1, std::memory_order_relaxed);
      for (int wait = 0; wait < 1000 && active_queries.load(std::memory_order_relaxed) == 0; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const int readers_in_flight = active_queries.load(std::memory_order_relaxed);
      const auto rebuild_start = driver::Clock::now();
      runtime.InvalidateAll();
      runtime.Build();
      const double rebuild_elapsed = driver::ElapsedUs(rebuild_start, driver::Clock::now());
      const int readers_after = active_queries.load(std::memory_order_relaxed);
      const int overlapping_readers = std::max(1, std::max(readers_in_flight, readers_after));
      rebuild_cost_per_query.push_back(rebuild_elapsed / overlapping_readers);
      rebuild_epoch.fetch_add(1, std::memory_order_relaxed);
      sample_phase.store(2, std::memory_order_relaxed);
    }
    std::this_thread::sleep_for(std::chrono::seconds(duration));
    stop.store(true, std::memory_order_relaxed);
    for (auto &worker : workers) worker.join();
    const double query_delta = std::max(0.0, concurrent_us - isolated_us);
    const double baseline_query_us = baseline_query_samples.empty()
                                         ? concurrent_us
                                         : driver::Median(baseline_query_samples);
    const double rebuilding_query_us = rebuild_query_samples.empty()
                                           ? baseline_query_us
                                           : driver::Median(rebuild_query_samples);
    const double observed_query_slowdown = std::max(0.0, rebuilding_query_us - baseline_query_us);
    const double rebuild_delta = observed_query_slowdown > 0.0
                                     ? observed_query_slowdown
                                     : driver::Median(rebuild_cost_per_query);
    const auto writes = runtime.CalibrationWrites();
    accelerator_codegen::aux_state::core::AuxCalibrationResult result;
    if (!writes.empty()) {
      accelerator_codegen::aux_state::engine::duckdb::DuckdbCalibrationEngine calibration_engine(
          runtime, accelerator_codegen::aux_state::engine::DefaultArguments(entry));
      const auto calibrated = accelerator_codegen::aux_state::core::CalibrateFromWrites(
          writes, calibration_engine, {}, std::max(1, driver::EnvInt("ACCEL_INTERFERENCE_ISOLATED_SAMPLES", 2)));
      result.params = calibrated.params;
    }
    result.params.interference_delta_per_query_us = query_delta;
    result.params.interference_rebuild_per_query_us = rebuild_delta;
    result.params.interference_query_slowdown_factor = isolated_us > 0 ? concurrent_us / isolated_us : 0;
    accelerator_codegen::aux_state::core::WriteAuxCostModelJson(args.positional.front(), result, "duckdb",
                                                                entry.accelerator_id ? entry.accelerator_id : "");
    std::cout << "calibration written to " << args.positional.front() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "calibration failed: " << error.what() << '\n';
    return 1;
  }
}
