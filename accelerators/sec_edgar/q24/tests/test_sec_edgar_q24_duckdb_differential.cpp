// Q24 aux vs plain DuckDB SQL, end to end through DuckdbRuntime (real scans,
// real coordinator/state store, SQL table function).
//
//   test_sec_edgar_q24_duckdb_differential
//       Synthetic in-memory num/pre: randomized INSERT / DELETE / UPDATE on
//       both tables (join-key + filter-column moves, NULL keys, duplicates,
//       multi-statement transactions, bulk rebuilds) interleaved with reads;
//       every step compares the aux result (module Execute and the
//       accel_sec_edgar_q24_aux() table function) with the base SQL.
//   test_sec_edgar_q24_duckdb_differential <sec_edgar.duckdb> <work_copy.duckdb>
//       Additionally runs a scripted maintenance audit on a writable COPY of
//       the real database (pre insert/delete on a live orphan triple, new
//       group creation, filter moves) plus the generic harness differential.
//
// Q24 takes (year, uom): accel_sec_edgar_q24_aux(year BIGINT, uom VARCHAR).
// Every comparison of the synthetic runs also draws random (year, uom) pairs
// (benchmark domain plus edges: NULLs, unknown uom, years 0 / 2017 / 2025 /
// huge) and compares them with the plain query bound to them; the data is
// randomized around the boundaries of every year window (ddate with month/day
// outside 101..1231, year 0 and negative years) and the HAVING > 10 threshold.
//   Q24_SWEEP=1 test_sec_edgar_q24_duckdb_differential <db_copy> [threads]
//       Parameter sweep over the benchmark domain on a real database: build
//       time, accelerator vs plain latency, exactness for every combination.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eval/harness/differential.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "sec_edgar/q24/tests/q24_random_writes.hpp"

namespace {

using namespace q24test;
namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
using catalog::duckdb::DuckdbRuntime;

[[noreturn]] void Fail(const std::string &message) {
  std::cerr << "FAIL: " << message << "\n";
  std::abort();
}

void Require(bool condition, const std::string &message) {
  if (!condition) Fail(message);
}

std::string Lit(const OptStr &s) {
  if (!s) return "NULL";
  std::string out = "'";
  for (const char c : *s) {
    if (c == '\'') out += "''";
    else out += c;
  }
  return out + "'";
}

std::string LitD(const std::optional<double> &d) {
  if (!d) return "NULL";
  if (std::isinf(*d)) return *d > 0 ? "'inf'::DOUBLE" : "'-inf'::DOUBLE";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", *d);
  return std::string(buf) + "::DOUBLE";
}

std::string LitI(const std::optional<int64_t> &i) {
  return i ? std::to_string(*i) : "NULL";
}

void Exec(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), sql + " -> " + (r ? r->GetError() : "null"));
}

std::string NumValues(const NumRow &r) {
  return "(" + Lit(r.adsh) + "," + Lit(r.tag) + "," + Lit(r.version) + "," +
         LitI(r.ddate) + "," + std::to_string(r.qtrs) + "," + Lit(r.uom) + "," +
         Lit(r.coreg) + "," + LitD(r.value) + ", NULL)";
}

std::string PreValues(const PreRow &r) {
  return "(" + Lit(r.adsh) + ",1,1,'IS',0,'H'," + Lit(r.tag) + "," +
         Lit(r.version) + ",'label',0)";
}

std::string NumMatch(const NumRow &r) {
  return "adsh IS NOT DISTINCT FROM " + Lit(r.adsh) +
         " AND tag IS NOT DISTINCT FROM " + Lit(r.tag) +
         " AND version IS NOT DISTINCT FROM " + Lit(r.version) +
         " AND ddate IS NOT DISTINCT FROM " + LitI(r.ddate) +
         " AND qtrs = " + std::to_string(r.qtrs) +
         " AND uom IS NOT DISTINCT FROM " + Lit(r.uom) +
         " AND coreg = " + Lit(r.coreg) +
         " AND value IS NOT DISTINCT FROM " + LitD(r.value);
}

std::string PreMatch(const PreRow &r) {
  return "adsh IS NOT DISTINCT FROM " + Lit(r.adsh) +
         " AND tag IS NOT DISTINCT FROM " + Lit(r.tag) +
         " AND version IS NOT DISTINCT FROM " + Lit(r.version);
}

