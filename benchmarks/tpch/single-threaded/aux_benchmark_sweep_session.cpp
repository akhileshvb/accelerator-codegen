#include "aux_benchmark_sweep_session.hpp"

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "accelerator_codegen/runtime/duckdb/tpch_rf.hpp"
#include "aux_benchmark_diagnostics.hpp"
#include "catalog/query_catalog.hpp"
#include "tpch_arg_variants.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::benchmarks::aux_sweep {
namespace {

namespace catalog_ns = accelerator_codegen::aux_state::engine;
namespace bench_diag = accelerator_codegen::benchmarks;
using catalog_ns::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

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

// Root "latency" from DuckDB JSON profiling output (seconds → µs).
double ParseJsonProfileLatencyUs(const std::string &path) {
  std::ifstream in(path);
  Require(in.good(), "cannot open DuckDB profile json: " + path);
  std::ostringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();
  const auto key_pos = text.find("\"latency\"");
  Require(key_pos != std::string::npos, "latency missing in profile json");
  const auto colon_pos = text.find(':', key_pos);
  Require(colon_pos != std::string::npos, "latency value missing in profile json");
  char *end = nullptr;
  const double seconds = std::strtod(text.c_str() + colon_pos + 1, &end);
  Require(end != text.c_str() + colon_pos + 1 && std::isfinite(seconds) &&
              seconds >= 0.0,
          "failed to parse profile latency");
  return seconds * 1e6;
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
  return std::chrono::duration<double, std::micro>(end - start).count();
}

double ExpectedQueriesBeforeWrite(int write_frequency_pct) {
  if (write_frequency_pct <= 0) {
    return 1000.0;
  }
  return 100.0 / static_cast<double>(write_frequency_pct);
}

double ExpectedQueriesBeforeWriteInterval(double write_interval_sec,
                                          double reader_qps) {
  if (write_interval_sec <= 0.0) {
    return 1000.0;
  }
  return std::max(1.0, reader_qps * write_interval_sec);
}

int EffectiveWriteFrequencyPct(int64_t writes_executed, int64_t num_queries) {
  if (num_queries <= 0 || writes_executed <= 0) {
    return 0;
  }
  const double pct =
      100.0 * static_cast<double>(writes_executed) / static_cast<double>(num_queries);
  return static_cast<int>(std::min(100.0, std::round(pct)));
}

double RunDurationForInterval(double base_duration_sec, double write_interval_sec,
                              int min_writes) {
  if (write_interval_sec <= 0.0) {
    return base_duration_sec;
  }
  const double min_duration = static_cast<double>(min_writes) * write_interval_sec;
  return std::max(base_duration_sec, min_duration);
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

std::vector<catalog_ns::SharedProbe>
PrecomputeWriteProbes(duckdb::Connection &connection,
                      const catalog_ns::SharedQueryCatalogEntry &entry,
                      int query_num) {
  // Only keep catalog probes that actually change DuckDB's answer for this
  // query — otherwise write-frequency sweeps do not exercise maintenance.
  auto probes = catalog_ns::CollectInvalidatingSharedProbes(
      connection, entry, /*candidate_limit=*/256, /*max_keep=*/64);
  Require(!probes.empty(),
          "failed to precompute invalidating Q" + std::to_string(query_num) +
              " write probes");
  std::cerr << "tpch_q" << query_num << "_aux_bench: invalidating_write_probes="
            << probes.size() << '\n'
            << std::flush;
  return probes;
}

catalog_ns::SharedProbe PickWriteProbe(
    const std::vector<catalog_ns::SharedProbe> &probes, int write_seq) {
  return probes[static_cast<size_t>(write_seq) % probes.size()];
}

void ApplyLineitemWrite(duckdb::Connection &connection,
                        const catalog_ns::SharedProbe &probe) {
  connection.BeginTransaction();
  auto result = connection.Query(probe.update_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "lineitem write failed");
  connection.Commit();
}

double TimeBaseWriteCommitUs(duckdb::Connection &connection,
                             const catalog_ns::SharedProbe &probe) {
  const auto start = Clock::now();
  ApplyLineitemWrite(connection, probe);
  return ElapsedUs(start, Clock::now());
}

double TimeAccelWriteCommitUs(DuckdbRuntime &runtime,
                              catalog_ns::SharedProbe &probe) {
  const auto start = Clock::now();
  runtime.Apply(probe);
  return ElapsedUs(start, Clock::now());
}

enum class WriteMode { Point, Rf };

WriteMode ParseWriteMode() {
  const char *value = std::getenv("ACCEL_AUX_WRITE_MODE");
  if (!value || !*value) {
    return WriteMode::Point;
  }
  const std::string mode(value);
  if (mode == "point" || mode == "lineitem" || mode == "update") {
    return WriteMode::Point;
  }
  if (mode == "rf" || mode == "rf1_rf2" || mode == "refresh") {
    return WriteMode::Rf;
  }
  throw std::runtime_error(
      "ACCEL_AUX_WRITE_MODE must be 'point' (default) or 'rf'");
}

const char *WriteModeName(WriteMode mode) {
  return mode == WriteMode::Rf ? "rf" : "point";
}

int RfChunkOrderCount() {
  const char *value = std::getenv("ACCEL_TPCH_RF_CHUNK_ORDERS");
  if (!value || !*value) {
    return 1;
  }
  return std::max(1, std::stoi(value));
}

double TimeAccelRfWriteCommitUs(DuckdbRuntime &runtime, int write_seq,
                                int order_count) {
  const auto start = Clock::now();
  runtime.ApplyRfChunk(write_seq, order_count);
  return ElapsedUs(start, Clock::now());
}

double TimeExecuteUs(duckdb::Connection &connection, const std::string &aux_sql) {
  const auto start = Clock::now();
  ExecuteAndConsume(connection, aux_sql);
  return ElapsedUs(start, Clock::now());
}

double MeasureReaderQps(duckdb::Connection &connection,
                        const std::vector<TpchArgVariant> &arg_pool, int samples) {
  if (samples <= 0 || arg_pool.empty()) {
    return 1.0;
  }
  std::vector<double> query_samples;
  query_samples.reserve(samples);
  for (int i = 0; i < samples; i++) {
    query_samples.push_back(TimeExecuteUs(
        connection,
        arg_pool[static_cast<std::size_t>(i) % arg_pool.size()].aux_sql));
  }
  const double median_us = SummarizeSamples(std::move(query_samples)).median_us;
  if (median_us <= 0.0) {
    return 1.0;
  }
  return 1e6 / median_us;
}

double MeasureQueryMedianUs(duckdb::Connection &connection,
                            const std::vector<TpchArgVariant> &arg_pool,
                            bool use_kernel_sql, int samples) {
  if (samples <= 0 || arg_pool.empty()) {
    return 0.0;
  }

  // DuckDB reference timing uses JSON profiler root "latency" (query execution
  // time reported by DuckDB), not outer wall-clock around connection.Query.
  const std::string profile_path = "/tmp/accel_aux_bench_profile_" +
                                   std::to_string(static_cast<long long>(::getpid())) +
                                   (use_kernel_sql ? "_kernel" : "_base") + ".json";
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

  std::vector<double> query_samples;
  query_samples.reserve(samples);
  for (int i = 0; i < samples; i++) {
    const auto &variant =
        arg_pool[static_cast<std::size_t>(i) % arg_pool.size()];
    const std::string &sql =
        use_kernel_sql ? variant.kernel_sql : variant.base_sql;
    ExecuteAndConsume(connection, sql);
    query_samples.push_back(ParseJsonProfileLatencyUs(profile_path));
  }
  connection.Query("PRAGMA disable_profiling");
  std::remove(profile_path.c_str());
  return SummarizeSamples(std::move(query_samples)).median_us;
}

struct WorkloadTimings {
  int64_t num_queries = 0;
  int64_t writes_executed = 0;
  double accel_hit_median_us = 0.0;
  double fallback_median_us = 0.0;
  double accel_hit_rate = 0.0;
  double fallback_rate = 0.0;
  double total_accel_query_us = 0.0;
  double total_accel_write_us = 0.0;
};

const TpchArgVariant &PickArgVariant(const std::vector<TpchArgVariant> &arg_pool,
                                     std::size_t &arg_seq) {
  Require(!arg_pool.empty(), "arg pool is empty");
  return arg_pool[arg_seq++ % arg_pool.size()];
}

WorkloadTimings RunMeasuredWorkloadWallClock(
    DuckdbRuntime &runtime, duckdb::Connection &accel_connection,
    double duration_sec, double write_interval_sec,
    std::vector<catalog_ns::SharedProbe> &probes, WriteMode write_mode,
    int rf_chunk_orders, const std::vector<TpchArgVariant> &arg_pool) {
  WorkloadTimings result;
  if (duration_sec <= 0.0) {
    return result;
  }

  const auto start = Clock::now();
  const auto deadline =
      start + std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(duration_sec));
  double next_write_elapsed_sec =
      write_interval_sec > 0.0 ? write_interval_sec : 1e30;

  int write_seq = 0;
  std::size_t arg_seq = 0;
  // Cap sample vectors — ultra-fast kernels can execute >1e8 queries/hour.
  constexpr std::size_t kMaxSamples = 200000;
  std::vector<double> hit_samples;
  hit_samples.reserve(kMaxSamples);
  std::vector<double> write_samples;
  std::size_t hit_sample_seen = 0;

  auto elapsed_sec = [&]() {
    return std::chrono::duration<double>(Clock::now() - start).count();
  };

  auto push_hit_sample = [&](double sample_us) {
    ++hit_sample_seen;
    if (hit_samples.size() < kMaxSamples) {
      hit_samples.push_back(sample_us);
      return;
    }
    // Reservoir sample so median stays representative under high QPS.
    const std::size_t slot = static_cast<std::size_t>(hit_sample_seen % kMaxSamples);
    hit_samples[slot] = sample_us;
  };

  while (Clock::now() < deadline) {
    // At most one write per outer iteration. If a write is slower than the
    // interval, skip missed slots instead of catch-up looping forever
    // (write_latency > write_interval used to livelock Q3 at SF20).
    if (write_interval_sec > 0.0 &&
        elapsed_sec() + 1e-9 >= next_write_elapsed_sec) {
      // Do not start a write after the budget — RF full-chunk apply can run
      // for minutes and the deadline is only checked between iterations.
      if (Clock::now() >= deadline) {
        break;
      }
      const auto write_start = Clock::now();
      if (write_mode == WriteMode::Rf) {
        std::cerr << "tpch_aux_bench: RF write begin seq=" << write_seq
                  << " orders=" << rf_chunk_orders
                  << " elapsed_sec=" << elapsed_sec() << '\n'
                  << std::flush;
        write_samples.push_back(
            TimeAccelRfWriteCommitUs(runtime, write_seq++, rf_chunk_orders));
        std::cerr << "tpch_aux_bench: RF write end seq=" << (write_seq - 1)
                  << " write_sec="
                  << std::chrono::duration<double>(Clock::now() - write_start)
                         .count()
                  << '\n'
                  << std::flush;
      } else {
        auto probe = PickWriteProbe(probes, write_seq++);
        write_samples.push_back(TimeAccelWriteCommitUs(runtime, probe));
      }
      result.writes_executed++;
      next_write_elapsed_sec += write_interval_sec;
      const double now = elapsed_sec();
      while (next_write_elapsed_sec <= now) {
        next_write_elapsed_sec += write_interval_sec;
      }
    }

    if (Clock::now() >= deadline) {
      break;
    }

    const auto sample =
        TimeExecuteUs(accel_connection, PickArgVariant(arg_pool, arg_seq).aux_sql);
    result.total_accel_query_us += sample;
    push_hit_sample(sample);
    result.num_queries++;
  }

  for (const auto sample : write_samples) {
    result.total_accel_write_us += sample;
  }
  result.accel_hit_rate = 1.0;
  result.fallback_rate = 0.0;
  result.accel_hit_median_us = hit_samples.empty() ? 0.0
                                                   : SummarizeSamples(hit_samples).median_us;
  return result;
}

WorkloadTimings RunMeasuredWorkload(DuckdbRuntime &runtime,
                                    duckdb::Connection &accel_connection,
                                    int num_queries, int write_frequency_pct,
                                    std::vector<catalog_ns::SharedProbe> &probes,
                                    WriteMode write_mode, int rf_chunk_orders,
                                    const std::vector<TpchArgVariant> &arg_pool) {
  WorkloadTimings result;
  result.num_queries = num_queries;

  int write_seq = 0;
  std::size_t arg_seq = 0;
  std::vector<double> hit_samples;
  hit_samples.reserve(num_queries);
  std::vector<double> write_samples;

  for (int query_index = 0; query_index < num_queries; query_index++) {
    if (ShouldWriteAtQueryIndex(query_index, write_frequency_pct)) {
      if (write_mode == WriteMode::Rf) {
        write_samples.push_back(
            TimeAccelRfWriteCommitUs(runtime, write_seq, rf_chunk_orders));
      } else {
        auto probe = PickWriteProbe(probes, write_seq);
        write_samples.push_back(TimeAccelWriteCommitUs(runtime, probe));
      }
      write_seq++;
      result.writes_executed++;
    }

    const auto sample =
        TimeExecuteUs(accel_connection, PickArgVariant(arg_pool, arg_seq).aux_sql);
    result.total_accel_query_us += sample;
    hit_samples.push_back(sample);
  }

  for (const auto sample : write_samples) {
    result.total_accel_write_us += sample;
  }
  result.accel_hit_rate = 1.0;
  result.fallback_rate = 0.0;
  result.accel_hit_median_us = hit_samples.empty() ? 0.0
                                                   : SummarizeSamples(hit_samples).median_us;
  return result;
}

void PrintCsvHeader(std::ostream &out) {
  out << "suite,write_mode,write_policy,locality,write_frequency_pct,write_interval_sec,"
         "measured_duration_sec,reader_qps,query_num,"
         "num_queries,writes_executed,duckdb_ref_query_median_us,"
         "base_kernel_median_us,base_write_median_us,accel_write_median_us,"
         "accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,"
         "total_duckdb_query_us,total_accel_query_us,total_base_write_us,"
         "total_accel_write_us,duckdb_workload_us,accel_workload_us,"
         "workload_speedup,cold_start_workload_speedup,query_only_speedup,"
         "blended_query_speedup,cold_start_accel_workload_us,setup_full_build_ms,"
      << bench_diag::kAuxStoreDiagnosticsCsvHeader << "\n";
}

struct BenchSession {
  int query_num = 0;
  const catalog_ns::SharedQueryCatalogEntry &entry;
  duckdb::DuckDB database;
  duckdb::Connection base_connection;
  duckdb::Connection accel_connection;
  std::vector<catalog_ns::SharedProbe> write_probes;
  std::vector<TpchArgVariant> arg_pool;
  WriteMode write_mode = WriteMode::Point;
  int rf_chunk_orders = 1;
  double base_write_median_us = 0.0;
  double duckdb_ref_query_median_us = 0.0;
  double base_kernel_median_us = 0.0;
  double setup_full_build_ms = 0.0;
  uint64_t aux_state_bytes = 0;
  double reader_qps = 1.0;
  std::unique_ptr<DuckdbRuntime> runtime;

