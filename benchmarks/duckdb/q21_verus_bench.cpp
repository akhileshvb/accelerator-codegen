// TPC-H Q21 on DuckDB: base SQL vs the Verus-verified accelerator
// (accel_tpch_q21_verus), with the C++ accelerator (accel_tpch_q21_aux) as a
// reference point.  Mirrors benchmarks/postgres/q21_verus_bench.py.
//
//   q21_verus_bench <db-path> [rounds=20] [acc_reps=30] [base_reps=3]
//
// 1. differential check: every nation, accelerator vs base SQL (ordered rows)
// 2. read-only: per nation, base median (threads=1 and default threads),
//    accelerator medians (table-function queries, result fetched)
// 3. writes: k write transactions then one query on a random nation, for
//    point / rf / mixed write mixes and k = 1, 10.  Every system replays the
//    same seeded sequence from the same state (restored after each run).
//    Accelerated writes go through DuckdbRuntime::ApplyBatch with the exact
//    captured row deltas (read from the database before each write).
//
// The database file is modified (writes are undone after each run); pass a
// copy.  Latencies are client-observed and include fetching the result.
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "catalog/query_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

const char *kBaseSql =
    "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation "
    "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
    "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
    "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
    "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE "
    "l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
    "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
    "n_name = '%s' GROUP BY s_name ORDER BY numwait DESC, s_name";
constexpr int64_t kNewKeyBase = 900000000;

void Require(bool ok, const std::string &msg) {
  if (!ok)
    throw std::runtime_error(msg);
}

std::unique_ptr<duckdb::MaterializedQueryResult> Q(duckdb::Connection &c,
                                                   const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), (r ? r->GetError() : "query failed") + ": " + sql);
  return r;
}

std::string BaseSql(const std::string &nation) {
  char buf[2048];
  std::snprintf(buf, sizeof(buf), kBaseSql, nation.c_str());
  return buf;
}

std::string AccelSql(const std::string &fn, const std::string &nation) {
  return "SELECT * FROM " + fn + "('" + nation + "')";
}

using Rows = std::vector<std::pair<std::string, int64_t>>;
Rows Fetch(duckdb::Connection &c, const std::string &sql) {
  auto r = Q(c, sql);
  Rows rows;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
    rows.emplace_back(r->GetValue(0, i).ToString(), r->GetValue(1, i).GetValue<int64_t>());
  return rows;
}

double TimeMs(duckdb::Connection &c, const std::string &sql) {
  const auto t = Clock::now();
  Fetch(c, sql);
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : (v.size() % 2 ? v[v.size() / 2]
                                        : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]));
}

int32_t Days(const duckdb::Value &v) {
  return duckdb::Date::EpochDays(v.GetValue<duckdb::date_t>());
}

core::AuxValue I(int64_t x) { return core::AuxValue::Of(core::AuxType::Int64, x); }
core::AuxValue D(int32_t d) { return core::AuxValue::Of(core::AuxType::Date, core::AuxDate{d}); }
core::AuxValue S(const std::string &s) { return core::AuxValue::Of(core::AuxType::String, s); }

// ── write workload ──────────────────────────────────────────────────────────
struct Op {
  enum Kind { Point, Rf1, Rf2 } kind;
  int64_t ok = 0;
  int delta = 0;                            // point: new receipt = commit + delta
  std::vector<std::pair<int64_t, int>> lines; // rf1: (suppkey, receipt - commit)
};

struct WriteGen {
  std::mt19937_64 rng;
  const std::vector<int64_t> &f_orders;
  int64_t n_supp;
  int64_t next_key = kNewKeyBase;
  std::vector<int64_t> inserted;
  WriteGen(uint64_t seed, const std::vector<int64_t> &f, int64_t ns)
      : rng(seed), f_orders(f), n_supp(ns) {}
  Op Next(const std::string &mix) {
    std::string kind = mix;
    if (mix == "mixed")
      kind = (rng() % 3 < 2) ? "point" : "rf";
    Op op;
    if (kind == "point") {
      op.kind = Op::Point;
      op.ok = f_orders[rng() % f_orders.size()];
      op.delta = (rng() % 2) ? 5 : -5;
      return op;
    }
    if (!inserted.empty() && rng() % 2) {
      const auto i = rng() % inserted.size();
      op.kind = Op::Rf2;
      op.ok = inserted[i];
      inserted.erase(inserted.begin() + i);
      return op;
    }
    op.kind = Op::Rf1;
    op.ok = next_key++;
    inserted.push_back(op.ok);
    const int n = 1 + rng() % 7;
    for (int i = 0; i < n; ++i)
      op.lines.push_back({1 + static_cast<int64_t>(rng() % n_supp), (rng() % 2) ? 3 : -3});
    return op;
  }
};

