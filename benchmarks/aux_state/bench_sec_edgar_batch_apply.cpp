// Batched-write microbenchmark for the SEC-EDGAR DuckDB accelerators.
//
//   bench_sec_edgar_batch_apply <query> <db_path> [rows_per_batch=500,5000,50000] [batches=3]
//
// Per batch size: builds the accelerator (timed), then `batches` times picks
// that many distinct rows the query depends on, applies them to the base
// table as ONE multi-row UPDATE in one transaction (timed: "base_update",
// "base_commit"), hands the accelerator the same rows as ONE MutationBatch
// (timed: "apply"), and runs one accelerator read (timed) checked against the
// base SQL (and the drawn parameter sets of ACCEL_SWEEP_PARAM_FILE's #check
// section, if given).  The rows are built with each query's own probe builder
// (catalog entry build_probe), so old/new rows are exactly what the harness
// probes deliver.  Single-threaded (PRAGMA threads=1, ACCEL_AUX_THREADS=1).
//
// Mutates the database: run it on a copy.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "accelerator_codegen/harness/differential.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace core = accelerator_codegen::aux_state::core;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

void Require(bool ok, const std::string &what) {
  if (!ok)
    throw std::runtime_error(what);
}

double Us(Clock::time_point a) {
  return std::chrono::duration<double, std::micro>(Clock::now() - a).count();
}

std::unique_ptr<duckdb::MaterializedQueryResult> Q(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), r ? r->GetError() + " in " + sql.substr(0, 300) : "null result");
  return r;
}

// Rows each query depends on, in the column layout of its probe_select.
std::string SampleSql(int query, size_t n, int seed) {
  const std::string num_cols = "n.rowid AS rid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, n.value";
  std::string from;
  switch (query) {
  case 1:
    return "SELECT * FROM (SELECT adsh, line, stmt, rfile, rowid AS rid FROM pre WHERE stmt IS NOT NULL "
           "AND line IS NOT NULL) USING SAMPLE " + std::to_string(n) + " ROWS (reservoir, " + std::to_string(seed) + ")";
  case 2:
    from = "FROM num n WHERE n.uom = 'pure' AND n.value IS NOT NULL";
    break;
  case 3:
    from = "FROM num n JOIN sub s ON n.adsh = s.adsh WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL";
    break;
  case 4:
    from = "FROM num n JOIN sub s ON n.adsh = s.adsh WHERE n.uom = 'USD' AND s.sic BETWEEN 4000 AND 4999 "
           "AND n.value IS NOT NULL AND EXISTS (SELECT 1 FROM pre p WHERE p.adsh = n.adsh AND p.tag = n.tag "
           "AND p.version = n.version AND p.stmt = 'EQ')";
    break;
  case 6:
    from = "FROM num n JOIN sub s ON n.adsh = s.adsh WHERE n.uom = 'USD' AND s.fy = 2023 AND n.value IS NOT NULL "
           "AND EXISTS (SELECT 1 FROM pre p WHERE p.adsh = n.adsh AND p.tag = n.tag AND p.version = n.version "
           "AND p.stmt = 'IS')";
    break;
  case 24:
    from = "FROM num n WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 AND n.value IS NOT NULL";
    break;
  default:
    throw std::runtime_error("unsupported query");
  }
  // USING SAMPLE applies before WHERE: sample the filtered rows.
  return "SELECT * FROM (SELECT " + num_cols + " " + from + ") USING SAMPLE " + std::to_string(n) + " ROWS (reservoir, " +
         std::to_string(seed) + ")";
}

struct Batch {
  std::string sql;
  core::MutationBatch mutation;
  size_t rows = 0;
};