struct World {
  duckdb::DuckDB db{nullptr};
  duckdb::Connection con{db};
  const catalog::SharedQueryCatalogEntry *entry = nullptr;
  std::unique_ptr<DuckdbRuntime> runtime;
  std::vector<NumRow> num;
  std::vector<PreRow> pre;
  int rebuilds_seen = 0;

  World() {
    accelerator_codegen::RegisterAcceleratorFunctions(con);
    Exec(con,
         "CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, ddate "
         "INTEGER, qtrs INTEGER, uom VARCHAR, coreg VARCHAR, value DOUBLE, "
         "footnote VARCHAR)");
    Exec(con,
         "CREATE TABLE pre(adsh VARCHAR, report INTEGER, line INTEGER, stmt "
         "VARCHAR, inpth INTEGER, rfile VARCHAR, tag VARCHAR, version VARCHAR, "
         "plabel VARCHAR, negating INTEGER)");
    // Q24_PARALLEL=1 (build with -DACCEL_ENABLE_PARALLEL_AUX=ON) exercises the
    // multi-threaded-build module under the same differential.
    entry = std::getenv("Q24_PARALLEL") ? &catalog::LookupSharedQueryParallel("sec_edgar", 24)
                                        : &catalog::LookupSharedQuery("sec_edgar", 24);
  }

  void InsertNum(const std::vector<NumRow> &rows) {
    for (std::size_t i = 0; i < rows.size(); i += 500) {
      std::string sql = "INSERT INTO num VALUES ";
      for (std::size_t j = i; j < std::min(rows.size(), i + 500); ++j) {
        if (j != i) sql += ",";
        sql += NumValues(rows[j]);
      }
      Exec(con, sql);
    }
  }
  void InsertPre(const std::vector<PreRow> &rows) {
    for (std::size_t i = 0; i < rows.size(); i += 500) {
      std::string sql = "INSERT INTO pre VALUES ";
      for (std::size_t j = i; j < std::min(rows.size(), i + 500); ++j) {
        if (j != i) sql += ",";
        sql += PreValues(rows[j]);
      }
      Exec(con, sql);
    }
  }
  void DeleteNum(const NumRow &r) {
    Exec(con, "DELETE FROM num WHERE rowid IN (SELECT rowid FROM num WHERE " +
                  NumMatch(r) + " LIMIT 1)");
  }
  void DeletePre(const PreRow &r) {
    Exec(con, "DELETE FROM pre WHERE rowid IN (SELECT rowid FROM pre WHERE " +
                  PreMatch(r) + " LIMIT 1)");
  }
  void UpdateNum(const NumRow &o, const NumRow &n) {
    Exec(con, "UPDATE num SET adsh=" + Lit(n.adsh) + ", tag=" + Lit(n.tag) +
                  ", version=" + Lit(n.version) + ", ddate=" + LitI(n.ddate) +
                  ", qtrs=" + std::to_string(n.qtrs) + ", uom=" + Lit(n.uom) +
                  ", coreg=" + Lit(n.coreg) + ", value=" + LitD(n.value) +
                  " WHERE rowid IN (SELECT rowid FROM num WHERE " + NumMatch(o) +
                  " LIMIT 1)");
  }
  void UpdatePre(const PreRow &o, const PreRow &n) {
    Exec(con, "UPDATE pre SET adsh=" + Lit(n.adsh) + ", tag=" + Lit(n.tag) +
                  ", version=" + Lit(n.version) +
                  " WHERE rowid IN (SELECT rowid FROM pre WHERE " + PreMatch(o) +
                  " LIMIT 1)");
  }

};

