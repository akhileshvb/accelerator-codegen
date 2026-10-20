// PostgreSQL differential / check / benchmark for the SEC EDGAR Q6 aux
// accelerator (accel_sec_edgar_q6_aux() and accel_sec_edgar_q6_aux(fy, stmt,
// uom)) against plain PostgreSQL SQL (workloads/sec_edgar/q6.sql with
// the three parameters substituted). Every comparison also draws random
// parameters (benchmark domain plus edges: NULL, unknown, look-alike values).
//
// Talks to a running server through libpq only (the accelerator lives in the
// server's accelerator_codegen_postgres module, installed by the pg helper
// scripts). Modes:
//
//   synthetic [seed] [steps]
//       Creates the scratch database q6_synth (C collation, extension,
//       capture triggers, plain tables with the SEC EDGAR indexes) and runs
//       randomized INSERT / DELETE / UPDATE / TRUNCATE on num, sub and pre:
//       cohort entry and exit (fy), IS <-> non-IS flips (stmt), join-key
//       updates, look-alike units / statements, NULL labels, orphan keys,
//       multi-statement transactions, ROLLBACK, SAVEPOINT / ROLLBACK TO and a
//       second connection. After every step the accelerator answer must equal
//       the plain SQL answer exactly (values are multiples of 0.5, so
//       double sums are exact in any order).
//   sf3 [db]
//       Check on the full data of `db` (default sec_edgar): pristine match,
//       then committed + rolled-back writes of every kind, each undone, with a
//       match after every step (sums compared to a relative 1e-9: plain
//       SUM(float8) is order dependent).
//   sweep [db] [percent=100]
//       The full (fy, stmt, uom) domain (fy 2021..2024 x BS/IS/CF/EQ/CI x
//       USD/shares/pure/CAD/CNY/EUR, sampled by `percent`) plus edge values on
//       the full data of `db` against plain SQL; also times the reads.
//   bench [db]
//       Read p50 (accel vs plain SQL), build time and write overhead.
//
// Connection: libpq environment (PGHOST, PGPORT, PGUSER, PGPASSWORD).
// ACCEL_Q6_TRIGGER_SQL = path of scripts/postgres_accel_mutation_triggers.sql.

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Cell = std::optional<std::string>;
using Row = std::vector<Cell>;
using Rows = std::vector<Row>;

const char *kBaseSql =
    "SELECT s.name, p.stmt, n.tag, p.plabel, SUM(n.value) AS total_value, COUNT(*) AS cnt "
    "FROM num n JOIN sub s ON n.adsh = s.adsh "
    "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
    "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 AND n.value IS NOT NULL "
    "GROUP BY s.name, p.stmt, n.tag, p.plabel "
    "ORDER BY total_value DESC, s.name, n.tag, p.plabel LIMIT 200";
const char *kAccelSql = "SELECT * FROM accel_sec_edgar_q6_aux()";

struct Params {
  std::string fy = "2023";      // SQL literal ("NULL" allowed)
  std::string stmt = "'IS'";    // SQL literal
  std::string uom = "'USD'";    // SQL literal
  std::string AccelSql() const {
    return "SELECT * FROM accel_sec_edgar_q6_aux(" + (fy == "NULL" ? std::string("NULL::bigint") : fy + "::bigint") +
           "," + stmt + "::text," + uom + "::text)";
  }
  std::string BaseSql() const {
    return "SELECT s.name, p.stmt, n.tag, p.plabel, SUM(n.value) AS total_value, COUNT(*) AS cnt "
           "FROM num n JOIN sub s ON n.adsh = s.adsh "
           "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
           "WHERE n.uom = " + uom + " AND p.stmt = " + stmt + " AND s.fy = " + fy +
           " AND n.value IS NOT NULL GROUP BY s.name, p.stmt, n.tag, p.plabel "
           "ORDER BY total_value DESC, s.name, n.tag, p.plabel LIMIT 200";
  }
  std::string Text() const { return "(" + fy + "," + stmt + "," + uom + ")"; }
};

[[noreturn]] void Fail(const std::string &msg) {
  std::cerr << "FAIL: " << msg << std::endl;
  std::exit(1);
}

class Conn {
public:
  explicit Conn(const std::string &db) {
    const char *host = std::getenv("PGHOST");
    const char *port = std::getenv("PGPORT");
    const char *user = std::getenv("PGUSER");
    const char *pass = std::getenv("PGPASSWORD");
    std::string info = std::string("host=") + (host ? host : "127.0.0.1") +
                       " port=" + (port ? port : "55406") +
                       " user=" + (user ? user : "postgres") +
                       " password=" + (pass ? pass : "postgres") + " dbname=" + db;
    conn_ = PQconnectdb(info.c_str());
    if (PQstatus(conn_) != CONNECTION_OK)
      Fail(std::string("connect: ") + PQerrorMessage(conn_));
    Exec("SET extra_float_digits = 3");
    Exec("SET client_min_messages = error");
  }
  ~Conn() { PQfinish(conn_); }
  Conn(const Conn &) = delete;

