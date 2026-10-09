#include <algorithm>
#include <chrono>
#include <cmath>
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

#include "accelerator_codegen/harness/differential.hpp"
#include "catalog/query_catalog.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
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

int BenchThreads() {
  const int fallback = 1;
  return std::max(1, GetEnvInt("ACCEL_BENCH_THREADS", fallback));
}

uint64_t ExecuteSql(duckdb::Connection &connection, const char *sql) {
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

uint64_t ExecuteBase(duckdb::Connection &connection,
                     const catalog::SharedQueryCatalogEntry &entry) {
  return ExecuteSql(connection, entry.base_sql);
}

uint64_t ExecuteAuxSql(duckdb::Connection &connection,
                       const catalog::SharedQueryCatalogEntry &entry) {
  Require(entry.duckdb_aux_sql != nullptr && *entry.duckdb_aux_sql,
          "missing duckdb_aux_sql");
  return ExecuteSql(connection, entry.duckdb_aux_sql);
}

// One drawn parameter set: the substituted base SQL and the accelerator
// overload call. Produced by benchmarks/sec_edgar/sweep/sweep_params.py from
// the per-query JSON specs (the same seeded sequence the Postgres driver uses).
struct ParamDraw {
  std::string base_sql;
  std::string call_sql;
};

struct ParamDraws {
  std::vector<ParamDraw> reads;
  std::vector<ParamDraw> checks;
};

// File format: one draw per line "base_sql<TAB>call_sql"; lines starting with
// "#check" mark the start of the correctness-check draws.
ParamDraws LoadParamDraws(const std::string &path) {
  std::ifstream in(path);
  Require(in.good(), "cannot open ACCEL_SWEEP_PARAM_FILE " + path);
  ParamDraws draws;
  bool in_checks = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("#check", 0) == 0) {
      in_checks = true;
      continue;
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto tab = line.find('\t');
    Require(tab != std::string::npos, "bad param draw line");
    ParamDraw draw{line.substr(0, tab), line.substr(tab + 1)};
    (in_checks ? draws.checks : draws.reads).push_back(std::move(draw));
  }
  return draws;
}

std::string CellText(const duckdb::Value &value) {
  return value.IsNull() ? std::string("NULL") : value.ToString();
}

bool CellsClose(const std::string &a, const std::string &b) {
  if (a == b) {
    return true;
  }
  try {
    size_t ia = 0, ib = 0;
    const long double x = std::stold(a, &ia), y = std::stold(b, &ib);
    if (ia != a.size() || ib != b.size()) {
      return false;
    }
    const long double scale = std::max<long double>({1.0L, std::fabsl(x), std::fabsl(y)});
    return std::fabsl(x - y) <= scale * 1e-6L;
  } catch (...) {
    return false;
  }
}

