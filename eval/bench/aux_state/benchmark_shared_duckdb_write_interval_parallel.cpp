#ifndef ACCEL_ENABLE_PARALLEL_AUX
#error "benchmark_shared_duckdb_write_interval_parallel requires ACCEL_ENABLE_PARALLEL_AUX"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "backends/duckdb/api/sql_fallback.hpp"
#include "eval/harness/differential.hpp"
#include "eval/harness/param_sets.hpp"
#include "catalog/query_catalog.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
using catalog::duckdb::DuckdbRuntime;
using Arguments = accelerator_codegen::aux_state::core::QueryArguments;
using Clock = std::chrono::steady_clock;

std::unique_ptr<std::ofstream> &SweepOutput() {
  static std::unique_ptr<std::ofstream> output;
  return output;
}

void Emit(const std::string &line) {
  std::cout << line << '\n';
  if (SweepOutput()) {
    *SweepOutput() << line << '\n';
    SweepOutput()->flush();
  }
}

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int GetEnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stoi(value) : fallback;
}

double GetEnvDouble(const char *name, double fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stod(value) : fallback;
}

int BenchThreads() {
  const int fallback = static_cast<int>(
      std::max(1u, std::min(8u, std::thread::hardware_concurrency())));
  return std::max(1, GetEnvInt("ACCEL_BENCH_THREADS", fallback));
}

uint64_t ExecuteSql(duckdb::Connection &connection, const char *sql) {
  // Full client path: parse + plan + execute + materialize, matching a
  // user-issued Query() for both base SQL and SELECT * FROM accel_*_aux*().
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB query failed");
  uint64_t rows = result->RowCount();
  for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      (void)result->GetValue(column, row);
    }
  }
  return rows;
}

uint64_t ExecuteBase(duckdb::Connection &connection, const catalog::SharedQueryCatalogEntry &entry,
                     const Arguments &arguments) {
  return ExecuteSql(connection, catalog::BindArguments(entry.base_sql, arguments).c_str());
}

uint64_t ExecuteAuxSql(duckdb::Connection &connection, const catalog::SharedQueryCatalogEntry &entry,
                       const Arguments &arguments) {
  Require(entry.duckdb_aux_sql != nullptr && *entry.duckdb_aux_sql,
          "missing duckdb_aux_sql");
  return ExecuteSql(connection, catalog::BindArguments(entry.duckdb_aux_sql, arguments).c_str());
}

// The first parameter set whose answer the query's catalog probes can change: the writes are aimed at it.
const Arguments &WriteTargetSet(duckdb::Connection &connection, const catalog::SharedQueryCatalogEntry &entry,
                                const std::vector<Arguments> &sets) {
  for (const auto &arguments : sets) {
    try {
      (void)catalog::FindInvalidatingSharedProbe(connection, entry, arguments);
      return arguments;
    } catch (const std::exception &) {
    }
  }
  throw std::runtime_error("no drawn parameter set has an answer the catalog probes can change");
}

struct TimingSummary {
  double p50_us = 0.0;
  double sum_us = 0.0;
};

struct ModeSummary {
  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t rows = 0;
  double build_us = 0.0;
  TimingSummary read;
  TimingSummary write;
};