  BenchSession(int query_num_in, const std::string &db_path,
               const catalog_ns::SharedQueryCatalogEntry &entry_in)
      : query_num(query_num_in), entry(entry_in), database(db_path.c_str()),
        base_connection(database), accel_connection(database),
        write_mode(ParseWriteMode()), rf_chunk_orders(RfChunkOrderCount()) {}

  void Initialize(int warmup_queries, int ref_samples, int base_write_samples) {
    Require(suiteSupported(), "unsupported bench suite for multi-frequency sweep");
    Require(base_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
    Require(accel_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
    accelerator_codegen::RegisterAcceleratorFunctions(base_connection);
    accelerator_codegen::RegisterAcceleratorFunctions(accel_connection);

    arg_pool = BuildTpchArgPool(query_num, entry);
    Require(!arg_pool.empty(), "failed to build TPC-H arg pool");

    std::cerr << "tpch_q" << query_num << "_aux_bench: write_mode="
              << WriteModeName(write_mode);
    if (write_mode == WriteMode::Rf) {
      std::cerr << " rf_chunk_orders=" << rf_chunk_orders;
    }
    std::cerr << " vary_args=" << (VaryArgsEnabled() ? 1 : 0)
              << " arg_pool=" << arg_pool.size() << " arg_seed=" << ArgSeed()
              << '\n'
              << std::flush;

    if (write_mode == WriteMode::Point) {
      write_probes = PrecomputeWriteProbes(base_connection, entry, query_num);
      if (base_write_samples > 0) {
        for (int i = 0; i < base_write_samples; i++) {
          const auto &probe = PickWriteProbe(write_probes, 100000 + i);
          base_write_median_us += TimeBaseWriteCommitUs(base_connection, probe);
        }
        base_write_median_us /= static_cast<double>(base_write_samples);
      }
    } else if (base_write_samples > 0 && query_num != 11) {
      // Do not run RF2 deletes on the shared bench DB before Build — that would
      // permanently remove orders. Workload accounting uses accel write cost only.
      base_write_median_us = 0.0;
    }

    runtime = std::make_unique<DuckdbRuntime>(accel_connection, entry,
                                              ExpectedQueriesBeforeWrite(0));
    const auto build_start = Clock::now();
    runtime->Build();
    setup_full_build_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                  build_start)
                .count());
    aux_state_bytes = bench_diag::CollectAuxStateFootprintBytes(*runtime);

    for (int i = 0; i < warmup_queries; i++) {
      const auto &variant =
          arg_pool[static_cast<std::size_t>(i) % arg_pool.size()];
      if (ref_samples > 0) {
        ExecuteAndConsume(base_connection, variant.base_sql);
      }
      if (i < std::min(warmup_queries, 2)) {
        ExecuteAndConsume(base_connection, variant.kernel_sql);
      }
      // Warm the DuckDB aux TF path (bind/schema + hit) used by measured queries.
      ExecuteAndConsume(accel_connection, variant.aux_sql);
    }

    duckdb_ref_query_median_us =
        MeasureQueryMedianUs(base_connection, arg_pool, /*use_kernel_sql=*/false,
                             ref_samples);
    base_kernel_median_us = MeasureQueryMedianUs(
        base_connection, arg_pool, /*use_kernel_sql=*/true,
        ref_samples > 0 ? std::min(ref_samples, 2) : 0);
    std::cerr << "tpch_q" << query_num
              << "_aux_bench: duckdb_ref uses JSON profile latency; "
                 "accel queries use TF SQL path\n"
              << std::flush;
    reader_qps = MeasureReaderQps(accel_connection, arg_pool,
                                  std::max(1, std::min(warmup_queries, 3)));
  }

