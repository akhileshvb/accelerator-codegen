#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "aux_benchmark_diagnostics.hpp"
#include "aux_benchmark_sweep_session.hpp"
#include "catalog/query_catalog.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_AUX_BENCH_QUERY_NUM
#error "ACCEL_AUX_BENCH_QUERY_NUM must be defined by the build system"
#endif

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
namespace bench_diag = accelerator_codegen::benchmarks;
namespace aux_sweep = accelerator_codegen::benchmarks::aux_sweep;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

constexpr int kQueryNum = ACCEL_AUX_BENCH_QUERY_NUM;

std::string GetTpchDbPath() {
  const char *env_path = std::getenv("ACCEL_TPCH_DB");
  if (env_path && *env_path) {
    return env_path;
  }
  return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

int GetEnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  return std::stoi(value);
}

std::string GetEnvString(const char *name, const std::string &fallback = "") {
  const char *value = std::getenv(name);
  return value ? value : fallback;
}

std::string QueryEnvName(const char *suffix) {
  return "ACCEL_Q" + std::to_string(kQueryNum) + "_" + suffix;
}

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "query returned null");
  auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
  for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
    for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
      (void)materialized.GetValue(col, row);
    }
  }
}

struct TimingStats {
  double median_us = 0.0;
  double mean_us = 0.0;
};

TimingStats SummarizeSamples(std::vector<double> samples_us) {
  TimingStats stats;
  if (samples_us.empty()) {
    return stats;
  }
  double sum = 0.0;
  for (const auto sample : samples_us) {
    sum += sample;
  }
  stats.mean_us = sum / static_cast<double>(samples_us.size());
  std::sort(samples_us.begin(), samples_us.end());
  const auto middle = samples_us.size() / 2;
  stats.median_us = samples_us.size() % 2 == 0
                        ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
                        : samples_us[middle];
  return stats;
}

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

double ExpectedQueriesBeforeWrite(int write_frequency_pct) {
  if (write_frequency_pct <= 0) {
    return 1000.0;
  }
  return 100.0 / static_cast<double>(write_frequency_pct);
}

bool ShouldWriteAtQueryIndex(int query_index, int write_frequency_pct) {
  if (write_frequency_pct <= 0) {
    return false;
  }
  if (write_frequency_pct == 1) {
    return query_index % 100 == 0;
  }
  if (write_frequency_pct == 10) {
    return query_index % 10 == 0;
  }
  if (write_frequency_pct == 50) {
    return query_index % 2 == 0;
  }
  const int stride = 100 / write_frequency_pct;
  return stride > 0 && (query_index % stride) == 0;
}

std::vector<catalog::SharedProbe>
PrecomputeWriteProbes(duckdb::Connection &connection,
                      const catalog::SharedQueryCatalogEntry &entry,
                      int query_num) {
  auto probes = catalog::CollectInvalidatingSharedProbes(
      connection, entry, /*candidate_limit=*/256, /*max_keep=*/64);
  Require(!probes.empty(),
          "failed to precompute invalidating Q" + std::to_string(query_num) +
              " write probes");
  return probes;
}

catalog::SharedProbe PickWriteProbe(
    const std::vector<catalog::SharedProbe> &probes, int write_seq) {
  return probes[static_cast<size_t>(write_seq) % probes.size()];
}

void ApplyLineitemWrite(duckdb::Connection &connection,
                        const catalog::SharedProbe &probe) {
  connection.BeginTransaction();
  auto result = connection.Query(probe.update_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "lineitem write failed");
  connection.Commit();
}

double TimeBaseWriteCommitUs(duckdb::Connection &connection,
                             const catalog::SharedProbe &probe) {
  const auto start = Clock::now();
  ApplyLineitemWrite(connection, probe);
  return ElapsedUs(start, Clock::now());
}

double TimeAccelWriteCommitUs(DuckdbRuntime &runtime,
                              catalog::SharedProbe &probe) {
  const auto start = Clock::now();
  runtime.Apply(probe);
  return ElapsedUs(start, Clock::now());
}

double TimeExecuteUs(duckdb::Connection &connection, const std::string &aux_sql) {
  const auto start = Clock::now();
  ExecuteAndConsume(connection, aux_sql);
  return ElapsedUs(start, Clock::now());
}