double ElapsedUs(const Clock::time_point &start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

TimingSummary Summarize(std::vector<double> samples) {
  if (samples.empty()) {
    return {};
  }
  std::sort(samples.begin(), samples.end());
  TimingSummary summary;
  summary.p50_us = samples[samples.size() / 2];
  for (const auto sample : samples) {
    summary.sum_us += sample;
  }
  return summary;
}

ModeSummary RunMode(const std::string &db_path,
                    const catalog::SharedQueryCatalogEntry &entry,
                    const std::vector<Arguments> &sets, double interval, bool shared, int threads) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  auto pragma = connection.Query("PRAGMA threads=" + std::to_string(threads));
  Require(pragma != nullptr && !pragma->HasError(), "failed to set threads");
  // Same worker count for DuckDB SQL and in-kernel aux scans.
  setenv("ACCEL_AUX_THREADS", std::to_string(threads).c_str(), 1);

  std::unique_ptr<DuckdbRuntime> runtime;
  const double target_qps = GetEnvDouble("ACCEL_SHARED_SWEEP_READER_QPS", 2.0);
  const int read_operations = GetEnvInt("ACCEL_SHARED_SWEEP_READS", 8);
  double build_us = 0.0;
  if (shared) {
    const double expected_queries_before_write =
        interval > 0.0 ? target_qps * interval : 10.0;
    runtime = std::make_unique<DuckdbRuntime>(connection, entry,
                                              expected_queries_before_write);
    const auto build_start = Clock::now();
    runtime->Build();
    build_us = ElapsedUs(build_start);
    (void)ExecuteAuxSql(connection, entry, sets.front());
  }
  for (int i = 0; i < GetEnvInt("ACCEL_SHARED_SWEEP_WARMUP", 1); ++i) {
    if (shared) {
      (void)ExecuteAuxSql(connection, entry, sets.front());
    } else {
      (void)ExecuteBase(connection, entry, sets.front());
    }
  }
  catalog::SharedProbe probe;
  const bool do_writes = interval > 0.0;
  if (do_writes) {
    probe = catalog::FindInvalidatingSharedProbe(connection, entry, WriteTargetSet(connection, entry, sets));
    std::cerr << "invalidating_write_probes=1 query=" << entry.query_id << '\n';
  }
  double next_write = do_writes ? interval : 1e30;
  std::vector<double> read_samples;
  std::vector<double> write_samples;
  uint64_t reads = 0, writes = 0, rows = 0;
  for (int read_index = 0; read_index < read_operations; ++read_index) {
    const double virtual_time = static_cast<double>(read_index) / target_qps;
    while (do_writes && virtual_time + 1e-12 >= next_write) {
      const auto start = Clock::now();
      if (shared) {
        runtime->Apply(probe);
      } else {
        connection.BeginTransaction();
        auto result = connection.Query(probe.update_sql);
        Require(result != nullptr && !result->HasError(),
                result ? result->GetError() : "DuckDB base write failed");
        connection.Commit();
      }
      write_samples.push_back(ElapsedUs(start));
      ++writes;
      next_write += interval;
    }
    const auto start = Clock::now();
    const auto &arguments = sets[static_cast<size_t>(read_index) % sets.size()];
    rows = shared ? ExecuteAuxSql(connection, entry, arguments) : ExecuteBase(connection, entry, arguments);
    if (shared) {
      runtime->last_rows = rows;
    }
    read_samples.push_back(ElapsedUs(start));
    ++reads;
  }
  const auto read_stats = Summarize(std::move(read_samples));
  const auto write_stats = Summarize(std::move(write_samples));
  if (shared) {
    for (const auto &arguments : sets) {
      harness::RequireSharedMatchesBase(connection, *runtime, entry, arguments);
    }
  }
  if (do_writes) {
    auto restore = connection.Query(probe.restore_sql);
    Require(restore != nullptr && !restore->HasError(),
            restore ? restore->GetError() : "DuckDB probe restore failed");
  }
  return {reads, writes, rows, build_us, read_stats, write_stats};
}