const int32_t kCommit = duckdb::Date::EpochDays(duckdb::Date::FromDate(1995, 1, 20));

// SQL statements and captured row deltas for one operation, read from the
// current database state (exact deltas).
std::vector<catalog::SharedProbe> Probes(duckdb::Connection &c, const Op &op) {
  std::vector<catalog::SharedProbe> probes;
  if (op.kind == Op::Point) {
    auto r = Q(c, "SELECT l_suppkey, l_commitdate, l_receiptdate FROM lineitem WHERE l_orderkey = " +
                      std::to_string(op.ok) + " AND l_linenumber = 1");
    Require(r->RowCount() == 1, "point probe row ok=" + std::to_string(op.ok) + " rows=" + std::to_string(r->RowCount()));
    const int64_t sk = r->GetValue(0, 0).GetValue<int64_t>();
    const int32_t c0 = Days(r->GetValue(1, 0)), r0 = Days(r->GetValue(2, 0));
    catalog::SharedProbe p;
    p.table = 3;
    p.kind = core::MutationKind::Update;
    p.update_sql = "UPDATE lineitem SET l_receiptdate = l_commitdate + INTERVAL (" +
                   std::to_string(op.delta) + ") DAY WHERE l_orderkey = " +
                   std::to_string(op.ok) + " AND l_linenumber = 1";
    p.old_row.values = {I(op.ok), I(sk), D(c0), D(r0)};
    p.new_row.values = {I(op.ok), I(sk), D(c0), D(c0 + op.delta)};
    p.updated_columns = {3};
    probes.push_back(p);
  } else if (op.kind == Op::Rf1) {
    catalog::SharedProbe o;
    o.table = 2;
    o.kind = core::MutationKind::Insert;
    o.update_sql = "INSERT INTO orders SELECT * REPLACE (" + std::to_string(op.ok) +
                   " AS o_orderkey, 'F' AS o_orderstatus, 'bench' AS o_comment) "
                   "FROM orders LIMIT 1";
    o.new_row.values = {I(op.ok), S("F")};
    probes.push_back(o);
    int ln = 1;
    for (const auto &[sk, d] : op.lines) {
      catalog::SharedProbe l;
      l.table = 3;
      l.kind = core::MutationKind::Insert;
      l.update_sql = "INSERT INTO lineitem SELECT * REPLACE (" + std::to_string(op.ok) +
                     " AS l_orderkey, " + std::to_string(sk) + " AS l_suppkey, " +
                     std::to_string(ln) + " AS l_linenumber, DATE '1995-01-20' AS "
                     "l_commitdate, DATE '1995-01-20' + INTERVAL (" + std::to_string(d) +
                     ") DAY AS l_receiptdate, 'bench' AS l_comment) FROM lineitem LIMIT 1";
      l.new_row.values = {I(op.ok), I(sk), D(kCommit), D(kCommit + d)};
      probes.push_back(l);
      ++ln;
    }
  } else {
    auto r = Q(c, "SELECT l_linenumber, l_suppkey, l_commitdate, l_receiptdate FROM lineitem "
                  "WHERE l_orderkey = " + std::to_string(op.ok));
    for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
      catalog::SharedProbe l;
      l.table = 3;
      l.kind = core::MutationKind::Delete;
      l.update_sql = "DELETE FROM lineitem WHERE l_orderkey = " + std::to_string(op.ok) +
                     " AND l_linenumber = " + r->GetValue(0, i).ToString();
      l.old_row.values = {I(op.ok), I(r->GetValue(1, i).GetValue<int64_t>()),
                          D(Days(r->GetValue(2, i))), D(Days(r->GetValue(3, i)))};
      probes.push_back(l);
    }
    catalog::SharedProbe o;
    o.table = 2;
    o.kind = core::MutationKind::Delete;
    o.update_sql = "DELETE FROM orders WHERE o_orderkey = " + std::to_string(op.ok);
    o.old_row.values = {I(op.ok), S("F")};
    probes.push_back(o);
  }
  return probes;
}