  // Returns false (and the message) on error instead of throwing.
  bool Try(const std::string &sql, Rows *out = nullptr, std::string *err = nullptr) {
    PGresult *res = PQexec(conn_, sql.c_str());
    const auto status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
      if (err) *err = PQerrorMessage(conn_);
      PQclear(res);
      return false;
    }
    if (out) {
      out->clear();
      const int rows = PQntuples(res), cols = PQnfields(res);
      for (int r = 0; r < rows; ++r) {
        Row row;
        for (int c = 0; c < cols; ++c)
          row.push_back(PQgetisnull(res, r, c) ? Cell() : Cell(PQgetvalue(res, r, c)));
        out->push_back(std::move(row));
      }
    }
    PQclear(res);
    return true;
  }
  Rows Exec(const std::string &sql) {
    Rows rows;
    std::string err;
    if (!Try(sql, &rows, &err)) Fail(sql.substr(0, 300) + "\n  -> " + err);
    return rows;
  }
  std::string Scalar(const std::string &sql) {
    auto rows = Exec(sql);
    return rows.empty() || rows[0].empty() || !rows[0][0] ? "" : *rows[0][0];
  }

private:
  PGconn *conn_;
};

double Seconds(std::chrono::steady_clock::time_point a) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}

std::string Show(const Rows &rows, std::size_t limit = 8) {
  std::ostringstream o;
  for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
    o << "  [" << i << "]";
    for (const auto &c : rows[i]) o << " | " << (c ? *c : std::string("<NULL>"));
    o << "\n";
  }
  if (rows.size() > limit) o << "  ... (" << rows.size() << " rows)\n";
  return o.str();
}

bool Equal(const Rows &a, const Rows &b, double rel_tol, std::string *why);

// Plain SUM(float8) depends on the summation order (the aux sums are
// order-independent double-doubles): groups whose sums agree to rel_tol may be
// ordered differently. Inside such a tie cluster the multiset of rows must
// agree (the cluster cut by LIMIT 200 only for its sums).
bool EqualTolerant(const Rows &a, const Rows &b, double rel_tol, std::string *why) {
  if (a.size() != b.size()) return Equal(a, b, rel_tol, why);
  const auto num = [](const Row &r) { return std::strtod(r[4]->c_str(), nullptr); };
  const auto close = [&](double x, double y) {
    return x == y || std::fabs(x - y) <= rel_tol * std::max(std::fabs(x), std::fabs(y));
  };
  const auto key = [](const Row &r) {
    std::string k;
    for (std::size_t c : {0, 1, 2, 3, 5}) k += (r[c] ? *r[c] : std::string("\x01NULL")) + '\x02';
    return k;
  };
  for (std::size_t i = 0; i < b.size();) {
    std::size_t j = i + 1;
    while (j < b.size() && close(num(b[j]), num(b[i]))) ++j;
    std::vector<std::string> ka, kb;
    for (std::size_t k = i; k < j; ++k) {
      if (!close(num(a[k]), num(b[k]))) {
        *why = "sum at row " + std::to_string(k);
        return false;
      }
      ka.push_back(key(a[k]));
      kb.push_back(key(b[k]));
    }
    if (j < b.size() || b.size() < 200) {
      std::sort(ka.begin(), ka.end());
      std::sort(kb.begin(), kb.end());
      if (ka != kb) {
        *why = "rows differ in tie cluster starting at " + std::to_string(i);
        return false;
      }
    }
    i = j;
  }
  return true;
}

bool Equal(const Rows &a, const Rows &b, double rel_tol, std::string *why) {
  if (rel_tol > 0 && a.size() == b.size() && !a.empty() && a[0].size() == 6) {
    bool exact = true;
    for (std::size_t i = 0; exact && i < a.size(); ++i)
      for (std::size_t c = 0; c < 6; ++c)
        if (c != 4 && a[i][c] != b[i][c]) exact = false;
    if (!exact) return EqualTolerant(a, b, rel_tol, why);
  }
  if (a.size() != b.size()) {
    *why = "row count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    for (std::size_t c = 0; c < 6; ++c) {
      const Cell &x = a[i][c], &y = b[i][c];
      bool ok;
      if (c == 4 && x && y) {
        const double dx = std::strtod(x->c_str(), nullptr), dy = std::strtod(y->c_str(), nullptr);
        ok = dx == dy || std::fabs(dx - dy) <= rel_tol * std::max(std::fabs(dx), std::fabs(dy));
      } else {
        ok = x == y;
      }
      if (!ok) {
        *why = "row " + std::to_string(i) + " col " + std::to_string(c) + ": '" +
               (x ? *x : "<NULL>") + "' vs '" + (y ? *y : "<NULL>") + "'";
        return false;
      }
    }
  }
  return true;
}