std::vector<std::string> SortedRows(duckdb::MaterializedQueryResult &r) {
  std::vector<std::string> rows;
  for (duckdb::idx_t i = 0; i < r.RowCount(); ++i) {
    std::string row;
    for (duckdb::idx_t c = 0; c < r.ColumnCount(); ++c) {
      const auto v = r.GetValue(c, i);
      if (c == 3 && !v.IsNull()) {
        // Round doubles so that last-bit summation-order differences compare
        // equal (relative 1e-9 bucket handled in CompareTotals below).
        row += "#";
      } else {
        row += v.IsNull() ? "<null>" : v.ToString();
      }
      row += "\t";
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

// Order-insensitive comparison (ties in cnt order arbitrarily in DuckDB) with
// a relative 1e-9 tolerance on the SUM column.
void CompareResults(duckdb::Connection &con, const std::string &label,
                    const std::string &expected_sql, const std::string &actual_sql) {
  auto e = con.Query(expected_sql);
  auto a = con.Query(actual_sql);
  Require(e && !e->HasError(), label + ": reference: " + (e ? e->GetError() : ""));
  Require(a && !a->HasError(), label + ": aux: " + (a ? a->GetError() : ""));
  auto &em = e->Cast<duckdb::MaterializedQueryResult>();
  auto &am = a->Cast<duckdb::MaterializedQueryResult>();
  Require(em.RowCount() == am.RowCount(),
          label + ": row count " + std::to_string(am.RowCount()) + " vs " +
              std::to_string(em.RowCount()));
  const auto erows = SortedRows(em);
  const auto arows = SortedRows(am);
  Require(erows == arows, label + ": key/count mismatch");
  // Key (tag, version) -> total, compared with tolerance.
  std::map<std::string, double> et;
  for (duckdb::idx_t i = 0; i < em.RowCount(); ++i)
    et[em.GetValue(0, i).ToString() + "|" + em.GetValue(1, i).ToString()] =
        em.GetValue(3, i).GetValue<double>();
  for (duckdb::idx_t i = 0; i < am.RowCount(); ++i) {
    const auto key = am.GetValue(0, i).ToString() + "|" + am.GetValue(1, i).ToString();
    const double x = am.GetValue(3, i).GetValue<double>();
    const double y = et.at(key);
    const double scale = std::max({1.0, std::abs(x), std::abs(y)});
    Require(std::abs(x - y) <= scale * 1e-9 || x == y,
            label + ": total mismatch for " + key + " " + std::to_string(x) +
                " vs " + std::to_string(y));
  }
}

// Q24 with its two parameters bound: n.uom = :uom, n.ddate BETWEEN
// :year*10000+101 AND :year*10000+1231 (HAVING / LIMIT fixed). A NULL parameter
// yields the empty result in SQL. Years beyond +-10^9 would overflow BIGINT in
// the reference; no INTEGER ddate can lie there, so they compare as 10^9.
std::string ParamBaseSql(std::optional<int64_t> year, const OptStr &uom) {
  int64_t y = year ? *year : 0;
  if (y > 1000000000LL) y = 1000000000LL;
  if (y < -1000000000LL) y = -1000000000LL;
  const std::string ys = year ? std::to_string(y) : "NULL";
  return "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
         "FROM num n LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version "
         "AND n.adsh = p.adsh WHERE n.uom = " + Lit(uom) +
         " AND n.ddate BETWEEN " + ys + "::BIGINT*10000+101 AND " + ys +
         "::BIGINT*10000+1231 AND n.value IS NOT NULL AND p.adsh IS NULL "
         "GROUP BY n.tag, n.version HAVING COUNT(*) > 10 ORDER BY cnt DESC "
         "LIMIT 100";
}

std::string ParamAuxSql(const std::string &aux_sql, std::optional<int64_t> year,
                        const OptStr &uom) {
  std::string sql = aux_sql;
  const auto pos = sql.rfind("()");
  Require(pos != std::string::npos, "aux sql has no empty argument list");
  return sql.substr(0, pos) + "(" +
         (year ? std::to_string(*year) : std::string("NULL")) + "::BIGINT, " +
         (uom ? Lit(uom) : std::string("NULL")) + "::VARCHAR)" + sql.substr(pos + 2);
}

int g_param_nonempty = 0, g_param_total = 0;

void CheckParams(World &w, const std::string &label, std::optional<int64_t> year,
                 const OptStr &uom) {
  const std::string l = label + " [year=" + (year ? std::to_string(*year) : "NULL") +
                        " uom=" + (uom ? *uom : "NULL") + "]";
  CompareResults(w.con, l, ParamBaseSql(year, uom),
                 ParamAuxSql(w.entry->duckdb_aux_sql, year, uom));
  ++g_param_total;
  auto r = w.con.Query(ParamBaseSql(year, uom));
  if (r && !r->HasError() && r->Cast<duckdb::MaterializedQueryResult>().RowCount() > 0)
    ++g_param_nonempty;
}

void CheckAll(World &w, const std::string &label, Gen *g = nullptr) {
  const std::string aux_sql = w.entry->duckdb_aux_sql;
  CompareResults(w.con, label + " [table function]", w.entry->base_sql, aux_sql);
  if (g) {
    for (int i = 0; i < 2; ++i) {
      const auto [year, uom] = g->Params();
      CheckParams(w, label, year, uom);
    }
    // Hot buckets hold the HAVING > 10 groups of the synthetic data.
    CheckParams(w, label, g->R(2) ? 2023 : 2024, OptStr(g->R(3) ? "USD" : "EUR"));
  }
  // Module-level Execute on the same published state.
  const auto rows = w.runtime->ExecuteCollect();
  auto base = w.con.Query(w.entry->base_sql);
  Require(base && !base->HasError(), "base failed");
  Require(base->Cast<duckdb::MaterializedQueryResult>().RowCount() == rows.size(),
          label + " [runtime]: row count");
}

// Mirrors one generated change into SQL (the generator already applied it to
// the shadow tables).
void ReplayToSql(World &w, const MutationBatch &m, const Typed &t) {
  if (m.table == 101) {
    switch (m.kind) {
    case MutationKind::Insert: w.InsertNum(t.n_new); break;
    case MutationKind::Delete: for (const auto &r : t.n_old) w.DeleteNum(r); break;
    case MutationKind::Update:
      for (std::size_t i = 0; i < t.n_old.size(); ++i) w.UpdateNum(t.n_old[i], t.n_new[i]);
      break;
    }
  } else {
    switch (m.kind) {
    case MutationKind::Insert: w.InsertPre(t.p_new); break;
    case MutationKind::Delete: for (const auto &r : t.p_old) w.DeletePre(r); break;
    case MutationKind::Update:
      for (std::size_t i = 0; i < t.p_old.size(); ++i) w.UpdatePre(t.p_old[i], t.p_new[i]);
      break;
    }
  }
}

void RunSynthetic(uint64_t seed, bool nulls, int steps) {
  World w;
  Gen g(seed, nulls);
  for (int i = 0; i < 2500; ++i) w.num.push_back(g.Num());
  for (int i = 0; i < 150; ++i) w.pre.push_back(g.Pre());
  w.InsertNum(w.num);
  w.InsertPre(w.pre);
  w.runtime = std::make_unique<DuckdbRuntime>(w.con, *w.entry, 10.0);
  w.runtime->Build();
  CheckAll(w, "initial", &g);

  int applied = 0;
  for (int step = 0; step < steps; ++step) {
    bool inc = true;
    int kind = 0;
    Typed typed;
    MutationBatch m = RandomWrite(g, w.num, w.pre, inc, kind, typed);
    if (m.old_rows.empty() && m.new_rows.empty()) continue;
    ReplayToSql(w, m, typed);
    w.runtime->ApplyMutationBatch(m);
    applied++;
    CheckAll(w, "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                    " kind " + std::to_string(kind), &g);
  }
  // Bulk statements beyond the ApplyDelta row cap (rebuild path).
  {
    std::vector<NumRow> rows;
    for (int i = 0; i < 2500; ++i) rows.push_back(g.Num());
    w.InsertNum(rows);
    for (const auto &r : rows) w.num.push_back(r);
    MutationBatch m;
    m.table = 101;
    m.kind = MutationKind::Insert;
    for (const auto &r : rows) m.new_rows.push_back(ToAux(r));
    w.runtime->ApplyMutationBatch(m);
    CheckAll(w, "bulk insert", &g);
    Exec(w.con, "DELETE FROM num WHERE qtrs = 3");
    MutationBatch d;
    d.table = 101;
    d.kind = MutationKind::Delete;
    std::vector<NumRow> keep;
    for (const auto &r : w.num) {
      if (r.qtrs == 3) d.old_rows.push_back(ToAux(r));
      else keep.push_back(r);
    }
    w.num = keep;
    w.runtime->ApplyMutationBatch(d);
    CheckAll(w, "bulk delete", &g);
  }
  std::cout << "synthetic seed " << seed << " nulls=" << nulls << ": " << applied
            << " writes verified\n";
}

// Parameter sweep over the benchmark domain plus edges on a real database.
int RunSweep(const std::string &db_path, int threads) {
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  };
  duckdb::DuckDB db(db_path.c_str());
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  Exec(con, "PRAGMA threads=" + std::to_string(threads));
  const auto &entry = std::getenv("Q24_PARALLEL") ? catalog::LookupSharedQueryParallel("sec_edgar", 24)
                                                   : catalog::LookupSharedQuery("sec_edgar", 24);
  DuckdbRuntime runtime(con, entry, 10.0);
  auto t0 = Clock::now();
  runtime.Build();
  std::cout << "build: " << ms(t0) << " ms (threads=" << threads << ")\n";
  std::vector<std::pair<std::optional<int64_t>, OptStr>> combos;
  for (const int64_t y : {2019, 2020, 2021, 2022, 2023, 2024})
    for (const char *u : {"USD", "shares", "pure", "CAD", "CNY", "EUR", "no_such_uom"})
      combos.push_back({y, std::string(u)});
  for (const int64_t y : std::initializer_list<int64_t>{2017, 2025, 0, -1, 4000000000000000000LL})
    for (const char *u : {"USD", "shares"}) combos.push_back({y, std::string(u)});
  combos.push_back({std::nullopt, std::string("USD")});
  combos.push_back({int64_t{2023}, std::nullopt});
  combos.push_back({std::nullopt, std::nullopt});
  const std::string aux_base = entry.duckdb_aux_sql;
  int nonempty = 0;
  double aux_sum = 0, aux_max = 0, plain_sum = 0;
  for (const auto &[year, uom] : combos) {
    const std::string label = "sweep year=" + (year ? std::to_string(*year) : std::string("NULL")) +
                              " uom=" + (uom ? *uom : std::string("NULL"));
    const std::string aux_sql = ParamAuxSql(aux_base, year, uom);
    Exec(con, aux_sql); // warm
    const auto ta = Clock::now();
    constexpr int kReps = 20;
    for (int i = 0; i < kReps; ++i) Exec(con, aux_sql);
    const double aux_ms = ms(ta) / kReps;
    const auto tp = Clock::now();
    auto want = con.Query(ParamBaseSql(year, uom));
    const double plain_ms = ms(tp);
    Require(want && !want->HasError(), label + ": reference failed");
    const auto rows = want->Cast<duckdb::MaterializedQueryResult>().RowCount();
    nonempty += rows > 0;
    CompareResults(con, label, ParamBaseSql(year, uom), aux_sql);
    aux_sum += aux_ms;
    aux_max = std::max(aux_max, aux_ms);
    plain_sum += plain_ms;
    std::printf("  %-34s rows=%3zu accel=%8.3f ms plain=%9.1f ms\n", label.c_str(),
                static_cast<std::size_t>(rows), aux_ms, plain_ms);
  }
  std::printf("sweep ok: %zu combinations (%d non-empty); accel mean %.3f ms max %.3f ms; "
              "plain mean %.1f ms\n",
              combos.size(), nonempty, aux_sum / combos.size(), aux_max,
              plain_sum / combos.size());
  return 0;
}

} // namespace

// Regression: tags of equal length sharing their first 24 bytes (common in
// us-gaap, e.g. IncreaseDecreaseInAccounts...) are distinct groups.  The base
// kernel keyed (tag, version) by a 24-byte prefix and merged them into one row.
void CheckTagPrefixCollision() {
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  Exec(con, "CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, ddate INTEGER, "
            "qtrs INTEGER, uom VARCHAR, coreg VARCHAR, value DOUBLE, footnote VARCHAR)");
  Exec(con, "CREATE TABLE pre(adsh VARCHAR, report INTEGER, line INTEGER, stmt VARCHAR, "
            "inpth INTEGER, rfile VARCHAR, tag VARCHAR, version VARCHAR, plabel VARCHAR, "
            "negating INTEGER)");
  const std::string tags[] = {"IncreaseDecreaseInAccountsPayableTrade",
                              "IncreaseDecreaseInAccountsReceivableXY"};
  std::string sql = "INSERT INTO num VALUES ";
  for (int t = 0; t < 2; ++t)
    for (int i = 0; i < 12 + 3 * t; ++i)
      sql += std::string(t + i ? "," : "") + "('0000000001-23-" + std::to_string(100 + i) +
             "'," + Lit(tags[t]) + ",'us-gaap/2023',20230601,4,'USD',NULL," +
             std::to_string((t + 1) * 10 + i) + ",NULL)";
  Exec(con, sql);
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 24);
  CompareResults(con, "tag prefix collision (base kernel)", entry.base_sql,
                 "SELECT * FROM accel_sec_edgar_q24()");
  std::cout << "tag prefix collision ok\n";
}

int main(int argc, char **argv) {
  try {
    if (std::getenv("Q24_SWEEP") && argc >= 2)
      return RunSweep(argv[1], argc > 2 ? std::atoi(argv[2]) : 12);
    const int seeds = std::getenv("Q24_SEEDS") ? std::atoi(std::getenv("Q24_SEEDS")) : 3;
    const int steps = std::getenv("Q24_STEPS") ? std::atoi(std::getenv("Q24_STEPS")) : 120;
    CheckTagPrefixCollision();
    for (int s = 1; s <= seeds; ++s) {
      RunSynthetic(500 + s, /*nulls=*/false, steps);
      RunSynthetic(600 + s, /*nulls=*/true, steps);
    }

    if (argc >= 3) {
      namespace fs = std::filesystem;
      const fs::path source = argv[1], work = argv[2];
      Require(source != work, "work copy must differ from source");
      fs::copy_file(source, work, fs::copy_options::overwrite_existing);
      const auto &entry = catalog::LookupSharedQuery("sec_edgar", 24);
      std::cerr << "real DB: read-only differential\n";
      harness::RunDifferentialCheck(work.string(), entry);
      std::cerr << "real DB: incremental differential\n";
      harness::RunIncrementalDifferentialCheck(work.string(), entry);
      std::cerr << "real DB: adversarial writes\n";
      harness::RunAdversarialWriteDifferentialCheck(work.string(), entry, 5, 0, false);
      std::cerr << "real DB: scripted maintenance audit\n";
      duckdb::DuckDB db(work.string().c_str());
      duckdb::Connection con(db);
      accelerator_codegen::RegisterAcceleratorFunctions(con);
      Exec(con, "PRAGMA threads=8");
      DuckdbRuntime runtime(con, entry, 10.0);
      runtime.Build();
      auto check = [&](const std::string &label) {
        CompareResults(con, label, entry.base_sql, "SELECT * FROM accel_sec_edgar_q24_aux()");
        for (const auto &[y, u] : std::vector<std::pair<int64_t, std::string>>{
                 {2022, "USD"}, {2023, "shares"}, {2024, "USD"}, {2021, "pure"}, {2023, "EUR"}})
          CompareResults(con, label + " " + std::to_string(y) + "/" + u,
                         ParamBaseSql(y, u),
                         "SELECT * FROM accel_sec_edgar_q24_aux(" + std::to_string(y) +
                             ", '" + u + "')");
        harness::RequireSharedMatchesBase(con, runtime, entry);
        std::cerr << "  ok: " << label << "\n";
      };
      check("real initial");
      // Live orphan triple belonging to the top group.
      auto top = con.Query(
          "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
          "n.value FROM num n LEFT JOIN pre p ON n.tag=p.tag AND "
          "n.version=p.version AND n.adsh=p.adsh WHERE n.uom='USD' AND n.ddate "
          "BETWEEN 20230101 AND 20231231 AND n.value IS NOT NULL AND p.adsh IS "
          "NULL ORDER BY n.tag, n.version, n.adsh, n.value LIMIT 1");
      Require(top && !top->HasError() && top->RowCount() == 1, "no orphan row in DB");
      NumRow orphan;
      orphan.adsh = top->GetValue(0, 0).ToString();
      orphan.tag = top->GetValue(1, 0).ToString();
      orphan.version = top->GetValue(2, 0).ToString();
      orphan.ddate = top->GetValue(3, 0).GetValue<int64_t>();
      orphan.qtrs = top->GetValue(4, 0).GetValue<int64_t>();
      orphan.uom = top->GetValue(5, 0).ToString();
      orphan.coreg = top->GetValue(6, 0).IsNull() ? "" : top->GetValue(6, 0).ToString();
      orphan.value = top->GetValue(7, 0).GetValue<double>();
      // 1. pre INSERT matching the orphan triple removes it from the result.
      PreRow match{orphan.adsh, orphan.tag, orphan.version};
      Exec(con, "INSERT INTO pre VALUES (" + Lit(match.adsh) + ",9999,9999,'IS',0,'H'," +
                    Lit(match.tag) + "," + Lit(match.version) + ",'x',0)");
      MutationBatch pi;
      pi.table = 102; pi.kind = MutationKind::Insert; pi.new_rows = {ToAux(match)};
      runtime.ApplyMutationBatch(pi);
      check("real pre insert (retract orphans)");
      // 2. pre DELETE of that row brings them back (rebuild path).
      Exec(con, "DELETE FROM pre WHERE adsh=" + Lit(match.adsh) + " AND report=9999");
      MutationBatch pd;
      pd.table = 102; pd.kind = MutationKind::Delete; pd.old_rows = {ToAux(match)};
      runtime.ApplyMutationBatch(pd);
      check("real pre delete (restore orphans)");
      // 3. num INSERT: 12 fresh orphan rows create a brand-new visible group.
      std::vector<NumRow> fresh;
      for (int i = 0; i < 12; ++i) {
        NumRow r = orphan;
        r.tag = "Q24NewTag";
        r.version = "q24/test";
        r.value = 100.0 + i;
        fresh.push_back(r);
      }
      {
        std::string sql = "INSERT INTO num VALUES ";
        for (std::size_t i = 0; i < fresh.size(); ++i)
          sql += (i ? "," : "") + NumValues(fresh[i]);
        Exec(con, sql);
        MutationBatch ni;
        ni.table = 101; ni.kind = MutationKind::Insert;
        for (const auto &r : fresh) ni.new_rows.push_back(ToAux(r));
        runtime.ApplyMutationBatch(ni);
        check("real num insert (new group appears)");
      }
      // 4. UPDATE ddate out of range removes one row; value update changes sum.
      {
        NumRow moved = fresh[0];
        NumRow after = moved;
        after.ddate = 20240105;
        Exec(con, "UPDATE num SET ddate=20240105 WHERE tag='Q24NewTag' AND value=" +
                      LitD(moved.value));
        MutationBatch u;
        u.table = 101; u.kind = MutationKind::Update; u.updated_columns = {3};
        u.old_rows = {ToAux(moved)}; u.new_rows = {ToAux(after)};
        runtime.ApplyMutationBatch(u);
        check("real ddate move out of range (group drops to HAVING edge)");
        Exec(con, "UPDATE num SET value=value+1000 WHERE tag='Q24NewTag' AND value=" +
                      LitD(fresh[1].value));
        NumRow v1 = fresh[1];
        NumRow v2 = v1;
        v2.value = *v1.value + 1000;
        MutationBatch u2;
        u2.table = 101; u2.kind = MutationKind::Update; u2.updated_columns = {7};
        u2.old_rows = {ToAux(v1)}; u2.new_rows = {ToAux(v2)};
        runtime.ApplyMutationBatch(u2);
        check("real value update");
      }
      // 5. DELETE all new rows.
      Exec(con, "DELETE FROM num WHERE tag='Q24NewTag'");
      MutationBatch nd;
      nd.table = 101; nd.kind = MutationKind::Delete;
      for (auto r : fresh) {
        if (*r.value == 100.0) r.ddate = 20240105;
        if (*r.value == 101.0) r.value = 1101.0;
        nd.old_rows.push_back(ToAux(r));
      }
      runtime.ApplyMutationBatch(nd);
      check("real num delete (group disappears)");
    }
    std::cout << "parameter comparisons: " << g_param_total << " (" << g_param_nonempty
              << " non-empty)\n";
    Require(g_param_nonempty > 20, "parameter checks never reached a populated bucket");
    std::cout << "test_sec_edgar_q24_duckdb_differential PASS\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "FAIL: " << ex.what() << "\n";
    return 1;
  }
}
