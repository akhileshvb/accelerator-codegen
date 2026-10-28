// Batched-write microbenchmark for the SEC-EDGAR DuckDB accelerators.
//
//   bench_sec_edgar_batch_apply <query> <db_path> [rows_per_batch=500,5000,50000] [batches=3]
//
// Per batch size: builds the accelerator (timed), then `batches` times picks
// that many distinct rows the query depends on, applies them to the base
// table as ONE multi-row UPDATE in one transaction (timed: "base_update",
// "base_commit"), hands the accelerator the same rows as ONE MutationBatch
// (timed: "apply"), and runs one accelerator read (timed) checked against the
// base SQL bound to the same arguments, and every drawn parameter set
// (eval/harness/param_sets: ACCEL_PARAM_SEED, _SETS, _FILE) is checked likewise.  The rows are built with each query's own probe builder
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

#include "backends/duckdb/api/sql_fallback.hpp"
#include "eval/harness/differential.hpp"
#include "eval/harness/param_sets.hpp"
#include "sec_edgar/common/duckdb_probes.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace core = accelerator_codegen::aux_state::core;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;
using Arguments = core::QueryArguments;

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

struct Batch {
  std::string sql;
  core::MutationBatch mutation;
  size_t rows = 0;
};

// probe update_sql: "UPDATE <t> SET <c> = <c> + <delta> WHERE rowid = <r>"
Batch MakeBatch(duckdb::Connection &c, const catalog::SharedQueryCatalogEntry &entry, const Arguments &arguments,
                size_t n, std::unordered_set<int64_t> &used) {
  // The rows the answer depends on (the catalog's invalidating candidates), then filler rows of the same column.
  const auto pool = catalog::CollectBatchWritePool(c, entry, arguments, used.size() + n, 1,
                                                   catalog::sec_edgar_probes::BatchFillerSelectSql(entry.query_id));
  static const std::regex re(R"(UPDATE (\w+) SET (\w+) = \w+ \+ ([-0-9.eE+]+) WHERE rowid = (\d+))");
  Batch b;
  std::string table, column;
  std::ostringstream values;
  for (size_t i = 0; i < pool.probes.size() && b.rows < n; ++i) {
    auto probe = pool.probes[i];
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
          const long double s = std::max<long double>({1.0L, std::fabs(p), std::fabs(q)});
          if (std::fabs(p - q) > s * 1e-6L)
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
    const auto sets = harness::ArgumentSets(entry);
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
    Q(connection, catalog::BindArguments(entry.base_sql, sets.front()));
    std::cout << "q" << query << " base_read_us=" << Us(t) << "\n";

    DuckdbRuntime runtime(connection, entry, /*expected queries before write*/ 1.0);
    runtime.UseManualCapture(); // changes are hand-built below
    t = Clock::now();
    runtime.Build();
    std::cout << "q" << query << " build_us=" << Us(t) << "\n";
    if (std::getenv("BENCH_BUILD_ONLY"))
      return 0;
    for (int i = 0; i < 2; ++i) {
      t = Clock::now();
      Q(connection, catalog::BindArguments(entry.duckdb_aux_sql, sets.front()));
      std::cout << "q" << query << " aux_read_us=" << Us(t) << " ("
                << core::FormatAuxExecuteCounts(core::ReadAuxExecuteCounts()) << ")\n";
    }

    // Accelerator latency of each drawn parameter set, twice; the writes below are aimed at the first
    // set that has rows to write.
    const Arguments *target = nullptr;
    for (const auto &arguments : sets) {
      const auto call_sql = catalog::BindArguments(entry.duckdb_aux_sql, arguments);
      for (int i = 0; i < 2; ++i) {
        t = Clock::now();
        Q(connection, call_sql);
        std::cout << "q" << query << " draw_read_us=" << Us(t) << " " << call_sql << "\n";
      }
      try {
        (void)catalog::FindInvalidatingSharedProbe(connection, entry, arguments);
        target = target ? target : &arguments;
      } catch (const std::exception &) {
      }
    }
    Require(target != nullptr, "no drawn parameter set has an answer the catalog probes can change");
    std::unordered_set<int64_t> used;
    std::cout << "query,rows,batch,make_us,base_update_us,base_commit_us,apply_us,apply_us_per_row,read_us\n";
    for (const size_t n : sizes) {
      for (int k = 0; k < batches; ++k) {
        t = Clock::now();
        auto batch = MakeBatch(connection, entry, *target, n, used);
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
        Q(connection, catalog::BindArguments(entry.duckdb_aux_sql, *target));
        const double read_us = Us(t);
        std::cout << query << "," << batch.rows << "," << k << "," << make_us << "," << upd_us << ","
                  << commit_us << "," << apply_us << "," << apply_us / batch.rows << "," << read_us
                  << std::endl;
        if (check) {
          for (const auto &arguments : sets) {
            harness::RequireSharedMatchesBase(connection, runtime, entry, arguments);
            auto base = Q(connection, catalog::BindArguments(entry.base_sql, arguments));
            auto acc = Q(connection, catalog::BindArguments(entry.duckdb_aux_sql, arguments));
            Require(Same(*base, *acc), "param check mismatch for " + harness::ArgumentLiterals(arguments));
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
