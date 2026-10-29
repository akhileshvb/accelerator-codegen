#include "backends/duckdb/tests/harness/common.hpp"

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "eval/bench/driver/bench_cli.hpp"
#include "eval/bench/driver/measured_cpus.hpp"
#include "eval/bench/driver/profile_window.hpp"

#include <iomanip>

namespace h = accelerator_codegen::duckdb_harness;
namespace driver = accelerator_codegen::benchmarks::driver;

namespace {
std::string PrivateCopy(const std::string &source) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dest = h::TempDirectory() / ("accel_benchmark_" + std::to_string(stamp) + ".duckdb");
  std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing);
  if (std::filesystem::exists(source + ".wal")) std::filesystem::copy_file(source + ".wal", dest.string() + ".wal");
  return dest.string();
}

void Consume(duckdb::Connection &connection, const std::string &sql) {
  auto result = h::Query(connection, sql);
  for (duckdb::idx_t row = 0; row < result->RowCount(); ++row)
    for (duckdb::idx_t col = 0; col < result->ColumnCount(); ++col) (void)result->GetValue(col, row).ToString();
}

std::vector<int> Frequencies(int fallback, bool explicit_frequency) {
  if (explicit_frequency) return {fallback};
  const char *env = std::getenv("ACCEL_AUX_WRITE_FREQUENCIES");
  if (!env || !*env) return {fallback};
  std::vector<int> points;
  std::istringstream input(env);
  for (std::string point; std::getline(input, point, ',');) points.push_back(std::stoi(point));
  return points;
}
}