  void RunFrequencyPoint(int write_frequency_pct, const std::string &suite,
                         const std::string &locality, int measured_queries,
                         bool print_header) {
    RunMeasuredPoint(suite, locality, write_frequency_pct, 0.0, measured_queries,
                     0.0, print_header,
                     [&](BenchSession &session) {
                       session.runtime->SetExpectedQueriesBeforeNextWrite(
                           ExpectedQueriesBeforeWrite(write_frequency_pct));
                       session.runtime->ResetStoreDiagnosticsProfiling();
                       return RunMeasuredWorkload(
                           *session.runtime, session.accel_connection,
                           measured_queries, write_frequency_pct,
                           session.write_probes, session.write_mode,
                           session.rf_chunk_orders, session.arg_pool);
                     });
  }

  void RunIntervalPoint(double write_interval_sec, const std::string &suite,
                        const std::string &locality, double measured_duration_sec,
                        int min_writes, bool print_header) {
    const double duration_sec =
        RunDurationForInterval(measured_duration_sec, write_interval_sec, min_writes);
    std::cerr << "tpch_q" << query_num << "_aux_bench: starting"
              << " write_mode=" << WriteModeName(write_mode)
              << " write_interval_sec=" << write_interval_sec
              << " measured_duration_sec=" << duration_sec << '\n'
              << std::flush;
    RunMeasuredPoint(
        suite, locality, 0, write_interval_sec, 0, duration_sec, print_header,
        [&](BenchSession &session) {
          session.runtime->SetExpectedQueriesBeforeNextWrite(
              ExpectedQueriesBeforeWriteInterval(write_interval_sec,
                                                   session.reader_qps));
          session.runtime->ResetStoreDiagnosticsProfiling();
          return RunMeasuredWorkloadWallClock(
              *session.runtime, session.accel_connection, duration_sec,
              write_interval_sec, session.write_probes, session.write_mode,
              session.rf_chunk_orders, session.arg_pool);
        });
  }

private:
  template <typename RunFn>
  void RunMeasuredPoint(const std::string &suite, const std::string &locality,
                        int write_frequency_pct, double write_interval_sec,
                        int measured_queries, double measured_duration_sec,
                        bool print_header, RunFn run_fn) {
    Require(runtime != nullptr, "benchmark session is not initialized");
    constexpr const char *kWritePolicy = "hybrid";

    auto workload = run_fn(*this);
    const auto store_diag = bench_diag::CollectStoreDiagnostics(*runtime);
    const bool interval_mode = measured_queries <= 0;
    if (interval_mode) {
      write_frequency_pct =
          EffectiveWriteFrequencyPct(workload.writes_executed, workload.num_queries);
    }

    const int64_t query_count =
        measured_queries > 0 ? static_cast<int64_t>(measured_queries)
                             : workload.num_queries;
    const double total_duckdb_query_us =
        duckdb_ref_query_median_us * static_cast<double>(query_count);
    const double total_base_write_us =
        base_write_median_us * static_cast<double>(workload.writes_executed);
    const double duckdb_workload_us = total_duckdb_query_us + total_base_write_us;
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
      std::cout << std::flush;
    }

