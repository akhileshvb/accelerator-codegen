// Shared Postgres TPC-H aux/base quick bench vs native Postgres SQL.
// Builds: benchmark_postgres_tpch_aux
// Usage: benchmark_postgres_tpch_aux --query N [write_pct]  (a leading positional query is accepted)
//
// Uses one psql session +\timing so aux Runtime stays warm across iterations.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <regex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "eval/bench/driver/bench_cli.hpp"
#include "backends/postgres/tests/postgres_tooling.hpp"
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace {

using namespace accel::postgres;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;

std::string ShellEscape(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

double TimeSQLSessionMs(const Tools &tools, const std::string &load_sql,
                        const std::string &query_sql, int warmup, int iters,
                        bool write_mix) {
  const std::string path =
      std::string("/tmp/accel_pg_bench_") + std::to_string(::getpid()) + ".sql";
  {
    std::ofstream out(path);
    out << load_sql << ";\n";
    for (int i = 0; i < warmup; ++i) {
      out << query_sql << ";\n";
    }
    out << "\\timing on\n";
    for (int i = 0; i < iters; ++i) {
      if (write_mix) {
        out << "UPDATE lineitem SET l_discount = l_discount "
               "WHERE ctid = (SELECT ctid FROM lineitem LIMIT 1);\n";
      }
      out << query_sql << ";\n";
    }
  }

  std::string cmd = "PGPASSWORD=" + ShellEscape(tools.password) + " " +
                    ShellEscape(tools.psql.string()) + " -h " + ShellEscape(tools.host) +
                    " -p " + std::to_string(tools.port) + " -U " + ShellEscape(tools.user) +
                    " -d " + ShellEscape(tools.database) +
                    " -v ON_ERROR_STOP=1 -f " + ShellEscape(path) + " 2>&1";

  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp)
    return -1.0;
  std::string output;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp))
    output += buf;
  int rc = pclose(fp);
  std::regex timing_re(R"(Time:\s+([0-9]+(?:\.[0-9]+)?)\s+ms)");
  std::vector<double> times;
  for (std::sregex_iterator it(output.begin(), output.end(), timing_re), end;
       it != end; ++it) {
    times.push_back(std::stod((*it)[1].str()));
  }
  if (rc != 0 || static_cast<int>(times.size()) < iters) {
    std::cerr << "bench session failed rc=" << rc << " timings=" << times.size()
              << " need>=" << iters << "\n--- output tail ---\n"
              << (output.size() > 2000 ? output.substr(output.size() - 2000) : output)
              << "\n";
    return -1.0;
  }
  // With write_mix each iter emits UPDATE timing + SELECT timing.
  if (write_mix) {
    if (static_cast<int>(times.size()) < iters * 2)
      return -1.0;
    double sum = 0.0;
    // Take SELECT timings: every 2nd after the first of each pair (UPDATE, SELECT).
    for (int i = 0; i < iters; ++i) {
      sum += times[times.size() - iters * 2 + i * 2 + 1];
    }
    return sum / iters;
  }
  double sum = 0.0;
  for (int i = 0; i < iters; ++i)
    sum += times[times.size() - iters + i];
  return sum / iters;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const accelerator_codegen::benchmarks::driver::Args args(argc, argv, 3);
    const int query = args.query;
    const int write_pct = std::stoi(args.Positional(0, "0"));
    auto tools = DiscoverTools();
    if (!tools.ok) {
      std::cerr << "postgres unavailable: " << tools.error << '\n';
      return 1;
    }
    const char *db = std::getenv("TPCH_PG_DB");
    tools.database = (db && *db) ? db : "accel_tpch_test";

    const auto &entry = LookupPostgresQuery("tpch", query);
    const std::string ref = entry.base_sql ? entry.base_sql : "";
    const bool use_aux = std::getenv("ACCEL_WORKLOAD_KIND") &&
                         std::string(std::getenv("ACCEL_WORKLOAD_KIND")) == "aux_state";
    const char *accel_sql =
        use_aux && entry.aux_sql ? entry.aux_sql : entry.accelerator_sql;
    const std::string accel = accel_sql ? accel_sql : "";

    const std::string load =
        "LOAD '" +
        (tools.extension_so.empty()
             ? std::string("/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so")
             : tools.extension_so) +
        "'";

    const int iters = 5;
    const bool write_mix = write_pct > 0;
    double ref_ms = TimeSQLSessionMs(tools, load, ref, 2, iters, write_mix);
    double accel_ms = TimeSQLSessionMs(tools, load, accel, 2, iters, write_mix);
    if (ref_ms < 0 || accel_ms < 0) {
      std::cerr << "bench SQL failed\n";
      return 1;
    }
    double speedup = ref_ms / std::max(accel_ms, 1e-9);
    std::cout << "query," << query << ",write_pct," << write_pct
              << ",ref_ms," << ref_ms << ",accel_ms," << accel_ms
              << ",workload_speedup," << speedup << '\n';
    if (write_pct == 0) {
      std::cout << "read_only_query_speedup," << speedup << '\n';
      std::cout << "speedup_gate median_us=" << (accel_ms * 1000.0) << '\n';
      std::cout << "speedup_gate wall_time_base_us=" << (ref_ms * 1000.0) << '\n';
    } else {
      std::cout << "write_mix_workload_speedup," << speedup << '\n';
    }
    std::cerr << "digest: ref_ms=" << ref_ms << " accel_ms=" << accel_ms
              << " speedup=" << speedup << " write_pct=" << write_pct
              << " engine=postgres native_base_sql vs "
              << (use_aux ? "aux" : "base_accel") << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "benchmark_postgres_tpch_aux failed: " << e.what() << '\n';
    return 1;
  }
}
