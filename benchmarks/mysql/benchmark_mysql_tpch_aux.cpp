// Warm MySQL TPC-H aux Execute vs native SQL.
// Usage: benchmark_mysql_tpch_aux <query_id> [write_pct] [bulk_rows]
// write_pct=0: read-only (default).
// write_pct=10: 1 write per 10 queries.
// bulk_rows=0 (default): differential point probe that changes this query's
//   native SQL and aux answer.
// bulk_rows>0: UPDATE that many qualifying rows (Q6-style lineitem price bump
//   unless the query has a dedicated bulk probe). Sets expected-queries for
//   the cost model to 100/write_pct.
//
// CSV (same schema as DuckDB/Postgres shared sweeps) is written to stdout and
// to ACCEL_SHARED_SWEEP_OUTPUT when set. Set ACCEL_SHARED_SWEEP_APPEND to
// append without repeating the header.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "aux_state/mysql_tpch_harness.hpp"
#include "aux_state/mysql_tpch_write_probes.hpp"

namespace {

using namespace accel::mysql::tpch_harness;
using Clock = std::chrono::steady_clock;

const char *kCsvHeader =
    "engine,workload,query,write_interval_sec,mode,reads,writes,rows,"
    "read_p50_us,read_sum_us,write_p50_us,write_sum_us,build_us,"
    "workload_sum_us,workload_speedup_vs_base,workload_with_build_us,"
    "workload_with_build_speedup_vs_base";

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

int GetEnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stoi(value) : fallback;
}

double GetEnvDouble(const char *name, double fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stod(value) : fallback;
}

double ElapsedUs(const Clock::time_point &start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

uint64_t CountResultRows(const std::string &text) {
  uint64_t n = 0;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty())
      ++n;
  }
  return n;
}

struct TimingSummary {
  double p50_us = 0.0;
  double sum_us = 0.0;
};

TimingSummary Summarize(std::vector<double> samples) {
  if (samples.empty())
    return {};
  std::sort(samples.begin(), samples.end());
  TimingSummary summary;
  summary.p50_us = samples[samples.size() / 2];
  for (const auto sample : samples)
    summary.sum_us += sample;
  return summary;
}

struct TimedQuery {
  bool ok = false;
  double us = 0.0;
  uint64_t rows = 0;
  std::string output;
};

TimedQuery TimeSqlOnce(const Tools &tools, const std::string &sql) {
  const auto start = Clock::now();
  auto q = QuerySQL(tools, sql);
  TimedQuery out;
  out.ok = q.first;
  out.output = q.second;
  out.us = ElapsedUs(start);
  out.rows = CountResultRows(q.second);
  return out;
}

double TimeWriteOnce(const Tools &tools, const std::string &sql) {
  const auto start = Clock::now();
  if (!RunSQL(tools, sql))
    throw std::runtime_error("write failed: " + sql);
  return ElapsedUs(start);
}

std::string LastAction(const Tools &tools) {
  auto r = QuerySQL(tools, "SELECT mysql_accel_get_last_maintenance_action()");
  return r.first ? TrimLines(r.second) : "error";
}

struct MixResult {
  int queries = 0;
  int writes = 0;
  uint64_t rows = 0;
  TimingSummary read;
  TimingSummary write;
  std::map<std::string, int> actions;
};

MixResult RunWriteMix(const Tools &tools, const std::string &query_sql,
                      const std::string &update_sql, int write_pct, int n_queries,
                      bool record_actions) {
  MixResult out;
  out.queries = n_queries;
  const int stride = std::max(1, 100 / std::max(1, write_pct));
  std::vector<double> read_samples;
  std::vector<double> write_samples;
  for (int i = 0; i < n_queries; ++i) {
    if (i % stride == 0) {
      write_samples.push_back(TimeWriteOnce(tools, update_sql));
      ++out.writes;
    }
    auto q = TimeSqlOnce(tools, query_sql);
    if (!q.ok) {
      throw std::runtime_error("write_mix query failed: " + query_sql + "\n" +
                               q.output);
    }
    read_samples.push_back(q.us);
    out.rows = q.rows;
    if (record_actions && (i % stride == 0))
      ++out.actions[LastAction(tools)];
  }
  out.read = Summarize(std::move(read_samples));
  out.write = Summarize(std::move(write_samples));
  return out;
}