    const auto accel_write_median_us = workload.writes_executed > 0
                                           ? workload.total_accel_write_us /
                                                 workload.writes_executed
                                           : 0.0;

    std::cout << suite << ',' << WriteModeName(write_mode) << ',' << kWritePolicy
              << ',' << locality << ',' << write_frequency_pct << ','
              << write_interval_sec << ',' << measured_duration_sec << ','
              << reader_qps << ',' << query_num << ',' << workload.num_queries
              << ',' << workload.writes_executed << ',' << std::fixed
              << std::setprecision(2) << duckdb_ref_query_median_us << ','
              << base_kernel_median_us << ',' << base_write_median_us << ','
              << accel_write_median_us << ',' << workload.accel_hit_median_us
              << ',' << workload.fallback_median_us << ','
              << workload.accel_hit_rate << ',' << workload.fallback_rate << ','
              << total_duckdb_query_us << ',' << workload.total_accel_query_us
              << ',' << total_base_write_us << ',' << workload.total_accel_write_us
              << ',' << duckdb_workload_us << ',' << accel_workload_us << ','
              << workload_speedup << ',' << cold_start_workload_speedup << ','
              << query_only_speedup << ',' << blended_query_speedup << ','
              << cold_start_accel_workload_us << ',' << setup_full_build_ms << ',';
    bench_diag::AppendStoreDiagnosticsCsv(std::cout, aux_state_bytes, store_diag);
    std::cout << '\n' << std::flush;
    std::cerr << "tpch_q" << query_num << "_aux_bench: wrote row"
              << " write_interval_sec=" << write_interval_sec
              << " write_frequency_pct=" << write_frequency_pct
              << " num_queries=" << query_count
              << " writes=" << workload.writes_executed
              << " workload_speedup=" << workload_speedup << '\n'
              << std::flush;
  }

