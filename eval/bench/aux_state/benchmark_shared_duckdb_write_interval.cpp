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

#include "eval/harness/differential.hpp"
#include "catalog/query_catalog.hpp"
#include "sec_edgar/common/duckdb_probes.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace core = accelerator_codegen::aux_state::core;
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
// overload call. Produced by eval/bench/sec_edgar/sweep/sweep_params.py from
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
    const long double scale = std::max<long double>({1.0L, std::fabs(x), std::fabs(y)});
    return std::fabs(x - y) <= scale * 1e-6L;
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
  uint64_t writes = 0;      // write transactions (batches), one sample each
  uint64_t rows = 0;
  uint64_t rows_touched = 0; // distinct-row updates over all batches
  double build_us = 0.0;
  TimingSummary read;
  TimingSummary write;
};

// ---- batched writes ----
//
// Writes are counted in rows touched per second on the reader's virtual
// clock (READER_QPS reads/s): before read i, every row-write due by its
// virtual time i/qps -- floor(i * rows_per_sec / qps) in all -- that has not
// been applied yet is applied as ONE set-based UPDATE of that many distinct
// rows in ONE transaction, timed as one write sample.
struct WriteSchedule {
  int64_t rows_per_sec = 0;
  std::vector<int64_t> batch_before_read; // rows due before read i (0: none)
  int64_t total = 0;
  int64_t max_batch = 0;
  int64_t batches = 0;
};

WriteSchedule MakeWriteSchedule(int64_t rows_per_sec, int reads, double qps) {
  WriteSchedule schedule;
  schedule.rows_per_sec = rows_per_sec;
  int64_t applied = 0;
  for (int i = 0; i < reads; ++i) {
    const auto due = static_cast<int64_t>(
        std::floor(static_cast<double>(i) * static_cast<double>(rows_per_sec) / qps + 1e-9));
    const int64_t batch = rows_per_sec > 0 ? due - applied : 0;
    schedule.batch_before_read.push_back(batch);
    if (batch > 0) {
      applied = due;
      schedule.total += batch;
      schedule.max_batch = std::max(schedule.max_batch, batch);
      ++schedule.batches;
    }
  }
  return schedule;
}

// The batch pool's rows in temp tables of the mode's connection (setup, not
// timed): accel_batch_pool(k, rid, delta) -- pool slot k, the row's rowid
// and its fixed per-row delta -- and accel_batch_orig(rid, orig), the
// column's value before the mode, for the restore.
void StageBatchPool(duckdb::Connection &connection,
                    const catalog::BatchWritePool &pool) {
  auto exec = [&](const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result != nullptr && !result->HasError(),
            result ? result->GetError() : "DuckDB batch pool staging failed");
  };
  exec("DROP TABLE IF EXISTS accel_batch_pool");
  exec("DROP TABLE IF EXISTS accel_batch_orig");
  exec("CREATE TEMP TABLE accel_batch_pool (k BIGINT, rid BIGINT, delta DOUBLE)");
  {
    duckdb::Appender appender(connection, "temp", "main", "accel_batch_pool");
    for (std::size_t k = 0; k < pool.probes.size(); ++k) {
      appender.BeginRow();
      appender.Append<int64_t>(static_cast<int64_t>(k));
      appender.Append<int64_t>(pool.rowids[k]);
      appender.Append<double>(pool.probes[k].write_delta);
      appender.EndRow();
    }
    appender.Close();
  }
  exec("CREATE TEMP TABLE accel_batch_orig AS SELECT p.rid, t." + pool.column +
       " AS orig FROM accel_batch_pool p JOIN " + pool.table + " t ON t.rowid = p.rid");
}

// The pool slots [start, start + n) (mod pool size) as a predicate on p.k.
std::string SlotPredicate(std::size_t pool_size, std::size_t start, std::size_t n) {
  const auto range = [](std::size_t lo, std::size_t hi) {
    return "(p.k >= " + std::to_string(lo) + " AND p.k < " + std::to_string(hi) + ")";
  };
  if (start + n <= pool_size) {
    return range(start, start + n);
  }
  return "(" + range(start, pool_size) + " OR " + range(0, start + n - pool_size) + ")";
}

// One set-based UPDATE of the batch's rows: each row's column += its delta.
std::string BatchUpdateSql(const catalog::BatchWritePool &pool, std::size_t start,
                           std::size_t n) {
  return "UPDATE " + pool.table + " SET " + pool.column + " = " + pool.table + "." +
         pool.column + " + p.delta FROM accel_batch_pool p WHERE " + pool.table +
         ".rowid = p.rid AND " + SlotPredicate(pool.probes.size(), start, n);
}

std::string BatchRestoreSql(const catalog::BatchWritePool &pool) {
  return "UPDATE " + pool.table + " SET " + pool.column + " = o.orig FROM accel_batch_orig o WHERE " +
         pool.table + ".rowid = o.rid";
}