struct Checker {
  double rel_tol = 0.0;
  long checks = 0;
  std::string last_ops;
  std::mt19937_64 param_rng{0x5EED};
  int param_checks = 2; // random parameter triples per Check
  Params RandomParams() {
    static const char *fys[] = {"2021", "2022", "2023", "2023", "2024", "2019", "2025", "0", "NULL"};
    static const char *stmts[] = {"'BS'", "'IS'", "'IS'", "'CF'", "'EQ'", "'CI'", "'ZZ'", "'is'", "NULL"};
    static const char *uoms[] = {"'USD'", "'USD'", "'shares'", "'pure'", "'CAD'", "'CNY'", "'EUR'", "'zz'", "'usd'", "NULL"};
    Params p;
    p.fy = fys[param_rng() % (sizeof fys / sizeof *fys)];
    p.stmt = stmts[param_rng() % (sizeof stmts / sizeof *stmts)];
    p.uom = uoms[param_rng() % (sizeof uoms / sizeof *uoms)];
    return p;
  }
  void CheckOne(Conn &c, const std::string &where, const std::string &accel_sql,
                const std::string &base_sql, const std::string &params) {
    Rows accel = c.Exec(accel_sql), base = c.Exec(base_sql);
    std::string why;
    ++checks;
    if (!Equal(accel, base, rel_tol, &why)) {
      std::cerr << "MISMATCH at " << where << " params " << params << ": " << why << "\nrecent ops:\n"
                << last_ops << "accel:\n" << Show(accel) << "base:\n" << Show(base);
      std::exit(1);
    }
  }
  void Check(Conn &c, const std::string &where) {
    CheckOne(c, where, kAccelSql, kBaseSql, "default");
    for (int i = 0; i < param_checks; ++i) {
      const Params p = RandomParams();
      CheckOne(c, where, p.AccelSql(), p.BaseSql(), p.Text());
    }
  }
  void Note(const std::string &op) {
    last_ops += "  " + op + "\n";
    if (last_ops.size() > 6000) last_ops.erase(0, last_ops.size() - 4000);
  }
};

// ---------------------------------------------------------------------------
// Synthetic randomized differential
// ---------------------------------------------------------------------------
std::string Q(const std::string &s) { return "'" + s + "'"; }

class Gen {
public:
  explicit Gen(uint64_t seed) : rng_(seed) {}
  int Int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
  bool Chance(int percent) { return Int(1, 100) <= percent; }
  template <typename T> const T &Pick(const std::vector<T> &v) { return v[Int(0, (int)v.size() - 1)]; }

  std::string Adsh() { return Q(Pick(adshs_)); }
  std::string AdshRaw() { return Pick(adshs_); }
  std::string Tag() { return Q(Pick(tags_)); }
  std::string Version() { return Q(Pick(versions_)); }
  std::string Uom() {
    static const std::vector<std::string> u = {"'USD'", "'USD'", "'USD'", "'USD'", "'usd'",
                                               "'USD '", "'EUR'", "NULL", "'shares'", "'pure'", "'CAD'", "'CNY'"};
    return Pick(u);
  }
  std::string Stmt() {
    static const std::vector<std::string> s = {"'IS'", "'IS'", "'IS'", "'BS'", "'is'", "'CF'", "NULL", "'EQ'", "'CI'"};
    return Pick(s);
  }
  std::string Fy() {
    static const std::vector<std::string> f = {"2023", "2023", "2023", "2022", "2024", "NULL", "2021", "2019", "0"};
    return Pick(f);
  }
  std::string Name() {
    static const std::vector<std::string> n = {"'Alpha'", "'Alpha'", "'Beta'", "'Gamma'",
                                               "''", "NULL", "'alpha'", "'Zeta'"};
    return Pick(n);
  }
  std::string Plabel() {
    static const std::vector<std::string> p = {"'Revenue'", "'Revenue'", "'Cost'", "'Net income'",
                                               "''", "NULL", "'revenue'"};
    return Pick(p);
  }
  std::string Value() {
    if (Chance(8)) return "NULL";
    // Multiples of 0.5: every partial sum is exactly representable.
    std::ostringstream o;
    o << (Int(-2000, 2000) * 0.5);
    if (Chance(10)) return std::to_string(1000000 * Int(1, 5)); // exact ties / large
    return o.str();
  }
  std::string FreshAdsh() { return "NEW-" + std::to_string(++fresh_); }

  std::vector<std::string> adshs_, tags_, versions_;

private:
  std::mt19937_64 rng_;
  int fresh_ = 0;
};

std::string Pred(Gen &g, int table) {
  // A random row-set predicate on the table.
  switch (g.Int(0, 3)) {
  case 0: return "adsh = " + g.Adsh();
  case 1: return table == 2 ? "name IS NOT DISTINCT FROM " + g.Name() : "adsh = " + g.Adsh() + " AND tag = " + g.Tag();
  case 2: return "ctid IN (SELECT ctid FROM " + std::string(table == 0 ? "num" : table == 1 ? "pre" : "sub") +
                 " ORDER BY random() LIMIT " + std::to_string(g.Int(1, 6)) + ")";
  default: return "random() < 0.08";
  }
}