  bool suiteSupported() const { return true; }
};

} // namespace

std::vector<int> ParseWriteFrequencyList(const std::string &spec) {
  std::vector<int> result;
  std::istringstream stream(spec);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty()) {
      result.push_back(std::stoi(token));
    }
  }
  return result;
}

std::vector<double> ParseWriteIntervalList(const std::string &spec) {
  std::vector<double> result;
  std::istringstream stream(spec);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty()) {
      result.push_back(std::stod(token));
    }
  }
  return result;
}

double GetEnvDouble(const char *name, double fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stod(value) : fallback;
}

int GetEnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stoi(value) : fallback;
}

void RunMultiFrequencySweep(int query_num, const std::string &db_path,
                            const std::string &suite, const std::string &locality,
                            const std::vector<int> &write_frequencies,
                            int warmup_queries, int measured_queries,
                            int ref_samples, int base_write_samples) {
  Require(suite == "lineitem_only" || suite == "rf_refresh",
          "unsupported bench suite: " + suite +
              " (use lineitem_only; ACCEL_AUX_WRITE_MODE=point|rf selects writes)");
  BenchSession session(query_num, db_path,
                       catalog_ns::LookupSharedQuery("tpch", query_num));
  session.Initialize(warmup_queries, ref_samples, base_write_samples);
  bool print_header = true;
  for (const int write_frequency_pct : write_frequencies) {
    session.RunFrequencyPoint(write_frequency_pct, suite, locality,
                              measured_queries, print_header);
    print_header = false;
  }
}