int main(int argc, char **argv) {
  try {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
      std::cout << "Usage: benchmark_duckdb_<benchmark>_aux --query <N> <write_pct>\n";
      return 0;
    }
    driver::Args args(argc, argv);
    const auto &entry = h::Lookup(args.query);
    const bool explicit_frequency = !args.positional.empty();
    const int fallback_pct = args.positional.empty() ? 0 : std::stoi(args.positional.front());
    const auto sql = h::SqlPair(entry);
    const std::string &ref = sql.first;
    const std::string &accel = sql.second;
    const int warmup = std::max(0, driver::EnvInt("ACCEL_BENCH_WARMUP", 2));
    const int count = std::max(1, driver::EnvInt("ACCEL_BENCH_QUERIES", 5));
    std::cout << "suite,write_frequency_pct,workload_speedup,cold_start_workload_speedup,query_only_speedup,"
                 "blended_query_speedup,cold_start_accel_workload_us,setup_full_build_ms,accel_hit_rate,writes_executed,"
                 "duckdb_workload_us,accel_workload_us,num_queries,accel_fallback_reads,state_builds,"
                 "engine_ref_query_median_us,accel_hit_median_us\n";
    for (const int frequency : Frequencies(fallback_pct, explicit_frequency)) {
      const auto path = PrivateCopy(h::DbPath());
      double build_ms = 0.0;
      double ref_workload_us = 0.0;
      double accel_workload_us = 0.0;
      double ref_us = 0.0;
      double accel_us = 0.0;
      double hit_rate = 0.0;
      unsigned long long measured_fallbacks = 0;
      unsigned long long measured_queries = 0;
      unsigned long long writes = 0;
      unsigned long long builds = 0;
      {
        auto database = h::OpenDatabase(path);
        auto connection = h::Connect(*database);
        auto reference_connection = h::Connect(*database);
        accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime runtime(*connection, entry, 1.0);
        const auto build_start = driver::Clock::now();
        runtime.Build();
        build_ms = driver::ElapsedUs(build_start, driver::Clock::now()) / 1000.0;

        std::vector<accelerator_codegen::aux_state::engine::SharedProbe> probes;
        if (entry.probe_select_sql && *entry.probe_select_sql && entry.build_probe) {
          auto result = h::Query(*connection, accelerator_codegen::aux_state::engine::ExpandProbeSelectLimit(
                                                  entry.probe_select_sql, 8));
          for (duckdb::idx_t row = 0; row < result->RowCount(); ++row)
            probes.push_back(entry.build_probe(*result, row));
        }
        driver::PinToMeasuredCpus(1);
        for (int i = 0; i < warmup; ++i) {
          if ((i & 1) == 0) { Consume(*reference_connection, ref); Consume(*connection, accel); }
          else { Consume(*connection, accel); Consume(*reference_connection, ref); }
        }
        std::vector<double> ref_samples, accel_samples;
        ref_samples.reserve(count);
        accel_samples.reserve(count);
        for (int i = 0; i < count; ++i) {
          if ((i & 1) == 0) {
            auto start = driver::Clock::now();
            Consume(*reference_connection, ref);
            ref_samples.push_back(driver::ElapsedUs(start, driver::Clock::now()));
            start = driver::Clock::now();
            Consume(*connection, accel);
            accel_samples.push_back(driver::ElapsedUs(start, driver::Clock::now()));
          } else {
            auto start = driver::Clock::now();
            Consume(*connection, accel);
            accel_samples.push_back(driver::ElapsedUs(start, driver::Clock::now()));
            start = driver::Clock::now();
            Consume(*reference_connection, ref);
            ref_samples.push_back(driver::ElapsedUs(start, driver::Clock::now()));
          }
        }
        const auto reads_before = runtime.Stats();
        const auto ref_start = driver::Clock::now();
        for (int i = 0; i < count; ++i) Consume(*reference_connection, ref);
        ref_workload_us = driver::ElapsedUs(ref_start, driver::Clock::now());
        {
          driver::ProfileWindow profile_window;
          const int interval = frequency <= 0 ? count + 1 : std::max(1, static_cast<int>(std::lround(100.0 / frequency)));
          std::size_t probe_index = 0;
          const auto accel_start = driver::Clock::now();
          for (int i = 0; i < count; ++i) {
            if (frequency > 0 && !probes.empty() && (i + 1) % interval == 0) {
              runtime.SetExpectedQueriesBeforeNextWrite(static_cast<double>(interval));
              runtime.Apply(probes[probe_index++ % probes.size()]);
              ++writes;
            }
            Consume(*connection, accel);
          }
          accel_workload_us = driver::ElapsedUs(accel_start, driver::Clock::now());
        }
        const auto reads_after = runtime.Stats();
        measured_queries = reads_after.reads_hit + reads_after.reads_fallback -
                           reads_before.reads_hit - reads_before.reads_fallback;
        measured_fallbacks = reads_after.reads_fallback - reads_before.reads_fallback;
        const auto measured_hits = reads_after.reads_hit - reads_before.reads_hit;
        hit_rate = measured_queries ? static_cast<double>(measured_hits) / measured_queries : 0.0;
        builds = reads_after.builds;
        ref_us = driver::Median(ref_samples);
        accel_us = driver::Median(accel_samples);
      }
      const double speedup = accel_workload_us > 0 ? ref_workload_us / accel_workload_us : 0;
      const double query_only_speedup = accel_us > 0 ? ref_us / accel_us : 0;
      const double cold_speedup = build_ms * 1000.0 + accel_workload_us > 0
                                      ? ref_workload_us / (build_ms * 1000.0 + accel_workload_us) : 0;
      const std::string suite_env = "ACCEL_Q" + std::to_string(args.query) + "_BENCH_SUITE";
      const char *suite = std::getenv(suite_env.c_str());
      std::cout << std::setprecision(12) << (suite && *suite ? suite : "default")
                << ',' << frequency << ',' << speedup << ',' << cold_speedup << ',' << query_only_speedup
                << ',' << speedup << ',' << (build_ms * 1000.0 + accel_workload_us) << ',' << build_ms << ','
                << hit_rate << ',' << writes << ',' << ref_workload_us << ',' << accel_workload_us << ','
                << measured_queries << ',' << measured_fallbacks << ',' << builds << ','
                << ref_us << ',' << accel_us << '\n';
      std::filesystem::remove(path);
      std::filesystem::remove(path + ".wal");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