double MeasureQueryMedianUs(duckdb::Connection &connection,
                            const std::string &sql, int samples) {
  if (samples <= 0) {
    return 0.0;
  }
  const std::string profile_path =
      "/tmp/accel_aux_bench_profile_" +
      std::to_string(static_cast<long long>(::getpid())) + "_single.json";
  {
    auto enabled = connection.Query("PRAGMA enable_profiling='json'");
    Require(enabled != nullptr && !enabled->HasError(),
            enabled ? enabled->GetError() : "enable_profiling failed");
  }
  {
    auto out = connection.Query("PRAGMA profiling_output='" + profile_path + "'");
    Require(out != nullptr && !out->HasError(),
            out ? out->GetError() : "profiling_output failed");
  }

  auto parse_latency_us = [&]() {
    std::ifstream in(profile_path);
    Require(in.good(), "cannot open DuckDB profile json: " + profile_path);
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    const auto key_pos = text.find("\"latency\"");
    Require(key_pos != std::string::npos, "latency missing in profile json");
    const auto colon_pos = text.find(':', key_pos);
    Require(colon_pos != std::string::npos, "latency value missing");
    char *end = nullptr;
    const double seconds = std::strtod(text.c_str() + colon_pos + 1, &end);
    Require(end != text.c_str() + colon_pos + 1, "failed to parse latency");
    return seconds * 1e6;
  };

  std::vector<double> query_samples;
  query_samples.reserve(samples);
  for (int i = 0; i < samples; i++) {
    ExecuteAndConsume(connection, sql);
    query_samples.push_back(parse_latency_us());
  }
  connection.Query("PRAGMA disable_profiling");
  std::remove(profile_path.c_str());
  return SummarizeSamples(std::move(query_samples)).median_us;
}

struct WorkloadTimings {
  int num_queries = 0;
  int writes_executed = 0;
  double duckdb_ref_query_median_us = 0.0;
  double base_kernel_median_us = 0.0;
  double base_write_median_us = 0.0;
  double accel_hit_median_us = 0.0;
  double fallback_median_us = 0.0;
  double accel_hit_rate = 0.0;
  double fallback_rate = 0.0;
  double total_duckdb_query_us = 0.0;
  double total_accel_query_us = 0.0;
  double total_base_write_us = 0.0;
  double total_accel_write_us = 0.0;
};

WorkloadTimings RunMeasuredWorkload(DuckdbRuntime &runtime,
                                    duckdb::Connection &accel_connection,
                                    const std::string &aux_sql, int num_queries,
                                    int write_frequency_pct,
                                    std::vector<catalog::SharedProbe> &probes) {
  WorkloadTimings result;
  result.num_queries = num_queries;

  int write_seq = 0;
  std::vector<double> hit_samples;
  hit_samples.reserve(num_queries);
  std::vector<double> write_samples;

  for (int query_index = 0; query_index < num_queries; query_index++) {
    if (ShouldWriteAtQueryIndex(query_index, write_frequency_pct)) {
      auto probe = PickWriteProbe(probes, write_seq);
      write_samples.push_back(TimeAccelWriteCommitUs(runtime, probe));
      write_seq++;
      result.writes_executed++;
    }

    const auto sample = TimeExecuteUs(accel_connection, aux_sql);
    result.total_accel_query_us += sample;
    hit_samples.push_back(sample);
  }

  result.total_accel_write_us = 0.0;
  for (const auto sample : write_samples) {
    result.total_accel_write_us += sample;
  }
  result.accel_hit_rate = 1.0;
  result.fallback_rate = 0.0;
  result.accel_hit_median_us = hit_samples.empty() ? 0.0
                                                   : SummarizeSamples(hit_samples).median_us;
  result.fallback_median_us = 0.0;
  return result;
}

void EnsureHybridMaintenanceMode() {
  const auto env_name = QueryEnvName("MAINTENANCE_MODE");
  if (!std::getenv(env_name.c_str())) {
    setenv(env_name.c_str(), "hybrid", 0);
  }
}