void PrintModeCsv(int query_id, double interval, const char *mode, int reads,
                  int writes, uint64_t rows, const TimingSummary &read,
                  const TimingSummary &write, double build_us,
                  double workload_speedup, double workload_with_build_speedup) {
  const double workload_us = read.sum_us + write.sum_us;
  const double workload_with_build_us = build_us + workload_us;
  std::ostringstream line;
  line << "mysql,tpch," << query_id << ',' << interval << ',' << mode << ','
       << reads << ',' << writes << ',' << rows << ',' << std::fixed
       << std::setprecision(2) << read.p50_us << ',' << read.sum_us << ','
       << write.p50_us << ',' << write.sum_us << ',' << build_us << ','
       << workload_us << ',' << std::setprecision(4) << workload_speedup << ','
       << std::setprecision(2) << workload_with_build_us << ','
       << std::setprecision(4) << workload_with_build_speedup;
  Emit(line.str());
}

void OpenSweepOutput() {
  if (const char *output_path = std::getenv("ACCEL_SHARED_SWEEP_OUTPUT")) {
    const bool append = std::getenv("ACCEL_SHARED_SWEEP_APPEND") != nullptr;
    std::ios::openmode mode = std::ios::out;
    if (append)
      mode |= std::ios::app;
    SweepOutput() = std::make_unique<std::ofstream>(output_path, mode);
    if (!SweepOutput()->good())
      throw std::runtime_error(std::string("failed to open sweep output: ") +
                               output_path);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    int query_id = argc >= 2 ? std::stoi(argv[1]) : 1;
    int write_pct = argc >= 3 ? std::stoi(argv[2]) : 0;
    int bulk_rows = argc >= 4 ? std::stoi(argv[3]) : 0;
    auto tools = ConnectTpch();
    EnsureExtension(tools);
    const auto &entry = LookupMysqlQuery("tpch", query_id);
    const std::string sql = ConcreteBaseSql(query_id, entry);
    const std::string aux = ConcreteAccelSql(query_id, entry, true);
    const int reads = GetEnvInt("ACCEL_SHARED_SWEEP_READS", 8);
    const int warmup = GetEnvInt("ACCEL_SHARED_SWEEP_WARMUP", 1);
    const double interval =
        write_pct <= 0
            ? 0.0
            : GetEnvDouble("ACCEL_SHARED_SWEEP_INTERVAL", 2.0);

    OpenSweepOutput();
    const bool append_output = std::getenv("ACCEL_SHARED_SWEEP_APPEND") != nullptr;
    if (!append_output || !SweepOutput())
      Emit(kCsvHeader);

    if (write_pct <= 0) {
      const auto build = TimeSqlOnce(tools, aux);
      if (!build.ok)
        throw std::runtime_error("aux build failed:\n" + build.output);
      // One mysql CLI session per mode so process spawn does not dominate aux.
      const double aux_mean_us = TimeSqlBatchUs(tools, aux, warmup, reads);
      const double sql_mean_us = TimeSqlBatchUs(tools, sql, warmup, reads);
      const TimingSummary aux_read{aux_mean_us, aux_mean_us * reads};
      const TimingSummary sql_read{sql_mean_us, sql_mean_us * reads};
      const uint64_t rows = build.rows;
      const TimingSummary no_write;
      const double base_workload = sql_read.sum_us;
      const double shared_workload = aux_read.sum_us;
      const double speedup = base_workload / std::max(shared_workload, 1e-9);
      const double with_build_speedup =
          base_workload / std::max(build.us + shared_workload, 1e-9);
      PrintModeCsv(query_id, 0.0, "base", reads, 0, rows, sql_read, no_write, 0.0,
                   1.0, 1.0);
      PrintModeCsv(query_id, 0.0, "shared", reads, 0, rows, aux_read, no_write,
                   build.us, speedup, with_build_speedup);
      std::cerr << "Q" << query_id << " write_pct=0 aux_ms="
                << (aux_read.p50_us / 1000.0) << " sql_ms="
                << (sql_read.p50_us / 1000.0) << " speedup=" << speedup
                << " build_s=" << (build.us / 1e6) << "\n";
      if (speedup < 1.0) {
        std::cerr << "warning: aux did not beat native SQL on this dataset\n";
        return 2;
      }
      return 0;
    }

    EnsureMutationTriggers(tools);
    const auto build = TimeSqlOnce(tools, aux);
    if (!build.ok)
      throw std::runtime_error("aux build failed:\n" + build.output);
    if (const char *force = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
        force && *force) {
      QuerySQL(tools, "SELECT mysql_accel_set_force_maintenance_action(" +
                          SqlLiteral(force) + ")");
    } else {
      QuerySQL(tools, "SELECT mysql_accel_set_force_maintenance_action('')");
    }
    const double expected_qpw =
        std::max(1.0, 100.0 / std::max(1, write_pct));
    QuerySQL(tools, "SELECT mysql_accel_set_expected_queries(" +
                        std::to_string(expected_qpw) + ")");

    std::string update_sql;
    std::string restore_sql;
    int probe_cand = -1;
    int invalidated = 0;
    if (bulk_rows > 0) {
      update_sql =
          "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
          "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
          "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25 "
          "ORDER BY l_orderkey, l_linenumber LIMIT " +
          std::to_string(bulk_rows);
      invalidated = 1;
    } else {
      const WriteProbe probe =
          RequireInvalidatingAuxWrite(tools, query_id, sql, aux);
      update_sql = probe.update_sql;
      restore_sql = probe.restore_sql;
      probe_cand = probe.candidate;
      invalidated = 1;
      auto after_probe = TimeSqlOnce(tools, aux);
      if (!after_probe.ok)
        throw std::runtime_error("aux after probe failed:\n" + after_probe.output);
    }
    const int stride = std::max(1, 100 / std::max(1, write_pct));
    const int n_queries = std::max(reads, stride);
    auto aux_mix =
        RunWriteMix(tools, aux, update_sql, write_pct, n_queries, true);
    if (!restore_sql.empty() && !RunSQL(tools, restore_sql)) {
      throw std::runtime_error("restore after aux mix failed: " + restore_sql);
    }
    auto sql_mix =
        RunWriteMix(tools, sql, update_sql, write_pct, n_queries, false);
    if (!restore_sql.empty())
      RunSQL(tools, restore_sql);

    const double base_workload = sql_mix.read.sum_us + sql_mix.write.sum_us;
    const double shared_workload = aux_mix.read.sum_us + aux_mix.write.sum_us;
    const double speedup = base_workload / std::max(shared_workload, 1e-9);
    const double with_build_speedup =
        base_workload / std::max(build.us + shared_workload, 1e-9);
    PrintModeCsv(query_id, interval, "base", sql_mix.queries, sql_mix.writes,
                 sql_mix.rows, sql_mix.read, sql_mix.write, 0.0, 1.0, 1.0);
    PrintModeCsv(query_id, interval, "shared", aux_mix.queries, aux_mix.writes,
                 aux_mix.rows, aux_mix.read, aux_mix.write, build.us, speedup,
                 with_build_speedup);

    std::cerr << "Q" << query_id << " write_pct=" << write_pct
              << " bulk_rows=" << bulk_rows << " qpw=" << expected_qpw
              << " aux_ms=" << (shared_workload / 1000.0)
              << " sql_ms=" << (base_workload / 1000.0)
              << " writes=" << aux_mix.writes << " queries=" << aux_mix.queries
              << " workload_speedup=" << speedup
              << " invalidated=" << invalidated
              << " probe_cand=" << probe_cand << " actions=";
    bool first = true;
    for (const auto &kv : aux_mix.actions) {
      if (!first)
        std::cerr << ",";
      first = false;
      std::cerr << kv.first << ":" << kv.second;
    }
    if (aux_mix.actions.empty())
      std::cerr << "none";
    std::cerr << "\n";
    if (speedup < 1.0) {
      std::cerr << "warning: write-mix aux did not beat native SQL\n";
      return 2;
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "benchmark_mysql_tpch_aux failed: " << ex.what() << "\n";
    return 1;
  }
}