// probe update_sql: "UPDATE <t> SET <c> = <c> + <delta> WHERE rowid = <r>"
Batch MakeBatch(duckdb::Connection &c, const catalog::SharedQueryCatalogEntry &entry, size_t n, int seed,
                std::unordered_set<int64_t> &used) {
  auto rows = Q(c, SampleSql(entry.query_id, n, seed));
  static const std::regex re(R"(UPDATE (\w+) SET (\w+) = \w+ \+ ([-0-9.eE+]+) WHERE rowid = (\d+))");
  Batch b;
  std::string table, column;
  std::ostringstream values;
  for (duckdb::idx_t i = 0; i < rows->RowCount(); ++i) {
    auto probe = entry.build_probe(*rows, i);
    std::smatch m;
    Require(std::regex_match(probe.update_sql, m, re), "unexpected probe sql " + probe.update_sql);
    const int64_t rid = std::stoll(m[4]);
    if (!used.insert(rid).second)
      continue;
    table = m[1];
    column = m[2];
    if (b.rows == 0) {
      b.mutation.table = probe.table;
      b.mutation.kind = probe.kind;
      b.mutation.updated_columns = probe.updated_columns;
    }
    values << (b.rows ? "," : "") << "(" << rid << "," << m[3] << ")";
    b.mutation.old_rows.push_back(std::move(probe.old_row));
    b.mutation.new_rows.push_back(std::move(probe.new_row));
    ++b.rows;
  }
  Require(b.rows > 0, "empty batch");
  b.sql = "UPDATE " + table + " SET " + column + " = " + table + "." + column + " + d.delta FROM (VALUES " +
          values.str() + ") d(rid, delta) WHERE " + table + ".rowid = d.rid";
  return b;
}

struct ParamDraw {
  std::string base_sql, call_sql;
};

// The #check draws (checks = true) or the read draws before them.
std::vector<ParamDraw> LoadChecks(const char *path, bool want_checks = true) {
  std::vector<ParamDraw> out;
  if (!path || !*path)
    return out;
  std::ifstream in(path);
  Require(in.good(), std::string("cannot open ") + path);
  bool checks = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("#check", 0) == 0) {
      checks = true;
      continue;
    }
    if (line.empty() || line[0] == '#')
      continue;
    if (checks != want_checks)
      continue;
    const auto tab = line.find('\t');
    out.push_back({line.substr(0, tab), line.substr(tab + 1)});
  }
  return out;
}

bool Same(duckdb::MaterializedQueryResult &a, duckdb::MaterializedQueryResult &b) {
  if (a.RowCount() != b.RowCount() || a.ColumnCount() != b.ColumnCount())
    return false;
  auto rows = [](duckdb::MaterializedQueryResult &r) {
    std::vector<std::vector<std::string>> out;
    for (duckdb::idx_t i = 0; i < r.RowCount(); ++i) {
      std::vector<std::string> row;
      for (duckdb::idx_t col = 0; col < r.ColumnCount(); ++col) {
        auto v = r.GetValue(col, i);
        row.push_back(v.IsNull() ? "NULL" : v.ToString());
      }
      out.push_back(std::move(row));
    }
    return out;
  };
  auto x = rows(a), y = rows(b);
  auto close = [&] {
    for (size_t i = 0; i < x.size(); ++i)
      for (size_t c = 0; c < x[i].size(); ++c) {
        if (x[i][c] == y[i][c])
          continue;
        try {
          const long double p = std::stold(x[i][c]), q = std::stold(y[i][c]);
          const long double s = std::max<long double>({1.0L, std::fabsl(p), std::fabsl(q)});
          if (std::fabsl(p - q) > s * 1e-6L)
            return false;
        } catch (...) {
          return false;
        }
      }
    return true;
  };
  if (close())
    return true;
  std::sort(x.begin(), x.end());
  std::sort(y.begin(), y.end());
  return close();
}