void PrintModeSummary(const char *engine_name, const char *workload_name,
                      const catalog::SharedQueryCatalogEntry &entry,
                      double interval, const char *mode,
                      const ModeSummary &summary, double workload_speedup,
                      double workload_with_build_speedup) {
  const double workload_us = summary.read.sum_us + summary.write.sum_us;
  const double workload_with_build_us = summary.build_us + workload_us;
  std::ostringstream line;
  line << engine_name << ',' << workload_name << ',' << entry.query_id << ','
       << interval << ',' << mode << ',' << summary.reads << ','
       << summary.writes << ',' << summary.rows << ',' << std::fixed
       << std::setprecision(2) << summary.read.p50_us << ','
       << summary.read.sum_us << ',' << summary.write.p50_us << ','
       << summary.write.sum_us << ',' << summary.build_us << ',' << workload_us
       << ',' << workload_speedup << ',' << workload_with_build_us << ','
       << workload_with_build_speedup;
  Emit(line.str());
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 4,
            "usage: benchmark_shared_duckdb_write_interval_parallel "
            "sec_edgar <query> <db_path> <interval_sec>");
    Require(std::string(argv[1]) == "sec_edgar",
            "parallel aux bench currently supports only sec_edgar");
    const auto entry = catalog::LookupSharedQueryParallel(std::string(argv[1]),
                                                          std::stoi(argv[2]));
    const auto db_path = std::string(argv[3]);
    const double interval = std::stod(argv[4]);
    const int threads = BenchThreads();
    if (const char *output_path = std::getenv("ACCEL_SHARED_SWEEP_OUTPUT")) {
      const bool append = std::getenv("ACCEL_SHARED_SWEEP_APPEND") != nullptr;
      std::ios::openmode mode = std::ios::out;
      if (append) {
        mode |= std::ios::app;
      }
      SweepOutput() = std::make_unique<std::ofstream>(output_path, mode);
      Require(SweepOutput()->good(), "failed to open sweep output");
    }
    const auto sets = harness::ArgumentSets(entry);
    if (std::getenv("ACCEL_SHARED_SWEEP_RUN_DIFFERENTIAL")) {
      for (const auto &arguments : sets) {
        harness::RunDifferentialCheck(db_path, entry, arguments);
      }
      std::cerr << "duckdb parallel aux differential check passed for Q"
                << entry.query_id << '\n';
      if (std::getenv("ACCEL_SHARED_SWEEP_RUN_INCREMENTAL_DIFFERENTIAL")) {
        duckdb::DuckDB database(db_path.c_str());
        duckdb::Connection connection(database);
        harness::RunIncrementalDifferentialCheck(db_path, entry, WriteTargetSet(connection, entry, sets));
        std::cerr << "duckdb parallel aux incremental differential check passed for Q"
                  << entry.query_id << '\n';
      }
      if (std::getenv("ACCEL_SHARED_SWEEP_ONLY_DIFFERENTIAL")) {
        return 0;
      }
    }
    const bool append_output = std::getenv("ACCEL_SHARED_SWEEP_APPEND") != nullptr;
    if (!append_output || !SweepOutput()) {
      Emit("engine,workload,query,write_interval_sec,mode,reads,writes,rows,"
           "read_p50_us,read_sum_us,write_p50_us,write_sum_us,build_us,"
           "workload_sum_us,workload_speedup_vs_base,workload_with_build_us,"
           "workload_with_build_speedup_vs_base");
    }
    std::cerr << "ACCEL_BENCH_THREADS=" << threads << '\n';
    const auto base = RunMode(db_path, entry, sets, interval, false, threads);
    const auto shared = RunMode(db_path, entry, sets, interval, true, threads);
    const double base_workload_us = base.read.sum_us + base.write.sum_us;
    const double shared_workload_us = shared.read.sum_us + shared.write.sum_us;
    const double base_with_build_us = base.build_us + base_workload_us;
    const double shared_with_build_us = shared.build_us + shared_workload_us;
    PrintModeSummary("duckdb", "sec_edgar", entry, interval, "base", base, 1.0,
                     base_with_build_us > 0.0 ? 1.0 : 0.0);
    PrintModeSummary("duckdb", "sec_edgar", entry, interval, "aux_parallel", shared,
                     base_workload_us / shared_workload_us,
                     base_with_build_us / shared_with_build_us);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark_shared_duckdb_write_interval_parallel failed: "
              << error.what() << '\n';
    return 1;
  }
}