std::string RandomOp(Gen &g) {
  switch (g.Int(0, 17)) {
  // ---- num ----
  case 0: case 1: case 2: {
    std::string sql = "INSERT INTO num (adsh, tag, version, uom, value) VALUES ";
    const int n = g.Int(1, 5);
    for (int i = 0; i < n; ++i)
      sql += std::string(i ? "," : "") + "(" + g.Adsh() + "," + g.Tag() + "," + g.Version() + "," + g.Uom() + "," + g.Value() + ")";
    return sql;
  }
  case 3: return "DELETE FROM num WHERE " + Pred(g, 0);
  case 4: return "UPDATE num SET value = " + g.Value() + " WHERE " + Pred(g, 0);
  case 5: return "UPDATE num SET tag = " + g.Tag() + ", version = " + g.Version() + " WHERE " + Pred(g, 0);
  case 6: return "UPDATE num SET uom = " + g.Uom() + " WHERE " + Pred(g, 0);
  case 7: return "UPDATE num SET adsh = " + g.Adsh() + " WHERE " + Pred(g, 0);
  // ---- pre ----
  case 8: {
    std::string sql = "INSERT INTO pre (adsh, report, line, stmt, tag, version, plabel) VALUES ";
    const int n = g.Int(1, 4);
    for (int i = 0; i < n; ++i)
      sql += std::string(i ? "," : "") + "(" + g.Adsh() + ",1,1," + g.Stmt() + "," + g.Tag() + "," + g.Version() + "," + g.Plabel() + ")";
    return sql;
  }
  case 9: return "DELETE FROM pre WHERE " + Pred(g, 1);
  case 10: return "UPDATE pre SET stmt = " + g.Stmt() + " WHERE " + Pred(g, 1);
  case 11: return "UPDATE pre SET plabel = " + g.Plabel() + " WHERE " + Pred(g, 1);
  case 12: return g.Chance(50) ? "UPDATE pre SET tag = " + g.Tag() + ", version = " + g.Version() + " WHERE " + Pred(g, 1)
                               : "UPDATE pre SET adsh = " + g.Adsh() + " WHERE " + Pred(g, 1);
  // ---- sub ----
  case 13: return "INSERT INTO sub (adsh, name, fy) VALUES (" + (g.Chance(50) ? Q(g.FreshAdsh()) : g.Adsh()) + "," +
                  g.Name() + "," + g.Fy() + ") ON CONFLICT DO NOTHING";
  case 14: return "UPDATE sub SET fy = " + g.Fy() + " WHERE " + Pred(g, 2);
  case 15: return "UPDATE sub SET name = " + g.Name() + " WHERE " + Pred(g, 2);
  case 16: return g.Chance(60) ? "DELETE FROM sub WHERE " + Pred(g, 2)
                               : "UPDATE sub SET adsh = " + Q(g.FreshAdsh()) + " WHERE adsh = " + g.Adsh();
  default: return g.Chance(70) ? "UPDATE sub SET fy = 2023 WHERE fy IS DISTINCT FROM 2023 AND random() < 0.3"
                               : "UPDATE sub SET fy = 2022 WHERE fy = 2023 AND random() < 0.3";
  }
}