std::vector<size_t> ParseSizes(const char *arg) {
  std::vector<size_t> out;
  std::stringstream ss(arg);
  std::string tok;
  while (std::getline(ss, tok, ','))
    out.push_back(std::stoul(tok));
  return out;
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 3, "usage: bench_sec_edgar_batch_apply <query> <db> [sizes] [batches]");
    const int query = std::stoi(argv[1]);
    const std::string db_path = argv[2];
    const auto sizes = ParseSizes(argc > 3 ? argv[3] : "500,5000,50000");
    const int batches = argc > 4 ? std::stoi(argv[4]) : 3;
    const auto entry = catalog::LookupSharedQuery("sec_edgar", query);
    const auto checks = LoadChecks(std::getenv("ACCEL_SWEEP_PARAM_FILE"));
    const bool check = std::getenv("BENCH_NO_CHECK") == nullptr;

    duckdb::DuckDB database(db_path.c_str());
    duckdb::Connection connection(database);
    accelerator_codegen::RegisterAcceleratorFunctions(connection);
    Q(connection, "PRAGMA threads=1");
    if (const char *cfg = std::getenv("BENCH_DUCKDB_SETTINGS"))
      Q(connection, cfg);
    setenv("ACCEL_AUX_THREADS", "1", 1);

    // base read once (timed)
    auto t = Clock::now();
    Q(connection, entry.base_sql);
    std::cout << "q" << query << " base_read_us=" << Us(t) << "\n";

    DuckdbRuntime runtime(connection, entry, /*expected queries before write*/ 1.0);
    t = Clock::now();
    runtime.Build();
    std::cout << "q" << query << " build_us=" << Us(t) << "\n";
    if (std::getenv("BENCH_BUILD_ONLY"))
      return 0;
    for (int i = 0; i < 2; ++i) {
      t = Clock::now();
      Q(connection, entry.duckdb_aux_sql);
      std::cout << "q" << query << " aux_read_us=" << Us(t) << " ("
                << core::FormatAuxExecuteCounts(core::ReadAuxExecuteCounts()) << ")\n";
    }

    // Accelerator latency of each read draw (ACCEL_SWEEP_PARAM_FILE), twice.
    for (const auto &d : LoadChecks(std::getenv("ACCEL_SWEEP_PARAM_FILE"), false)) {
      for (int i = 0; i < 2; ++i) {
        t = Clock::now();
        Q(connection, d.call_sql);
        std::cout << "q" << query << " draw_read_us=" << Us(t) << " " << d.call_sql << "\n";
      }
    }
    std::unordered_set<int64_t> used;
    int seed = 1000 + query;
    std::cout << "query,rows,batch,make_us,base_update_us,base_commit_us,apply_us,apply_us_per_row,read_us\n";
    for (const size_t n : sizes) {
      for (int k = 0; k < batches; ++k) {
        t = Clock::now();
        auto batch = MakeBatch(connection, entry, n, seed++, used);
        const double make_us = Us(t);
        connection.BeginTransaction();
        t = Clock::now();
        auto res = Q(connection, batch.sql);
        const double upd_us = Us(t);
        Require(res->GetValue(0, 0).GetValue<int64_t>() == static_cast<int64_t>(batch.rows),
                "update touched " + res->GetValue(0, 0).ToString() + " rows, expected " +
                    std::to_string(batch.rows));
        t = Clock::now();
        connection.Commit();
        const double commit_us = Us(t);
        t = Clock::now();
        runtime.ApplyMutationBatch(batch.mutation);
        const double apply_us = Us(t);
        t = Clock::now();
        Q(connection, entry.duckdb_aux_sql);
        const double read_us = Us(t);
        std::cout << query << "," << batch.rows << "," << k << "," << make_us << "," << upd_us << ","
                  << commit_us << "," << apply_us << "," << apply_us / batch.rows << "," << read_us
                  << std::endl;
        if (check) {
          harness::RequireSharedMatchesBase(connection, runtime, entry);
          for (const auto &d : checks) {
            auto base = Q(connection, d.base_sql);
            auto acc = Q(connection, d.call_sql);
            Require(Same(*base, *acc), "param check mismatch for " + d.call_sql);
          }
        }
      }
    }
    std::cout << "q" << query << " OK" << (check ? " (checked)" : "") << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "bench_sec_edgar_batch_apply failed: " << e.what() << "\n";
    return 1;
  }
}