struct RunResult {
  double total_ms = 0, query_mean = 0, write_mean = 0;
  int writes = 0, queries = 0;
  bool matches = true;
};

const catalog::SharedQueryCatalogEntry &EntryFor(const std::string &system) {
  return system == "verus" ? catalog::TpchQ21VerusCatalogEntry()
                           : catalog::LookupSharedQuery(catalog::SharedWorkloadKind::Tpch, 21);
}
std::string FnFor(const std::string &system) {
  return system == "verus" ? "accel_tpch_q21_verus" : "accel_tpch_q21_aux";
}

void Restore(duckdb::DuckDB &db, const std::vector<std::pair<int64_t, int32_t>> &points) {
  duckdb::Connection c(db);
  Q(c, "DELETE FROM lineitem WHERE l_orderkey >= " + std::to_string(kNewKeyBase));
  Q(c, "DELETE FROM orders WHERE o_orderkey >= " + std::to_string(kNewKeyBase));
  Q(c, "BEGIN");
  for (const auto &[ok, r] : points)
    Q(c, "UPDATE lineitem SET l_receiptdate = DATE '1970-01-01' + INTERVAL (" +
             std::to_string(r) + ") DAY WHERE l_orderkey = " + std::to_string(ok) +
             " AND l_linenumber = 1");
  Q(c, "COMMIT");
}

