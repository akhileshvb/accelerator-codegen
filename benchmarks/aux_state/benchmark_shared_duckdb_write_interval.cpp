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
#include <utility>
#include <vector>

#include "accelerator_codegen/aux_state/harness/differential.hpp"
#include "accelerator_codegen/aux_state/engine/shared/query_catalog.hpp"
#include "accelerator_codegen/aux_state/engine/duckdb/duckdb_runtime.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
using catalog::duckdb::DuckdbRuntime;
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

uint64_t ExecuteBase(duckdb::Connection &connection,
                     const catalog::SharedQueryCatalogEntry &entry) {
  auto result = connection.Query(entry.base_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB base query failed");
  uint64_t rows = result->RowCount();
  for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      (void)result->GetValue(column, row);
    }
  }
  return rows;
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
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
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
                    double interval, bool shared) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  auto pragma = connection.Query("PRAGMA threads=1");
  Require(pragma != nullptr && !pragma->HasError(), "failed to set threads");

  std::unique_ptr<DuckdbRuntime> runtime;
  const double target_qps =
      GetEnvDouble("ACCEL_SHARED_SWEEP_READER_QPS", 2.0);
  const int read_operations =
      GetEnvInt("ACCEL_SHARED_SWEEP_READS", 8);
  double build_us = 0.0;
  if (shared) {
    const double expected_queries_before_write =
        interval > 0.0 ? target_qps * interval : 10.0;
    runtime = std::make_unique<DuckdbRuntime>(
        connection, entry, expected_queries_before_write);
    const auto build_start = Clock::now();
    runtime->Build();
    build_us = ElapsedUs(build_start);
    runtime->Execute();
  }
  for (int i = 0; i < GetEnvInt("ACCEL_SHARED_SWEEP_WARMUP", 1); ++i) {
    if (shared) {
      runtime->Execute();
    } else {
      (void)ExecuteBase(connection, entry);
    }
  }
  auto probe = catalog::FindSharedProbe(connection, entry);
  double next_write = interval > 0.0 ? interval : 1e30;
  std::vector<double> read_samples;
  std::vector<double> write_samples;
  uint64_t reads = 0, writes = 0, rows = 0;
  for (int read_index = 0; read_index < read_operations; ++read_index) {
    const double virtual_time = static_cast<double>(read_index) / target_qps;
    while (interval > 0.0 && virtual_time + 1e-12 >= next_write) {
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
    rows = shared ? (runtime->Execute(), runtime->last_rows)
                  : ExecuteBase(connection, entry);
    read_samples.push_back(ElapsedUs(start));
    ++reads;
  }
  const auto read_stats = Summarize(std::move(read_samples));
  const auto write_stats = Summarize(std::move(write_samples));
  if (shared) {
    harness::RequireSharedMatchesBase(connection, *runtime, entry);
  }
  auto restore = connection.Query(probe.restore_sql);
  Require(restore != nullptr && !restore->HasError(),
          restore ? restore->GetError() : "DuckDB probe restore failed");
  return {reads, writes, rows, build_us, read_stats, write_stats};
}

void PrintModeSummary(const char *engine_name, const char *workload_name,
                      const catalog::SharedQueryCatalogEntry &entry,
                      double interval, const char *mode,
                      const ModeSummary &summary, double workload_speedup,
                      double workload_with_build_speedup) {
  const double workload_us = summary.read.sum_us + summary.write.sum_us;
  const double workload_with_build_us =
      summary.build_us + workload_us;
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
            "usage: benchmark_shared_duckdb_write_interval "
            "<tpch|sec_edgar> <query> <db_path> <interval_sec>");
    const auto entry =
        catalog::LookupSharedQuery(std::string(argv[1]), std::stoi(argv[2]));
    const auto db_path = std::string(argv[3]);
    const double interval = std::stod(argv[4]);
    if (const char *output_path = std::getenv("ACCEL_SHARED_SWEEP_OUTPUT")) {
      const bool append = std::getenv("ACCEL_SHARED_SWEEP_APPEND") != nullptr;
      std::ios::openmode mode = std::ios::out;
      if (append) {
        mode |= std::ios::app;
      }
      SweepOutput() =
          std::make_unique<std::ofstream>(output_path, mode);
      Require(SweepOutput()->good(), "failed to open sweep output");
    }
    if (std::getenv("ACCEL_SHARED_SWEEP_RUN_DIFFERENTIAL")) {
      harness::RunDifferentialCheck(db_path, entry);
      std::cerr << "duckdb differential check passed for Q" << entry.query_id
                << '\n';
      if (std::getenv("ACCEL_SHARED_SWEEP_RUN_INCREMENTAL_DIFFERENTIAL")) {
        harness::RunIncrementalDifferentialCheck(db_path, entry);
        std::cerr << "duckdb incremental differential check passed for Q"
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
    const auto base = RunMode(db_path, entry, interval, false);
    const auto shared = RunMode(db_path, entry, interval, true);
    const double base_workload_us = base.read.sum_us + base.write.sum_us;
    const double shared_workload_us = shared.read.sum_us + shared.write.sum_us;
    const double base_with_build_us = base.build_us + base_workload_us;
    const double shared_with_build_us = shared.build_us + shared_workload_us;
    const char *workload_name =
        entry.workload_name != nullptr ? entry.workload_name : "tpch";
    PrintModeSummary("duckdb", workload_name, entry, interval, "base", base, 1.0,
                     base_with_build_us > 0.0 ? 1.0 : 0.0);
    PrintModeSummary("duckdb", workload_name, entry, interval, "shared", shared,
                     base_workload_us / shared_workload_us,
                     base_with_build_us / shared_with_build_us);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark_shared_duckdb_write_interval failed: "
              << error.what() << '\n';
    return 1;
  }
}