void ReadFileOrFail(const std::string &path, std::string &out) {
  std::ifstream in(path);
  if (!in) Fail("cannot read " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  out = ss.str();
}

void SetupSynthetic() {
  const char *trig = std::getenv("ACCEL_Q6_TRIGGER_SQL");
  if (!trig) Fail("set ACCEL_Q6_TRIGGER_SQL=<repo>/scripts/postgres_accel_mutation_triggers.sql");
  std::string trigger_sql;
  ReadFileOrFail(trig, trigger_sql);
  {
    Conn admin("postgres");
    admin.Try("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='q6_synth'");
    admin.Exec("DROP DATABASE IF EXISTS q6_synth WITH (FORCE)");
    admin.Exec("CREATE DATABASE q6_synth TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C'");
  }
  Conn c("q6_synth");
  c.Exec("SET client_min_messages = error");
  c.Exec("CREATE EXTENSION accelerator_codegen_postgres");
  c.Exec(trigger_sql);
  // Full-width tables: capture projects physical attribute numbers.
  c.Exec("CREATE TABLE sub (adsh VARCHAR(20) PRIMARY KEY, cik INTEGER, name VARCHAR(150), sic INTEGER, "
         "countryba VARCHAR(2), stprba VARCHAR(2), cityba VARCHAR(30), countryinc VARCHAR(3), form VARCHAR(10), "
         "period INTEGER, fy INTEGER, fp VARCHAR(2), filed INTEGER, accepted VARCHAR(24), prevrpt INTEGER, "
         "nciks INTEGER, afs VARCHAR(5), wksi INTEGER, fye VARCHAR(4), instance VARCHAR(64))");
  c.Exec("CREATE TABLE num (adsh VARCHAR(20), tag VARCHAR(256), version VARCHAR(20), ddate INTEGER, qtrs INTEGER, "
         "uom VARCHAR(20), coreg VARCHAR(256), value DOUBLE PRECISION, footnote VARCHAR(1024))");
  c.Exec("CREATE TABLE pre (adsh VARCHAR(20), report INTEGER, line INTEGER, stmt VARCHAR(2), inpth INTEGER, "
         "rfile VARCHAR(1), tag VARCHAR(256), version VARCHAR(20), plabel VARCHAR(1024), negating INTEGER)");
  c.Exec("CREATE INDEX num_adsh_idx ON num (adsh)");
  c.Exec("CREATE INDEX pre_adsh_idx ON pre (adsh)");
  c.Exec("SELECT create_accel_mutation_triggers('sub', 'accel_codegen_sec_edgar')");
  c.Exec("SELECT create_accel_mutation_triggers('num', 'accel_codegen_sec_edgar')");
  c.Exec("SELECT create_accel_mutation_triggers('pre', 'accel_codegen_sec_edgar')");
}

int RunSynthetic(uint64_t seed, int steps) {
  std::cerr << "synthetic seed=" << seed << " steps=" << steps << "\n";
  SetupSynthetic();
  Gen g(seed);
  for (int i = 0; i < 28; ++i) g.adshs_.push_back("A" + std::to_string(i));
  g.adshs_.push_back("ORPHAN");
  for (int i = 0; i < 9; ++i) g.tags_.push_back("T" + std::to_string(i));
  g.versions_ = {"v1", "v2"};

  Conn a("q6_synth"), b("q6_synth");
  Checker chk;
  // Initial population.
  for (int i = 0; i < 24; ++i)
    a.Exec("INSERT INTO sub (adsh, name, fy) VALUES (" + Q(g.adshs_[i]) + "," + g.Name() + "," + g.Fy() + ")");
  for (int i = 0; i < 80; ++i)
    a.Exec("INSERT INTO num (adsh,tag,version,uom,value) VALUES (" + g.Adsh() + "," + g.Tag() + "," + g.Version() + ",'USD'," + g.Value() + ")");
  for (int i = 0; i < 70; ++i)
    a.Exec("INSERT INTO pre (adsh,report,line,stmt,tag,version,plabel) VALUES (" + g.Adsh() + ",1,1," + g.Stmt() + "," + g.Tag() + "," + g.Version() + "," + g.Plabel() + ")");
  chk.Check(a, "initial (rebuild)");
  chk.Check(a, "initial (hit)");
  chk.Check(b, "initial other connection");

  std::mt19937_64 ctl(seed ^ 0x9e3779b97f4a7c15ULL);
  auto roll = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(ctl); };
  long committed = 0, rolled = 0, savepoints = 0;
  for (int step = 1; step <= steps; ++step) {
    const std::string where = "step " + std::to_string(step);
    const int kind = roll(0, 9);
    auto run = [&](Conn &c, const std::string &sql) {
      chk.Note(sql);
      std::string err;
      if (!c.Try(sql, nullptr, &err)) {
        // A statement may legitimately fail (unique violation on sub.adsh);
        // the surrounding transaction is then aborted: roll it back.
        chk.Note("  (error: " + err.substr(0, 80) + ")");
        c.Try("ROLLBACK");
        return false;
      }
      return true;
    };
    if (kind <= 3) { // autocommit statement
      run(a, RandomOp(g));
    } else if (kind <= 7) { // transaction
      chk.Note("BEGIN");
      a.Exec("BEGIN");
      bool ok = true;
      const int n = roll(1, 4);
      for (int i = 0; ok && i < n; ++i) {
        ok = run(a, RandomOp(g));
        if (ok && roll(0, 3) == 0) chk.Check(a, where + " in-txn read");
        if (ok && roll(0, 5) == 0) {
          ++savepoints;
          chk.Note("SAVEPOINT s");
          a.Exec("SAVEPOINT s");
          ok = run(a, RandomOp(g));
          if (ok) {
            chk.Note("ROLLBACK TO s");
            a.Exec("ROLLBACK TO SAVEPOINT s");
          }
        }
      }
      if (ok) {
        if (roll(0, 2) == 0) {
          chk.Note("ROLLBACK");
          a.Exec("ROLLBACK");
          ++rolled;
        } else {
          chk.Note("COMMIT");
          a.Exec("COMMIT");
          ++committed;
        }
      }
    } else if (kind == 8) { // bulk / truncate
      if (roll(0, 5) == 0) {
        const char *t = roll(0, 1) ? "num" : "pre";
        run(a, std::string("TRUNCATE ") + t);
        chk.Check(a, where + " after truncate");
        for (int i = 0; i < 60; ++i) {
          std::string sql = std::string(t) == "num"
              ? "INSERT INTO num (adsh,tag,version,uom,value) VALUES (" + g.Adsh() + "," + g.Tag() + "," + g.Version() + ",'USD'," + g.Value() + ")"
              : "INSERT INTO pre (adsh,report,line,stmt,tag,version,plabel) VALUES (" + g.Adsh() + ",1,1," + g.Stmt() + "," + g.Tag() + "," + g.Version() + "," + g.Plabel() + ")";
          a.Exec(sql);
        }
      } else {
        run(a, "INSERT INTO num (adsh,tag,version,uom,value) SELECT adsh, tag, version, uom, value FROM num "
               "WHERE random() < 0.1");
      }
    } else { // a read only step on the other connection while a txn is open on a
      chk.Note("-- open txn on a, read on b");
      a.Exec("BEGIN");
      if (run(a, RandomOp(g))) {
        chk.Check(b, where + " other connection, writer in flight");
        a.Exec("COMMIT");
        ++committed;
      }
    }
    chk.Check(a, where);
    if (step % 3 == 0) chk.Check(b, where + " (other connection)");
    if (step % 50 == 0)
      std::cerr << "  step " << step << " ok (checks=" << chk.checks << ")\n";
  }
  std::cerr << "PASS synthetic seed=" << seed << " steps=" << steps << " checks=" << chk.checks
            << " committed_txns=" << committed << " rolled_back=" << rolled
            << " savepoint_rollbacks=" << savepoints << "\n";
  std::cerr << "execute counts: " << a.Scalar("SELECT accel_get_execute_counts()") << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
// SF3 check and benchmark
// ---------------------------------------------------------------------------
struct Target {
  std::string adsh, tag, version;
};

Target PickTarget(Conn &c) {
  // A (adsh, tag, version) that contributes to the top group.
  Rows r = c.Exec(
      "WITH top AS (SELECT s.name, n.tag, p.plabel FROM num n JOIN sub s ON n.adsh = s.adsh "
      "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
      "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 AND n.value IS NOT NULL "
      "GROUP BY s.name, n.tag, p.plabel ORDER BY SUM(n.value) DESC, s.name, n.tag, p.plabel LIMIT 1) "
      "SELECT n.adsh, n.tag, n.version FROM num n JOIN sub s ON n.adsh = s.adsh "
      "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
      "JOIN top t ON s.name IS NOT DISTINCT FROM t.name AND n.tag = t.tag AND p.plabel IS NOT DISTINCT FROM t.plabel "
      "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 AND n.value IS NOT NULL ORDER BY n.adsh LIMIT 1");
  if (r.empty()) Fail("no contributing row found");
  return {*r[0][0], *r[0][1], *r[0][2]};
}

int RunSf3(const std::string &db) {
  Conn a(db), b(db);
  Checker chk;
  chk.rel_tol = 1e-9;
  std::cerr << "sf3 check on " << db << "\n";
  auto t = std::chrono::steady_clock::now();
  chk.Check(a, "pristine (rebuild)");
  std::cerr << "  first accel+base check (includes build): " << Seconds(t) << " s\n";
  chk.Check(a, "pristine (hit)");
  chk.Check(b, "pristine other connection");
  const Target tg = PickTarget(a);
  std::cerr << "  target adsh=" << tg.adsh << " tag=" << tg.tag << " version=" << tg.version << "\n";
  const std::string key = "adsh = " + Q(tg.adsh) + " AND tag = " + Q(tg.tag) + " AND version = " + Q(tg.version);
  const std::string adsh = Q(tg.adsh);
  auto step = [&](const std::string &sql, const std::string &label, bool also_b = true) {
    chk.Note(sql);
    a.Exec(sql);
    chk.Check(a, label);
    if (also_b) chk.Check(b, label + " (other connection)");
  };
  // num write: insert / update / delete, autocommit
  step("INSERT INTO num (adsh, tag, version, uom, value) VALUES (" + Q(tg.adsh) + "," + Q(tg.tag) + "," + Q(tg.version) + ",'USD', 123456789.25)", "num insert");
  step("UPDATE num SET value = value + 1000000 WHERE " + key + " AND value = 123456789.25", "num update");
  step("DELETE FROM num WHERE " + key + " AND value = 123456789.25 + 1000000", "num delete");
  // rolled back and in-transaction
  a.Exec("BEGIN");
  a.Exec("UPDATE num SET value = value * 2 WHERE " + key);
  chk.Check(a, "in-txn num update");
  chk.Check(b, "other connection during txn");
  a.Exec("ROLLBACK");
  chk.Check(a, "after rollback");
  // sub: cohort exit / entry, name change
  step("UPDATE sub SET fy = 2022 WHERE adsh = " + adsh, "sub cohort exit");
  step("UPDATE sub SET fy = 2023 WHERE adsh = " + adsh, "sub cohort entry");
  const std::string old_name = a.Scalar("SELECT coalesce(name, '<<NULL>>') FROM sub WHERE adsh = " + adsh);
  step("UPDATE sub SET name = 'Zz renamed' WHERE adsh = " + adsh, "sub rename");
  step(old_name == "<<NULL>>" ? "UPDATE sub SET name = NULL WHERE adsh = " + adsh
                              : "UPDATE sub SET name = " + Q(old_name) + " WHERE adsh = " + adsh,
       "sub rename back");
  // pre: IS <-> BS flip of the bridge row
  step("UPDATE pre SET stmt = 'BS' WHERE adsh = " + adsh + " AND tag = " + Q(tg.tag) + " AND version = " + Q(tg.version) + " AND stmt = 'IS'", "pre IS->BS");
  step("UPDATE pre SET stmt = 'IS' WHERE adsh = " + adsh + " AND tag = " + Q(tg.tag) + " AND version = " + Q(tg.version) + " AND stmt = 'BS'", "pre BS->IS");
  // multi-table transaction with a savepoint and a rollback
  a.Exec("BEGIN");
  a.Exec("UPDATE sub SET fy = 2022 WHERE adsh = " + adsh);
  a.Exec("INSERT INTO num (adsh, tag, version, uom, value) VALUES (" + Q(tg.adsh) + "," + Q(tg.tag) + "," + Q(tg.version) + ",'USD', 5.5)");
  a.Exec("SAVEPOINT s");
  a.Exec("UPDATE pre SET stmt = 'CF' WHERE adsh = " + adsh);
  chk.Check(a, "in-txn multi-table");
  a.Exec("ROLLBACK TO SAVEPOINT s");
  a.Exec("UPDATE sub SET fy = 2023 WHERE adsh = " + adsh);
  a.Exec("COMMIT");
  chk.Check(a, "multi-table commit");
  chk.Check(b, "multi-table commit (other connection)");
  step("DELETE FROM num WHERE " + key + " AND value = 5.5", "undo multi-table insert");
  // restore check: back to the pristine answer
  chk.Check(a, "final");
  chk.Check(b, "final (other connection)");
  std::cerr << "PASS sf3 checks=" << chk.checks << "\n";
  std::cerr << "execute counts: " << a.Scalar("SELECT accel_get_execute_counts()") << "\n";
  return 0;
}

int RunSweep(const std::string &db, int percent) {
  Conn a(db);
  Checker chk;
  chk.rel_tol = 1e-9;
  auto t = std::chrono::steady_clock::now();
  a.Exec(kAccelSql); // builds the state
  std::cout << "build_s=" << Seconds(t) << "\n";
  std::vector<Params> cases;
  for (const char *f : {"2021", "2022", "2023", "2024"})
    for (const char *st : {"'BS'", "'IS'", "'CF'", "'EQ'", "'CI'"})
      for (const char *u : {"'USD'", "'shares'", "'pure'", "'CAD'", "'CNY'", "'EUR'"}) {
        Params p;
        p.fy = f;
        p.stmt = st;
        p.uom = u;
        cases.push_back(p);
      }
  std::mt19937_64 rng(7);
  if (percent < 100) {
    std::shuffle(cases.begin(), cases.end(), rng);
    cases.resize(std::max<std::size_t>(1, cases.size() * percent / 100));
  }
  auto edge = [&](const char *f, const char *st, const char *u) {
    Params p;
    p.fy = f;
    p.stmt = st;
    p.uom = u;
    cases.push_back(p);
  };
  for (const char *f : {"2019", "2025", "0", "-1", "2147483648", "NULL"}) edge(f, "'IS'", "'USD'");
  edge("2023", "'ZZ'", "'USD'");
  edge("2023", "'is'", "'USD'");
  edge("2023", "NULL", "'USD'");
  edge("2023", "'IS'", "'zz'");
  edge("2023", "'IS'", "'usd'");
  edge("2023", "'IS'", "NULL");
  edge("2022", "'BS'", "'us''d'");
  std::vector<double> aux_ms, base_ms;
  long nonempty = 0;
  for (const auto &p : cases) {
    t = std::chrono::steady_clock::now();
    Rows accel = a.Exec(p.AccelSql());
    aux_ms.push_back(Seconds(t) * 1e3);
    t = std::chrono::steady_clock::now();
    Rows base = a.Exec(p.BaseSql());
    base_ms.push_back(Seconds(t) * 1e3);
    nonempty += !base.empty();
    std::string why;
    ++chk.checks;
    if (!Equal(accel, base, chk.rel_tol, &why)) {
      std::cerr << "MISMATCH params " << p.Text() << ": " << why << "\naccel:\n" << Show(accel) << "base:\n"
                << Show(base);
      return 1;
    }
  }
  std::sort(aux_ms.begin(), aux_ms.end());
  std::sort(base_ms.begin(), base_ms.end());
  std::cout << "PASS sweep checked=" << chk.checks << " nonempty=" << nonempty << " aux_p50_ms="
            << aux_ms[aux_ms.size() / 2] << " aux_max_ms=" << aux_ms.back()
            << " base_p50_ms=" << base_ms[base_ms.size() / 2] << "\n";
  std::cout << "execute counts: " << a.Scalar("SELECT accel_get_execute_counts()") << "\n";
  return 0;
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[v.size() / 2];
}

int RunBench(const std::string &db) {
  const char *reps_env = std::getenv("BENCH_REPS");
  const int reps = reps_env ? std::atoi(reps_env) : 21;
  const int base_reps = std::max(3, reps / 5);
  const int writes = 100;
  Conn a(db);
  std::cout << "bench db=" << db << " reps=" << reps << "\n";
  // Build time: the first aux call in a fresh session builds the state.
  auto t = std::chrono::steady_clock::now();
  a.Exec(kAccelSql);
  const double build_s = Seconds(t);
  std::vector<double> accel_ms, base_ms;
  for (int i = 0; i < reps; ++i) {
    t = std::chrono::steady_clock::now();
    a.Exec(kAccelSql);
    accel_ms.push_back(Seconds(t) * 1e3);
  }
  for (int i = 0; i < base_reps; ++i) {
    t = std::chrono::steady_clock::now();
    a.Exec(kBaseSql);
    base_ms.push_back(Seconds(t) * 1e3);
  }
  std::cout << "build_s=" << build_s << "\n";
  std::cout << "read_accel_p50_ms=" << Median(accel_ms) << " read_base_p50_ms=" << Median(base_ms)
            << " (base reps=" << base_reps << ")\n";

  const Target tg = PickTarget(a);
  const std::string key = "adsh = " + Q(tg.adsh) + " AND tag = " + Q(tg.tag) + " AND version = " + Q(tg.version);
  const std::string adsh = Q(tg.adsh);
  struct Op {
    const char *label;
    std::string fwd, back;
  };
  std::vector<Op> ops = {
      {"num write (insert+delete, 1 row)",
       "INSERT INTO num (adsh, tag, version, uom, value) VALUES (" + Q(tg.adsh) + "," + Q(tg.tag) + "," + Q(tg.version) + ",'USD', 7.25)",
       "DELETE FROM num WHERE " + key + " AND value = 7.25"},
      {"num write (update value)", "UPDATE num SET value = value + 1 WHERE ctid = (SELECT ctid FROM num WHERE " + key + " LIMIT 1)",
       "UPDATE num SET value = value - 1 WHERE ctid = (SELECT ctid FROM num WHERE " + key + " LIMIT 1)"},
      {"sub structural (fy 2023->2022->2023)", "UPDATE sub SET fy = 2022 WHERE adsh = " + adsh,
       "UPDATE sub SET fy = 2023 WHERE adsh = " + adsh},
      {"pre structural (IS->BS->IS)",
       "UPDATE pre SET stmt = 'BS' WHERE adsh = " + adsh + " AND tag = " + Q(tg.tag) + " AND version = " + Q(tg.version) + " AND stmt = 'IS'",
       "UPDATE pre SET stmt = 'IS' WHERE adsh = " + adsh + " AND tag = " + Q(tg.tag) + " AND version = " + Q(tg.version) + " AND stmt = 'BS'"}};
  // Plain writes: capture triggers disabled on num / sub / pre.
  const char *tables[] = {"num", "sub", "pre"};
  auto trig = [&](bool on) {
    for (const char *tb : tables)
      a.Exec(std::string("ALTER TABLE ") + tb + (on ? " ENABLE" : " DISABLE") + " TRIGGER USER");
  };
  struct Res {
    double plain, accel, plain_pair, accel_pair, first;
  };
  std::vector<Res> res(ops.size());
  for (int mode = 0; mode < 2; ++mode) { // 0 plain, 1 accelerated
    trig(mode == 1);
    if (mode == 1) a.Exec(kAccelSql); // state (re)built before measuring
    for (std::size_t o = 0; o < ops.size(); ++o) {
      std::vector<double> fwd, back;
      for (int i = 0; i < writes; ++i) {
        t = std::chrono::steady_clock::now();
        a.Exec(ops[o].fwd);
        fwd.push_back(Seconds(t) * 1e3);
        t = std::chrono::steady_clock::now();
        a.Exec(ops[o].back);
        back.push_back(Seconds(t) * 1e3);
      }
      std::vector<double> all = fwd;
      all.insert(all.end(), back.begin(), back.end());
      (mode ? res[o].accel : res[o].plain) = Median(all);
      if (mode == 1) {
        t = std::chrono::steady_clock::now();
        a.Exec(kAccelSql);
        res[o].first = Seconds(t) * 1e3; // read after the writes (state maintained)
      }
    }
  }
  trig(true);
  Conn v(db);
  std::string why;
  Rows ac = v.Exec(kAccelSql), bs = v.Exec(kBaseSql);
  if (!Equal(ac, bs, 1e-9, &why)) Fail("bench end state mismatch: " + why);
  for (std::size_t o = 0; o < ops.size(); ++o)
    std::cout << "write p50_ms: " << ops[o].label << ": plain=" << res[o].plain
              << " accel=" << res[o].accel << " overhead=" << (res[o].accel - res[o].plain)
              << " (read after: " << res[o].first << " ms)\n";
  std::cout << "execute counts: " << a.Scalar("SELECT accel_get_execute_counts()") << "\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "synthetic";
  if (mode == "synthetic")
    return RunSynthetic(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1,
                        argc > 3 ? std::atoi(argv[3]) : 300);
  if (mode == "sf3") return RunSf3(argc > 2 ? argv[2] : "sec_edgar");
  if (mode == "bench") return RunBench(argc > 2 ? argv[2] : "sec_edgar");
  if (mode == "sweep") return RunSweep(argc > 2 ? argv[2] : "sec_edgar", argc > 3 ? std::atoi(argv[3]) : 100);
  std::cerr << "usage: test_sec_edgar_q6_postgres_differential synthetic [seed] [steps] | sf3 [db] | sweep [db] [percent] | bench [db]\n";
  return 2;
}