RunResult WriteRun(duckdb::DuckDB &db, const std::string &system, const std::string &mix, int k,
                   int rounds, const std::vector<std::string> &nations,
                   const std::vector<int64_t> &f_orders, int64_t n_supp) {
  WriteGen gen(100 + k, f_orders, n_supp);
  std::mt19937_64 qrng(7 + k);
  // c owns the runtime and performs writes; qc issues the timed queries (a
  // table-function query must not run on the runtime's own connection, whose
  // scans would then wait on the query's client-context lock).
  duckdb::Connection c(db);
  duckdb::Connection qc(db);
  Q(c, "PRAGMA threads=1");
  Q(qc, "PRAGMA threads=1");
  std::unique_ptr<DuckdbRuntime> rt;
  if (system != "base") {
    accelerator_codegen::RegisterAcceleratorFunctions(qc);
    rt = std::make_unique<DuckdbRuntime>(c, EntryFor(system), 10.0);
    rt->Build();
  }
  RunResult out;
  double wsum = 0, qsum = 0;
  const auto t0 = Clock::now();
  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < k; ++i) {
      const Op op = gen.Next(mix);
      auto probes = Probes(c, op);
      const auto t = Clock::now();
      if (std::getenv("Q21V_TRACE"))
        std::cerr << "[w] " << system << " op " << op.kind << " ok " << op.ok << std::endl;
      if (rt) {
        rt->ApplyBatch(probes);
      } else {
        Q(c, "BEGIN");
        for (const auto &p : probes)
          Q(c, p.update_sql);
        Q(c, "COMMIT");
      }
      wsum += std::chrono::duration<double, std::milli>(Clock::now() - t).count();
      ++out.writes;
    }
    const auto &n = nations[qrng() % nations.size()];
    qsum += TimeMs(qc, system == "base" ? BaseSql(n) : AccelSql(FnFor(system), n));
    ++out.queries;
  }
  out.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  out.query_mean = qsum / out.queries;
  out.write_mean = wsum / out.writes;
  if (rt) {
    for (int i = 0; i < 4; ++i) {
      const auto &n = nations[(i * 7) % nations.size()];
      if (Fetch(qc, AccelSql(FnFor(system), n)) != Fetch(qc, BaseSql(n)))
        out.matches = false;
    }
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 2, "usage: q21_verus_bench <db> [rounds] [acc_reps] [base_reps]");
    const std::string path = argv[1];
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 20;
    const int acc_reps = argc > 3 ? std::atoi(argv[3]) : 30;
    const int base_reps = argc > 4 ? std::atoi(argv[4]) : 3;
    duckdb::DuckDB db(path);
    duckdb::Connection meta(db);
    std::vector<std::string> nations;
    {
      auto r = Q(meta, "SELECT n_name FROM nation ORDER BY n_nationkey");
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
        nations.push_back(r->GetValue(0, i).ToString());
    }
    std::vector<int64_t> all_f;
    {
      auto r = Q(meta, "SELECT DISTINCT o_orderkey FROM orders JOIN lineitem ON l_orderkey = o_orderkey "
                     "AND l_linenumber = 1 WHERE o_orderstatus = 'F' ORDER BY o_orderkey");
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
        all_f.push_back(r->GetValue(0, i).GetValue<int64_t>());
    }
    std::mt19937_64 srng(1);
    std::vector<int64_t> f_orders;
    for (int i = 0; i < 5000; ++i)
      f_orders.push_back(all_f[srng() % all_f.size()]);
    const int64_t n_supp = Q(meta, "SELECT count(*) FROM supplier")->GetValue(0, 0).GetValue<int64_t>();
    std::vector<std::pair<int64_t, int32_t>> points;
    {
      std::string in;
      for (auto k : f_orders)
        in += (in.empty() ? "" : ",") + std::to_string(k);
      auto r = Q(meta, "SELECT l_orderkey, l_receiptdate FROM lineitem WHERE l_linenumber = 1 AND l_orderkey IN (" + in + ")");
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
        points.push_back({r->GetValue(0, i).GetValue<int64_t>(), Days(r->GetValue(1, i))});
    }
    Restore(db, points);
    std::cout << "lineitem_rows " << Q(meta, "SELECT count(*) FROM lineitem")->GetValue(0, 0).ToString()
              << " duckdb " << duckdb::DuckDB::LibraryVersion() << "\n";

    const bool base_only = argc > 5 && std::string(argv[5]) == "base_only";
    // 1-2. differential check + read-only timings
    for (const std::string system : {"verus", "cpp"}) {
      if (base_only)
        break;
      duckdb::Connection c(db);
      Q(c, "PRAGMA threads=1");
      accelerator_codegen::RegisterAcceleratorFunctions(c);
      const auto t = Clock::now();
      DuckdbRuntime rt(c, EntryFor(system), 10.0);
      rt.Build();
      const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
      int bad = 0, rows = 0;
      std::vector<double> med;
      for (const auto &n : nations) {
        const auto a = Fetch(c, AccelSql(FnFor(system), n));
        const auto b = Fetch(c, BaseSql(n));
        rows += b.size();
        if (a != b) {
          ++bad;
          std::cout << "MISMATCH " << system << " " << n << " " << a.size() << " vs " << b.size() << "\n";
        }
        std::vector<double> ts;
        for (int i = 0; i < acc_reps; ++i)
          ts.push_back(TimeMs(c, AccelSql(FnFor(system), n)));
        med.push_back(Median(ts));
        std::cout << "ro " << system << " " << n << " " << Median(ts) << "\n";
      }
      std::cout << "diff " << system << " nations " << nations.size() << " rows " << rows
                << " mismatching " << bad << " build_ms " << build_ms
                << " median_over_nations_ms " << Median(med) << "\n";
    }
    for (const std::string threads : {"1", "all"}) {
      duckdb::Connection c(db);
      // threads is a database-wide setting: set it explicitly each time.
      Q(c, threads == "1" ? "SET threads = 1"
                          : "SET threads = " + std::to_string(std::thread::hardware_concurrency()));
      std::vector<double> med;
      for (const auto &n : nations) {
        TimeMs(c, BaseSql(n));
        std::vector<double> ts;
        for (int i = 0; i < base_reps; ++i)
          ts.push_back(TimeMs(c, BaseSql(n)));
        med.push_back(Median(ts));
        std::cout << "ro base_threads_" << threads << " " << n << " " << Median(ts) << "\n";
      }
      std::cout << "base threads=" << threads << " median_over_nations_ms " << Median(med) << "\n";
    }

    // 3. writes
    if (base_only)
      return 0;
    for (const std::string mix : {"point", "rf", "mixed"}) {
      for (int k : {1, 10}) {
        for (const std::string system : {"base", "verus", "cpp"}) {
          const auto r = WriteRun(db, system, mix, k, rounds, nations, f_orders, n_supp);
          Restore(db, points);
          std::cout << "writes " << mix << "/k=" << k << " " << system << " total_ms "
                    << r.total_ms << " query_mean_ms " << r.query_mean << " write_mean_ms "
                    << r.write_mean << " writes " << r.writes << " queries " << r.queries
                    << " matches_sql " << (system == "base" ? "-" : (r.matches ? "true" : "false"))
                    << std::endl;
        }
      }
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "q21_verus_bench failed: " << e.what() << "\n";
    return 1;
  }
}