// Shared-vs-base check for one drawn parameter set: the overload call must
// return the same rows (in order, numbers to 1e-6 relative) as the substituted
// base SQL. Falls back to an order-insensitive comparison on ties.
void RequireDrawMatches(duckdb::Connection &connection, const ParamDraw &draw) {
  auto base = connection.Query(draw.base_sql);
  Require(base != nullptr && !base->HasError(),
          base ? base->GetError() : "param check: base query failed");
  auto shared = connection.Query(draw.call_sql);
  Require(shared != nullptr && !shared->HasError(),
          shared ? shared->GetError() : "param check: accelerator call failed");
  const std::string what = " for params draw: " + draw.call_sql;
  Require(base->RowCount() == shared->RowCount() &&
              base->ColumnCount() == shared->ColumnCount(),
          "param check shape mismatch (base rows=" +
              std::to_string(base->RowCount()) + " shared rows=" +
              std::to_string(shared->RowCount()) + ")" + what);
  auto rows_of = [](duckdb::MaterializedQueryResult &r) {
    std::vector<std::vector<std::string>> rows;
    for (duckdb::idx_t i = 0; i < r.RowCount(); ++i) {
      std::vector<std::string> row;
      for (duckdb::idx_t c = 0; c < r.ColumnCount(); ++c) {
        row.push_back(CellText(r.GetValue(c, i)));
      }
      rows.push_back(std::move(row));
    }
    return rows;
  };
  auto a = rows_of(*base), b = rows_of(*shared);
  auto equal = [&]() {
    for (size_t i = 0; i < a.size(); ++i) {
      for (size_t c = 0; c < a[i].size(); ++c) {
        if (!CellsClose(a[i][c], b[i][c])) {
          return false;
        }
      }
    }
    return true;
  };
  if (equal()) {
    return;
  }
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  Require(equal(), "param check value mismatch" + what);
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
                    double interval, bool shared,
                    const ParamDraws *draws = nullptr) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  const int threads = BenchThreads();
  auto pragma = connection.Query("PRAGMA threads=" + std::to_string(threads));
  Require(pragma != nullptr && !pragma->HasError(), "failed to set threads");
  setenv("ACCEL_AUX_THREADS", std::to_string(threads).c_str(), 1);

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
    (void)ExecuteAuxSql(connection, entry);
  }
  for (int i = 0; i < GetEnvInt("ACCEL_SHARED_SWEEP_WARMUP", 1); ++i) {
    if (shared) {
      (void)ExecuteAuxSql(connection, entry);
    } else {
      (void)ExecuteBase(connection, entry);
    }
  }
  catalog::SharedProbe probe;
  const bool do_writes = interval > 0.0;
  if (do_writes) {
    probe = catalog::FindInvalidatingSharedProbe(connection, entry);
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
    if (draws != nullptr) {
      const auto &draw = draws->reads[static_cast<size_t>(read_index) %
                                      draws->reads.size()];
      rows = ExecuteSql(connection,
                        (shared ? draw.call_sql : draw.base_sql).c_str());
    } else {
      rows = shared ? ExecuteAuxSql(connection, entry)
                    : ExecuteBase(connection, entry);
    }
    if (shared) {
      runtime->last_rows = rows;
    }
    read_samples.push_back(ElapsedUs(start));
    ++reads;
  }
  const auto read_stats = Summarize(std::move(read_samples));
  const auto write_stats = Summarize(std::move(write_samples));
  if (shared) {
    harness::RequireSharedMatchesBase(connection, *runtime, entry);
    if (draws != nullptr) {
      for (const auto &draw : draws->checks) {
        RequireDrawMatches(connection, draw);
      }
      std::cerr << "param correctness: " << draws->checks.size()
                << " drawn sets match base" << '\n';
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
                      double workload_with_build_speedup,
                      const char *params_mode) {
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
       << workload_with_build_speedup << ',' << params_mode;
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
           "workload_with_build_speedup_vs_base,params");
    }
    std::cerr << "ACCEL_BENCH_THREADS=" << BenchThreads() << '\n';
    // ACCEL_SWEEP_PARAMS=default|random. Random mode reads the pre-drawn
    // parameter sets (ACCEL_SWEEP_PARAM_FILE, made by sweep_params.py from
    // ACCEL_SWEEP_PARAM_SEED) so base and shared see the same sequence.
    const char *params_env = std::getenv("ACCEL_SWEEP_PARAMS");
    const std::string params_mode =
        params_env && *params_env ? params_env : "default";
    Require(params_mode == "default" || params_mode == "random",
            "ACCEL_SWEEP_PARAMS must be default or random");
    ParamDraws draws;
    if (params_mode == "random") {
      const char *file = std::getenv("ACCEL_SWEEP_PARAM_FILE");
      Require(file && *file, "random mode needs ACCEL_SWEEP_PARAM_FILE "
                             "(see benchmarks/sec_edgar/sweep/sweep_params.py)");
      draws = LoadParamDraws(file);
      Require(!draws.reads.empty(), "param file has no read draws");
    }
    const ParamDraws *draws_ptr = params_mode == "random" ? &draws : nullptr;
    const auto base = RunMode(db_path, entry, interval, false, draws_ptr);
    const auto shared = RunMode(db_path, entry, interval, true, draws_ptr);
    const double base_workload_us = base.read.sum_us + base.write.sum_us;
    const double shared_workload_us = shared.read.sum_us + shared.write.sum_us;
    const double base_with_build_us = base.build_us + base_workload_us;
    const double shared_with_build_us = shared.build_us + shared_workload_us;
    const char *workload_name =
        entry.workload_name != nullptr ? entry.workload_name : "tpch";
    PrintModeSummary("duckdb", workload_name, entry, interval, "base", base, 1.0,
                     base_with_build_us > 0.0 ? 1.0 : 0.0,
                     params_mode.c_str());
    PrintModeSummary("duckdb", workload_name, entry, interval, "shared", shared,
                     base_workload_us / shared_workload_us,
                     base_with_build_us / shared_with_build_us,
                     params_mode.c_str());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark_shared_duckdb_write_interval failed: "
              << error.what() << '\n';
    return 1;
  }
}