std::vector<int> ParseFrequencyList(const std::string &value) {
  std::vector<int> frequencies;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) {
      frequencies.push_back(std::stoi(item));
    }
  }
  return frequencies;
}

void PrintCsvHeader(std::ostream &out) {
  out << "suite,write_policy,locality,write_frequency_pct,query_num,"
         "num_queries,writes_executed,duckdb_ref_query_median_us,"
         "base_kernel_median_us,base_write_median_us,accel_write_median_us,"
         "accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,"
         "total_duckdb_query_us,total_accel_query_us,total_base_write_us,"
         "total_accel_write_us,duckdb_workload_us,accel_workload_us,"
         "workload_speedup,cold_start_workload_speedup,query_only_speedup,"
         "blended_query_speedup,cold_start_accel_workload_us,setup_full_build_ms,"
      << bench_diag::kAuxStoreDiagnosticsCsvHeader << "\n";
}

void RunBenchmarkPoint(int write_frequency_pct, const std::string &db_path,
                       const std::string &suite, const std::string &locality,
                       int warmup_queries, int measured_queries, int ref_samples,
                       int base_write_samples, bool print_header) {
  const auto &entry = catalog::LookupSharedQuery("tpch", kQueryNum);
  Require(suite == "lineitem_only", "unsupported bench suite: " + suite);
  constexpr const char *kWritePolicy = "hybrid";

  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection base_connection(database);
  duckdb::Connection accel_connection(database);
  Require(base_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
  Require(accel_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");

  accelerator_codegen::RegisterAcceleratorFunctions(base_connection);
  accelerator_codegen::RegisterAcceleratorFunctions(accel_connection);

  const std::string reference_sql = entry.base_sql;
  const std::string base_kernel_sql = entry.postgres_base_kernel_sql;
  const std::string aux_sql = entry.duckdb_aux_sql;
  auto write_probes = PrecomputeWriteProbes(base_connection, entry, kQueryNum);

  double base_write_median_us = 0.0;
  if (base_write_samples > 0) {
    for (int i = 0; i < base_write_samples; i++) {
      const auto &probe = PickWriteProbe(write_probes, 100000 + i);
      const auto sample = TimeBaseWriteCommitUs(base_connection, probe);
      base_write_median_us += sample;
    }
    base_write_median_us /= static_cast<double>(base_write_samples);
  }

  const double expected_queries_before_write =
      ExpectedQueriesBeforeWrite(write_frequency_pct);
  DuckdbRuntime runtime(accel_connection, entry, expected_queries_before_write);

  const auto build_start = Clock::now();
  runtime.Build();
  const double setup_full_build_ms =
      static_cast<double>(
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                build_start)
              .count());
  const uint64_t aux_state_bytes =
      bench_diag::CollectAuxStateFootprintBytes(runtime);

  for (int i = 0; i < warmup_queries; i++) {
    if (ref_samples > 0) {
      ExecuteAndConsume(base_connection, reference_sql);
    }
    if (i < std::min(warmup_queries, 2)) {
      ExecuteAndConsume(base_connection, base_kernel_sql);
    }
    ExecuteAndConsume(accel_connection, aux_sql);
  }

  const double duckdb_ref_query_median_us =
      MeasureQueryMedianUs(base_connection, reference_sql, ref_samples);
  const double base_kernel_median_us =
      MeasureQueryMedianUs(base_connection, base_kernel_sql,
                           ref_samples > 0 ? std::min(ref_samples, 2) : 0);

  runtime.ResetStoreDiagnosticsProfiling();
  auto workload = RunMeasuredWorkload(runtime, accel_connection, aux_sql,
                                      measured_queries, write_frequency_pct,
                                      write_probes);
  const auto store_diag = bench_diag::CollectStoreDiagnostics(runtime);
  workload.duckdb_ref_query_median_us = duckdb_ref_query_median_us;
  workload.base_kernel_median_us = base_kernel_median_us;
  workload.base_write_median_us = base_write_median_us;
  workload.total_duckdb_query_us =
      duckdb_ref_query_median_us * static_cast<double>(measured_queries);
  workload.total_base_write_us =
      base_write_median_us * static_cast<double>(workload.writes_executed);

  const double duckdb_workload_us =
      workload.total_duckdb_query_us + workload.total_base_write_us;
  const double accel_workload_us =
      workload.total_accel_query_us + workload.total_accel_write_us;
  const double workload_speedup =
      accel_workload_us > 0.0 ? duckdb_workload_us / accel_workload_us : 0.0;
  const double query_only_speedup =
      workload.accel_hit_median_us > 0.0
          ? base_kernel_median_us / workload.accel_hit_median_us
          : 0.0;
  const double blended_query_us =
      workload.accel_hit_rate * workload.accel_hit_median_us +
      workload.fallback_rate *
          (workload.fallback_median_us > 0.0 ? workload.fallback_median_us
                                             : base_kernel_median_us);
  const double blended_query_speedup =
      blended_query_us > 0.0 ? base_kernel_median_us / blended_query_us : 0.0;
  const double cold_start_accel_workload_us =
      setup_full_build_ms * 1000.0 + accel_workload_us;
  const double cold_start_workload_speedup =
      cold_start_accel_workload_us > 0.0
          ? duckdb_workload_us / cold_start_accel_workload_us
          : 0.0;

  if (print_header) {
    PrintCsvHeader(std::cout);
  }

  const auto accel_write_median_us = workload.writes_executed > 0
                                         ? workload.total_accel_write_us /
                                               workload.writes_executed
                                         : 0.0;

  std::cout << suite << ',' << kWritePolicy << ',' << locality << ','
            << write_frequency_pct << ',' << kQueryNum << ','
            << workload.num_queries << ',' << workload.writes_executed << ','
            << std::fixed << std::setprecision(2)
            << workload.duckdb_ref_query_median_us << ','
            << workload.base_kernel_median_us << ','
            << workload.base_write_median_us << ',' << accel_write_median_us
            << ',' << workload.accel_hit_median_us << ','
            << workload.fallback_median_us << ',' << workload.accel_hit_rate
            << ',' << workload.fallback_rate << ','
            << workload.total_duckdb_query_us << ','
            << workload.total_accel_query_us << ','
            << workload.total_base_write_us << ','
            << workload.total_accel_write_us << ',' << duckdb_workload_us
            << ',' << accel_workload_us << ',' << workload_speedup << ','
            << cold_start_workload_speedup << ',' << query_only_speedup << ','
            << blended_query_speedup << ',' << cold_start_accel_workload_us
            << ',' << setup_full_build_ms << ',';
  bench_diag::AppendStoreDiagnosticsCsv(std::cout, aux_state_bytes, store_diag);
  std::cout << '\n' << std::flush;
}

} // namespace