// Pool for `schedule`: as many distinct rows as the run touches (each row
// written at most once per mode) if the table has them, at least one batch.
// Reports how many rows are the query's invalidating candidates, checks a
// sample of them one by one, and requires the first batch as a whole to
// change the base result (rolled back).
catalog::BatchWritePool SelectBatchPool(const std::string &db_path,
                                        const catalog::SharedQueryCatalogEntry &entry,
                                        const WriteSchedule &schedule) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  auto pragma = connection.Query("PRAGMA threads=" + std::to_string(BenchThreads()));
  Require(pragma != nullptr && !pragma->HasError(), "failed to set threads");
  const char *filler =
      std::string(entry.workload_name) == "sec_edgar"
          ? catalog::sec_edgar_probes::BatchFillerSelectSql(entry.query_id)
          : nullptr;
  auto pool = catalog::CollectBatchWritePool(
      connection, entry, static_cast<std::size_t>(schedule.total),
      static_cast<std::size_t>(schedule.max_batch), filler);
  std::cerr << "batch_pool query=" << entry.query_id << " table=" << pool.table
            << " column=" << pool.column << " rows=" << pool.probes.size()
            << " invalidating_candidates=" << pool.invalidating
            << " filler=" << pool.probes.size() - pool.invalidating
            << " rows_per_sec=" << schedule.rows_per_sec
            << " batches=" << schedule.batches << " max_batch=" << schedule.max_batch
            << " total_rows=" << schedule.total << '\n';
  const auto before_fp = catalog::FingerprintSharedBaseSql(connection, entry);
  const int sample = GetEnvInt("ACCEL_BATCH_VERIFY_SAMPLE", 4);
  if (sample > 0 && pool.invalidating > 0) {
    int changed = 0, checked = 0;
    for (int s = 0; s < sample; ++s) {
      const std::size_t idx = pool.invalidating * static_cast<std::size_t>(s) /
                              static_cast<std::size_t>(sample);
      if (s > 0 && idx == pool.invalidating * static_cast<std::size_t>(s - 1) /
                               static_cast<std::size_t>(sample)) {
        continue;
      }
      ++checked;
      changed += catalog::SharedProbeChangesBaseResult(connection, entry,
                                                       pool.probes[idx], before_fp)
                     ? 1
                     : 0;
    }
    std::cerr << "batch_pool query=" << entry.query_id
              << " sampled_candidates_changing_result_alone=" << changed << "/"
              << checked << '\n';
  }
  StageBatchPool(connection, pool);
  connection.BeginTransaction();
  auto updated = connection.Query(
      BatchUpdateSql(pool, 0, static_cast<std::size_t>(schedule.max_batch)));
  Require(updated != nullptr && !updated->HasError(),
          updated ? updated->GetError() : "batch update failed");
  const auto touched = updated->GetValue(0, 0).GetValue<int64_t>();
  const auto after_fp = catalog::FingerprintSharedBaseSql(connection, entry);
  connection.Rollback();
  Require(touched == schedule.max_batch,
          "batch update touched " + std::to_string(touched) + " rows, expected " +
              std::to_string(schedule.max_batch));
  Require(after_fp != before_fp,
          "a full batch does not change the Q" + std::to_string(entry.query_id) +
              " base result");
  std::cerr << "batch_pool query=" << entry.query_id
            << " first_batch_changes_result=1 rows=" << touched << '\n';
  return pool;
}

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
                    const WriteSchedule &schedule,
                    const catalog::BatchWritePool &pool, bool shared,
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
  const bool do_writes = schedule.batches > 0;
  double build_us = 0.0;
  if (shared) {
    // Reads between consecutive write transactions (one batch before each
    // read); read-only: none expected.
    const double expected_queries_before_write =
        do_writes ? static_cast<double>(std::max(1, read_operations - 1)) /
                        static_cast<double>(schedule.batches)
                  : 10.0;
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
  (void)target_qps;
  // This mode's copy of the pool's probes: each one's old/new rows advance
  // as its row is written (the same rows and deltas in both modes).
  std::vector<catalog::SharedProbe> probes;
  if (do_writes) {
    StageBatchPool(connection, pool);
    probes = pool.probes;
  }
  std::size_t cursor = 0;
  std::vector<double> read_samples;
  std::vector<double> write_samples;
  uint64_t reads = 0, writes = 0, rows = 0, rows_touched = 0;
  for (int read_index = 0; read_index < read_operations; ++read_index) {
    const int64_t batch =
        static_cast<std::size_t>(read_index) < schedule.batch_before_read.size()
            ? schedule.batch_before_read[static_cast<std::size_t>(read_index)]
            : 0;
    if (batch > 0) {
      const auto n = static_cast<std::size_t>(batch);
      const auto sql = BatchUpdateSql(pool, cursor, n);
      // The accelerator's batch (the harness's stand-in for engine capture)
      // is assembled before the timer starts.
      core::MutationBatch mutation;
      if (shared) {
        std::vector<const catalog::SharedProbe *> members;
        members.reserve(n);
        for (std::size_t j = 0; j < n; ++j) {
          members.push_back(&probes[(cursor + j) % probes.size()]);
        }
        mutation = catalog::BuildSharedBatchMutation(entry, members);
      }
      const auto start = Clock::now();
      if (shared) {
        runtime->ApplyBatchWrite(sql, mutation);
      } else {
        connection.BeginTransaction();
        auto result = connection.Query(sql);
        Require(result != nullptr && !result->HasError(),
                result ? result->GetError() : "DuckDB base write failed");
        connection.Commit();
      }
      write_samples.push_back(ElapsedUs(start));
      for (std::size_t j = 0; j < n; ++j) {
        catalog::AdvanceProbe(probes[(cursor + j) % probes.size()]);
      }
      cursor = (cursor + n) % probes.size();
      ++writes;
      rows_touched += n;
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
    auto restore = connection.Query(BatchRestoreSql(pool));
    Require(restore != nullptr && !restore->HasError(),
            restore ? restore->GetError() : "DuckDB batch restore failed");
  }
  ModeSummary summary;
  summary.reads = reads;
  summary.writes = writes;
  summary.rows = rows;
  summary.rows_touched = rows_touched;
  summary.build_us = build_us;
  summary.read = read_stats;
  summary.write = write_stats;
  return summary;
}

void PrintModeSummary(const char *engine_name, const char *workload_name,
                      const catalog::SharedQueryCatalogEntry &entry,
                      double interval, const char *mode,
                      const ModeSummary &summary, double workload_speedup,
                      double workload_with_build_speedup,
                      const char *params_mode, const WriteSchedule &schedule,
                      std::size_t invalidating_rows) {
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
       << workload_with_build_speedup << ',' << params_mode << ','
       << schedule.rows_per_sec << ',' << summary.rows_touched << ','
       << summary.writes << ','
       << (summary.writes ? static_cast<double>(summary.rows_touched) /
                                static_cast<double>(summary.writes)
                          : 0.0)
       << ','
       << (summary.rows_touched ? summary.write.sum_us /
                                      static_cast<double>(summary.rows_touched)
                                : 0.0)
       << ',' << invalidating_rows;
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
           "workload_with_build_speedup_vs_base,params,rows_per_sec,"
           "rows_touched,batches,rows_per_batch,write_sum_per_row_us,"
           "invalidating_rows");
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
                             "(see eval/bench/sec_edgar/sweep/sweep_params.py)");
      draws = LoadParamDraws(file);
      Require(!draws.reads.empty(), "param file has no read draws");
    }
    const ParamDraws *draws_ptr = params_mode == "random" ? &draws : nullptr;
    // interval_sec = 1 / (rows touched per second); 0 = read-only.
    const int64_t rows_per_sec =
        interval > 0.0 ? static_cast<int64_t>(std::llround(1.0 / interval)) : 0;
    const auto schedule = MakeWriteSchedule(
        rows_per_sec, GetEnvInt("ACCEL_SHARED_SWEEP_READS", 8),
        GetEnvDouble("ACCEL_SHARED_SWEEP_READER_QPS", 2.0));
    catalog::BatchWritePool pool;
    if (schedule.batches > 0) {
      pool = SelectBatchPool(db_path, entry, schedule);
    }
    // Each row in the pool is invalidating or filler; this run's batches
    // walk the pool from slot 0, so these many of its row-writes hit
    // invalidating candidates.
    std::size_t invalidating_rows = 0;
    for (int64_t k = 0; k < schedule.total && !pool.probes.empty(); ++k) {
      invalidating_rows +=
          static_cast<std::size_t>(k) % pool.probes.size() < pool.invalidating ? 1 : 0;
    }
    const auto base = RunMode(db_path, entry, schedule, pool, false, draws_ptr);
    const auto shared = RunMode(db_path, entry, schedule, pool, true, draws_ptr);
    const double base_workload_us = base.read.sum_us + base.write.sum_us;
    const double shared_workload_us = shared.read.sum_us + shared.write.sum_us;
    const double base_with_build_us = base.build_us + base_workload_us;
    const double shared_with_build_us = shared.build_us + shared_workload_us;
    const char *workload_name =
        entry.workload_name != nullptr ? entry.workload_name : "tpch";
    PrintModeSummary("duckdb", workload_name, entry, interval, "base", base, 1.0,
                     base_with_build_us > 0.0 ? 1.0 : 0.0,
                     params_mode.c_str(), schedule, invalidating_rows);
    PrintModeSummary("duckdb", workload_name, entry, interval, "shared", shared,
                     base_workload_us / shared_workload_us,
                     base_with_build_us / shared_with_build_us,
                     params_mode.c_str(), schedule, invalidating_rows);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "benchmark_shared_duckdb_write_interval failed: "
              << error.what() << '\n';
    return 1;
  }
}
