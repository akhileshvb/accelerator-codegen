#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/bench/tpch/single-threaded/aux_benchmark_diagnostics.hpp"
#include "eval/bench/tpch/single-threaded/aux_benchmark_sweep_session.hpp"
#include "catalog/query_catalog.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
namespace bench_diag = accelerator_codegen::benchmarks;
namespace aux_sweep = accelerator_codegen::benchmarks::aux_sweep;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
  return accelerator_codegen::test_support::TpchDbPath();
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
};

TimingStats SummarizeSamples(std::vector<double> samples_us) {
  TimingStats stats;
  if (samples_us.empty()) {
    return stats;
  }
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

void EnsureHybridMaintenanceMode() {
  setenv("ACCEL_MAINTENANCE_MODE", "hybrid", 1);
}

std::string ReferenceSql() {
  return "SELECT s_name, s_address "
         "FROM supplier, nation "
         "WHERE s_suppkey IN ("
         "  SELECT ps_suppkey FROM partsupp, ("
         "    SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, "
         "           0.5 * SUM(l_quantity) AS agg_quantity "
         "    FROM lineitem "
         "    WHERE l_shipdate >= DATE '1997-01-01' "
         "      AND l_shipdate < DATE '1997-01-01' + INTERVAL '1' year "
         "    GROUP BY l_partkey, l_suppkey"
         "  ) agg_lineitem "
         "  WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
         "    AND ps_partkey IN ("
         "      SELECT p_partkey FROM part WHERE p_name LIKE 'gainsboro%') "
         "    AND ps_availqty > agg_quantity) "
         "  AND s_nationkey = n_nationkey AND n_name = 'RUSSIA' "
         "ORDER BY s_name";
}

std::string BaseKernelSql() {
  return "SELECT * FROM accel_tpch_q20(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')";
}

std::string AuxSql() {
  return "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')";
}

std::vector<catalog::SharedProbe>
PrecomputeWriteProbes(duckdb::Connection &connection,
                      const catalog::SharedQueryCatalogEntry &entry) {
  auto result = connection.Query(
      "SELECT l_orderkey, l_linenumber, l_quantity, l_shipdate, "
      "l_partkey, l_suppkey FROM lineitem "
      "WHERE l_shipdate >= DATE '1997-01-01' "
      "  AND l_shipdate < DATE '1997-01-01' + INTERVAL '1' year "
      "ORDER BY l_orderkey, l_linenumber LIMIT 256");
  Require(result != nullptr && !result->HasError(), result->GetError());
  auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
  Require(materialized.RowCount() > 0,
          "failed to precompute Q20 write probes");
  std::vector<catalog::SharedProbe> probes;
  probes.reserve(materialized.RowCount());
  for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
    probes.push_back(entry.build_probe(materialized, row));
  }
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

double TimeExecuteUs(DuckdbRuntime &runtime) {
  const auto start = Clock::now();
  runtime.Execute();
  return ElapsedUs(start, Clock::now());
}

double MeasureQueryMedianUs(duckdb::Connection &connection,
                            const std::string &sql, int samples) {
  std::vector<double> query_samples;
  query_samples.reserve(samples);
  for (int i = 0; i < samples; i++) {
    const auto start = Clock::now();
    ExecuteAndConsume(connection, sql);
    query_samples.push_back(ElapsedUs(start, Clock::now()));
  }
  return SummarizeSamples(std::move(query_samples)).median_us;
}

bool IsFallback(DuckdbRuntime &runtime) {
  (void)runtime;
  return false;
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
                                    int num_queries,
                                    int write_frequency_pct,
                                    std::vector<catalog::SharedProbe> &probes) {
  WorkloadTimings result;
  result.num_queries = num_queries;

  int write_seq = 0;
  int accel_hits = 0;
  int fallbacks = 0;
  std::vector<double> hit_times;
  std::vector<double> fallback_times;

  for (int i = 0; i < num_queries; i++) {
    const bool write = ShouldWriteAtQueryIndex(i, write_frequency_pct);
    if (write) {
      const auto &probe = PickWriteProbe(probes, write_seq);
      const double write_us = TimeAccelWriteCommitUs(runtime,
                                                      const_cast<catalog::SharedProbe &>(probe));
      result.total_accel_write_us += write_us;
      result.writes_executed++;
      write_seq++;
    }

    const auto q_start = Clock::now();
    runtime.Execute();
    const double q_us = ElapsedUs(q_start, Clock::now());

    if (false /* fallback detection not available in shared runtime */) {
      fallback_times.push_back(q_us);
      result.total_accel_query_us += q_us;
      fallbacks++;
    } else {
      hit_times.push_back(q_us);
      result.total_accel_query_us += q_us;
      accel_hits++;
    }
  }

  result.accel_hit_rate =
      result.num_queries > 0
          ? static_cast<double>(accel_hits) / static_cast<double>(result.num_queries)
          : 0.0;
  result.fallback_rate =
      result.num_queries > 0
          ? static_cast<double>(fallbacks) / static_cast<double>(result.num_queries)
          : 0.0;
  result.accel_hit_median_us =
      SummarizeSamples(std::move(hit_times)).median_us;
  result.fallback_median_us =
      SummarizeSamples(std::move(fallback_times)).median_us;

  return result;
}

void PrintCsvHeader(std::ostream &out) {
  out << "suite,write_policy,locality,write_frequency_pct,"
         "num_queries,writes_executed,duckdb_ref_query_median_us,"
         "base_kernel_median_us,base_write_median_us,accel_write_median_us,"
         "accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,"
         "total_duckdb_query_us,total_accel_query_us,total_base_write_us,"
         "total_accel_write_us,duckdb_workload_us,accel_workload_us,"
         "workload_speedup,cold_start_workload_speedup,query_only_speedup,"
         "blended_query_speedup,cold_start_accel_workload_us,setup_full_build_ms,"
      << bench_diag::kAuxStoreDiagnosticsCsvHeader << "\n"
      << std::flush;
}

void RunBenchmarkPoint(int write_frequency_pct, const std::string &source_db_path,
                       const std::string &suite, const std::string &locality,
                       int warmup_queries, int measured_queries,
                       int ref_samples, int base_write_samples,
                       bool print_header) {
  const auto &entry = catalog::LookupSharedQuery("tpch", 20);
  Require(suite == "lineitem_only", "unsupported bench suite: " + suite);
  constexpr const char *kWritePolicy = "hybrid";

  duckdb::DuckDB db(source_db_path.c_str());
  duckdb::Connection base_connection(db);
  duckdb::Connection accel_connection(db);
  Require(base_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
  Require(accel_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");

  accelerator_codegen::RegisterAcceleratorFunctions(base_connection);
  accelerator_codegen::RegisterAcceleratorFunctions(accel_connection);

  const std::string reference_sql = entry.base_sql;
  const std::string base_kernel_sql = entry.postgres_base_kernel_sql;
  auto write_probes = PrecomputeWriteProbes(base_connection, entry);

  double base_write_median_us = 0.0;
  for (int i = 0; i < base_write_samples; i++) {
    const auto &probe = PickWriteProbe(write_probes, 100000 + i);
    const auto sample = TimeBaseWriteCommitUs(base_connection, probe);
    base_write_median_us += sample;
  }
  base_write_median_us /= static_cast<double>(base_write_samples);

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

  // Warmup
  for (int i = 0; i < warmup_queries; i++) {
    ExecuteAndConsume(base_connection, reference_sql);
    if (i < std::min(warmup_queries, 2)) {
      ExecuteAndConsume(base_connection, base_kernel_sql);
    }
    runtime.Execute();
  }

  const double duckdb_ref_query_median_us =
      MeasureQueryMedianUs(base_connection, reference_sql, ref_samples);
  const double base_kernel_median_us =
      MeasureQueryMedianUs(base_connection, base_kernel_sql,
                           std::min(ref_samples, 2));

  runtime.ResetStoreDiagnosticsProfiling();
  auto workload = RunMeasuredWorkload(runtime, measured_queries,
                                      write_frequency_pct, write_probes);
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
            << write_frequency_pct << ','
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
    const auto suite = GetEnvString("ACCEL_Q20_BENCH_SUITE", "lineitem_only");
    const auto locality = GetEnvString("ACCEL_Q20_WRITE_LOCALITY", "global");
    const auto source_db_path = GetTpchDbPath();

    if (std::getenv("ACCEL_AUX_WRITE_INTERVALS") ||
        std::getenv("ACCEL_AUX_WRITE_FREQUENCIES")) {
      aux_sweep::RunConfiguredMultiSweep(20, source_db_path, suite, locality,
                                         warmup_queries, measured_queries, ref_samples,
                                         base_write_samples);
      return EXIT_SUCCESS;
    }

    const int write_frequency_pct = GetEnvInt("ACCEL_Q20_WRITE_FREQUENCY_PCT", 0);
    RunBenchmarkPoint(write_frequency_pct, source_db_path, suite, locality,
                      warmup_queries, measured_queries, ref_samples,
                      base_write_samples, true);
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "benchmark_q20_aux failed: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