void RunMultiWriteIntervalSweep(int query_num, const std::string &db_path,
                                const std::string &suite, const std::string &locality,
                                const std::vector<double> &write_intervals_sec,
                                int warmup_queries, double base_duration_sec,
                                int min_writes, int ref_samples,
                                int base_write_samples) {
  Require(suite == "lineitem_only" || suite == "rf_refresh",
          "unsupported bench suite: " + suite +
              " (use lineitem_only; ACCEL_AUX_WRITE_MODE=point|rf selects writes)");
  BenchSession session(query_num, db_path,
                       catalog_ns::LookupSharedQuery("tpch", query_num));
  session.Initialize(warmup_queries, ref_samples, base_write_samples);
  bool print_header = true;
  for (const double write_interval_sec : write_intervals_sec) {
    session.RunIntervalPoint(write_interval_sec, suite, locality, base_duration_sec,
                             min_writes, print_header);
    print_header = false;
  }
}

void RunConfiguredMultiSweep(int query_num, const std::string &db_path,
                             const std::string &suite, const std::string &locality,
                             int warmup_queries, int measured_queries,
                             int ref_samples, int base_write_samples) {
  // Ensure CSV rows reach tee/results.csv immediately when stdout is a pipe.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  if (const char *intervals = std::getenv("ACCEL_AUX_WRITE_INTERVALS")) {
    const auto interval_list = ParseWriteIntervalList(intervals);
    Require(!interval_list.empty(), "ACCEL_AUX_WRITE_INTERVALS is empty");
    const double duration_sec =
        GetEnvDouble("ACCEL_AUX_BENCH_DURATION_SEC", 120.0);
    const int min_writes = GetEnvInt("ACCEL_AUX_BENCH_MIN_WRITES", 30);
    RunMultiWriteIntervalSweep(query_num, db_path, suite, locality, interval_list,
                               warmup_queries, duration_sec, min_writes,
                               ref_samples, base_write_samples);
    return;
  }
  if (const char *frequencies = std::getenv("ACCEL_AUX_WRITE_FREQUENCIES")) {
    const auto frequency_list = ParseWriteFrequencyList(frequencies);
    Require(!frequency_list.empty(), "ACCEL_AUX_WRITE_FREQUENCIES is empty");
    RunMultiFrequencySweep(query_num, db_path, suite, locality, frequency_list,
                           warmup_queries, measured_queries, ref_samples,
                           base_write_samples);
    return;
  }
  throw std::runtime_error(
      "multi-point sweep requires ACCEL_AUX_WRITE_INTERVALS or "
      "ACCEL_AUX_WRITE_FREQUENCIES");
}

} // namespace accelerator_codegen::benchmarks::aux_sweep