int main() {
  try {
    EnsureHybridMaintenanceMode();

    const int warmup_queries = GetEnvInt("ACCEL_BENCH_WARMUP", 10);
    const int measured_queries = GetEnvInt("ACCEL_BENCH_QUERIES", 100);
    const int ref_samples = GetEnvInt("ACCEL_BENCH_REF_SAMPLES", 5);
    const int base_write_samples = GetEnvInt("ACCEL_BENCH_BASE_WRITE_SAMPLES", 3);
    const auto suite_env = QueryEnvName("BENCH_SUITE");
    const auto locality_env = QueryEnvName("WRITE_LOCALITY");
    const auto suite = GetEnvString(suite_env.c_str(), "lineitem_only");
    const auto locality = GetEnvString(locality_env.c_str(), "global");
    const auto source_db_path = GetTpchDbPath();

    if (std::getenv("ACCEL_AUX_WRITE_INTERVALS") ||
        std::getenv("ACCEL_AUX_WRITE_FREQUENCIES")) {
      aux_sweep::RunConfiguredMultiSweep(kQueryNum, source_db_path, suite, locality,
                                         warmup_queries, measured_queries, ref_samples,
                                         base_write_samples);
      return EXIT_SUCCESS;
    }

    const int write_frequency_pct =
        GetEnvInt(QueryEnvName("WRITE_FREQUENCY_PCT").c_str(), 0);
    RunBenchmarkPoint(write_frequency_pct, source_db_path, suite, locality,
                      warmup_queries, measured_queries, ref_samples,
                      base_write_samples, true);
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "benchmark_tpch_q" << kQueryNum
              << "_aux failed: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
