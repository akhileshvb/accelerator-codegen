// PostgreSQL differential for the SEC EDGAR Q1 accelerator
// (accel_sec_edgar_q1_aux()) against plain SQL (workloads/sec_edgar/q1.sql).
//
// Modes (argv[1]):
//   random  [seed] [steps]  scratch database with a small random `pre`: randomized
//                           INSERT / DELETE / UPDATE (q1 columns and others), multi-
//                           statement transactions, savepoints, rollbacks, TRUNCATE;
//                           accelerator vs SQL after every statement (inside the
//                           transaction: fallback path) and after every commit/
//                           rollback on a second connection (incremental maintenance
//                           + cross-backend sync).
//   concurrent [seconds]    same scratch database: writer threads commit random
//                           writes while reader threads compare accelerator and SQL
//                           inside REPEATABLE READ transactions; plus snapshot-
//                           isolation scenarios (uncommitted / later-committed writes
//                           are invisible to an older snapshot).
//   sf3                     the full data in `sec_edgar`: read match, then net-zero
//                           write cycles (update, commit, compare, revert, compare).
// Env: Q1_PG_CONN (default: host=127.0.0.1 port=55401 user=postgres password=postgres
//      dbname=sec_edgar), ACCEL_SRC (repo root, for the trigger script).
#include <libpq-fe.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Rows = std::vector<std::vector<std::string>>;

std::string BaseConn() {
  const char *c = std::getenv("Q1_PG_CONN");
  return c ? c : "host=127.0.0.1 port=55401 user=postgres password=postgres dbname=sec_edgar";
}
std::string ConnWithDb(const std::string &db) {
  std::string c = BaseConn();
  const auto pos = c.find("dbname=");
  if (pos == std::string::npos) return c + " dbname=" + db;
  auto end = c.find(' ', pos);
  return c.substr(0, pos) + "dbname=" + db + (end == std::string::npos ? "" : c.substr(end));
}

struct Conn {
  PGconn *c = nullptr;
  explicit Conn(const std::string &info) {
    c = PQconnectdb(info.c_str());
    if (PQstatus(c) != CONNECTION_OK) throw std::runtime_error(std::string("connect: ") + PQerrorMessage(c));
  }
  ~Conn() { PQfinish(c); }
  Conn(const Conn &) = delete;
  Rows Query(const std::string &sql) {
    PGresult *r = PQexec(c, sql.c_str());
    const auto st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
      std::string msg = PQresultErrorMessage(r);
      PQclear(r);
      throw std::runtime_error("SQL failed: " + sql.substr(0, 200) + "\n" + msg);
    }
    Rows rows;
    for (int i = 0; i < PQntuples(r); ++i) {
      std::vector<std::string> row;
      for (int j = 0; j < PQnfields(r); ++j) row.push_back(PQgetisnull(r, i, j) ? "\\N" : PQgetvalue(r, i, j));
      rows.push_back(std::move(row));
    }
    PQclear(r);
    return rows;
  }
  // Like Query but returns false (and swallows the error) on failure.
  bool TryExec(const std::string &sql) {
    PGresult *r = PQexec(c, sql.c_str());
    const bool ok = PQresultStatus(r) == PGRES_COMMAND_OK || PQresultStatus(r) == PGRES_TUPLES_OK;
    PQclear(r);
    return ok;
  }
};

[[noreturn]] void Fail(const std::string &m) { throw std::runtime_error(m); }

// Reference (workloads/sec_edgar/q1.sql) and accelerator, both in a
// total order (cnt DESC, stmt, rfile) so the unspecified order of ties does not matter.
const char *kRefSql =
    "SELECT stmt, rfile, cnt, num_filings, avg_line_num FROM ("
    "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, AVG(line) AS avg_line_num "
    "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile) q "
    "ORDER BY cnt DESC, stmt COLLATE \"C\", rfile COLLATE \"C\" NULLS FIRST";
const char *kAccelSql =
    "SELECT stmt, rfile, cnt, num_filings, avg_line_num FROM accel_sec_edgar_q1_aux() "
    "ORDER BY cnt DESC, stmt COLLATE \"C\", rfile COLLATE \"C\" NULLS FIRST";

std::string Dump(const Rows &rows) {
  std::string s;
  for (const auto &r : rows) {
    for (std::size_t j = 0; j < r.size(); ++j) s += (j ? "|" : "") + r[j];
    s += "\n";
  }
  return s;
}

void CheckSame(Conn &c, const std::string &label) {
  const Rows ref = c.Query(kRefSql);
  const Rows acc = c.Query(kAccelSql);
  if (ref != acc) Fail(label + ": accelerator differs from SQL\nSQL:\n" + Dump(ref) + "accelerator:\n" + Dump(acc));
  // The accelerator's own output order: cnt non-increasing (ORDER BY cnt DESC).
  const Rows raw = c.Query("SELECT cnt FROM accel_sec_edgar_q1_aux()");
  long long last = -1;
  bool first = true;
  for (const auto &r : raw) {
    const long long v = std::stoll(r[0]);
    if (!first && v > last) Fail(label + ": accelerator output not ordered by cnt DESC");
    last = v;
    first = false;
  }
}

// ---------------------------------------------------------------------------
// Scratch database
// ---------------------------------------------------------------------------

const char *kScratchDb = "q1_scratch";

std::string ReadFile(const std::string &path) {
  std::ifstream in(path);
  if (!in) Fail("cannot read " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void SetupScratch() {
  {
    Conn admin(ConnWithDb("postgres"));
    admin.Query(std::string("DROP DATABASE IF EXISTS ") + kScratchDb + " WITH (FORCE)");
    admin.Query(std::string("CREATE DATABASE ") + kScratchDb);
  }
  Conn c(ConnWithDb(kScratchDb));
  c.Query("CREATE EXTENSION accelerator_codegen_postgres");
  const char *src = std::getenv("ACCEL_SRC");
  const std::string root = src ? src : ".";
  c.Query(ReadFile(root + "/backends/postgres/host/capture_triggers.sql"));
  c.Query(
      "CREATE TABLE pre (adsh VARCHAR(20), report INTEGER, line INTEGER, stmt VARCHAR(2), inpth INTEGER, "
      "rfile VARCHAR(1), tag VARCHAR(256), version VARCHAR(20), plabel VARCHAR(1024), negating INTEGER)");
  c.Query("SELECT create_accel_mutation_triggers('pre', 'accel_codegen_sec_edgar')");
  // 40k rows over ~4k canonical accessions, a few non-canonical ones, NULLs.
  c.Query(
      "INSERT INTO pre SELECT "
      "CASE WHEN i % 997 = 0 THEN NULL WHEN i % 331 = 0 THEN 'odd-' || (i % 7) "
      "ELSE lpad(((i / 10) % 4000)::text, 10, '0') || '-22-' || lpad(((i / 10) % 4000 * 7 % 1000000)::text, 6, '0') END, "
      "i % 5, CASE WHEN i % 211 = 0 THEN NULL ELSE (i * 7919) % 300 - 20 END, "
      "CASE WHEN i % 389 = 0 THEN NULL ELSE (ARRAY['BS','IS','CF','EQ','CI','UN'])[1 + i % 6] END, "
      "i % 2, CASE WHEN i % 173 = 0 THEN NULL ELSE (ARRAY['H','R','X'])[1 + i % 3] END, "
      "'tag' || (i % 50), 'v' || (i % 3), 'label ' || i, i % 2 "
      "FROM generate_series(1, 40000) i");
}

struct Gen {
  std::mt19937_64 rng;
  explicit Gen(uint64_t seed) : rng(seed) {}
  uint64_t R(uint64_t n) { return rng() % n; }
  std::string Adsh() {
    switch (R(12)) {
    case 0: return "NULL";
    case 1: return "'odd-" + std::to_string(R(12)) + "'"; // non-canonical
    case 2: return "'0000000000-00-000000'";
    case 3: return "''";
    default: {
      char b[40];
      std::snprintf(b, sizeof b, "'%010llu-22-%06llu'", (unsigned long long)R(4200), (unsigned long long)(R(4200) * 7 % 1000000));
      return b;
    }
    }
  }
  std::string Stmt() {
    static const char *v[] = {"'BS'", "'IS'", "'CF'", "'EQ'", "'CI'", "'UN'", "'ZZ'", "''", "NULL"};
    return v[R(9)];
  }
  std::string Rfile() {
    static const char *v[] = {"'H'", "'R'", "'X'", "'Q'", "NULL", "''"};
    return v[R(6)];
  }
  std::string Line() { return R(15) == 0 ? std::string("NULL") : std::to_string((long long)R(400) - 30); }
  std::string Row() {
    return "(" + Adsh() + "," + std::to_string(R(5)) + "," + Line() + "," + Stmt() + ",0," + Rfile() + ",'t" +
           std::to_string(R(50)) + "','v',NULL,0)";
  }
  std::string Pred() {
    switch (R(9)) {
    case 0: return "stmt = " + Stmt();
    case 1: return "rfile = " + Rfile();
    case 2: return "adsh = " + Adsh();
    case 3: return "line < " + std::to_string((long long)R(300) - 20);
    case 4: return "stmt IS NULL";
    case 5: return "line IS NULL";
    case 6: return "report = " + std::to_string(R(5)) + " AND line > " + std::to_string(R(250));
    case 7: return "tag = 't" + std::to_string(R(50)) + "'";
    default: return "(ctid::text::point)[0] < " + std::to_string(R(120)) + " AND (ctid::text::point)[1] < " + std::to_string(R(30));
    }
  }
  std::string Statement() {
    switch (R(14)) {
    case 0:
    case 1: return "INSERT INTO pre VALUES " + Row() + "," + Row() + "," + Row();
    case 2: return "INSERT INTO pre VALUES " + Row();
    case 3: return "INSERT INTO pre SELECT * FROM pre WHERE " + Pred() + " LIMIT " + std::to_string(1 + R(80));
    case 4:
    case 5: return "DELETE FROM pre WHERE " + Pred() + " AND ctid IN (SELECT ctid FROM pre TABLESAMPLE SYSTEM (10) LIMIT 60)";
    case 6: return "DELETE FROM pre WHERE adsh = " + Adsh();
    case 7: return "UPDATE pre SET line = line + " + std::to_string((long long)R(9) - 4) + " WHERE " + Pred();
    case 8: return "UPDATE pre SET line = NULL WHERE " + Pred() + " AND ctid IN (SELECT ctid FROM pre LIMIT 30)";
    case 9: return "UPDATE pre SET stmt = " + Stmt() + " WHERE " + Pred() + " AND ctid IN (SELECT ctid FROM pre LIMIT 40)";
    case 10: return "UPDATE pre SET rfile = " + Rfile() + " WHERE " + Pred() + " AND ctid IN (SELECT ctid FROM pre LIMIT 40)";
    case 11: return "UPDATE pre SET adsh = " + Adsh() + " WHERE " + Pred() + " AND ctid IN (SELECT ctid FROM pre LIMIT 40)";
    case 12: return "UPDATE pre SET plabel = 'x' || report, tag = 'u' WHERE " + Pred(); // columns q1 does not read
    default: return "UPDATE pre SET stmt = " + Stmt() + ", rfile = " + Rfile() + ", adsh = " + Adsh() + ", line = " + Line() +
                    " WHERE " + Pred() + " AND ctid IN (SELECT ctid FROM pre LIMIT 25)";
    }
  }
};

int RunRandom(uint64_t seed, int steps) {
  SetupScratch();
  Conn w(ConnWithDb(kScratchDb)), other(ConnWithDb(kScratchDb));
  Gen g(seed);
  CheckSame(w, "initial (build)");
  CheckSame(other, "initial (second connection)");
  int commits = 0, rollbacks = 0, stmts = 0;
  for (int step = 0; step < steps; ++step) {
    w.Query("BEGIN");
    const int n = 1 + (int)g.R(4);
    bool saved = false;
    for (int i = 0; i < n; ++i) {
      if (g.R(8) == 0) {
        if (!saved) { w.Query("SAVEPOINT sp"); saved = true; }
        else { w.Query("ROLLBACK TO SAVEPOINT sp"); CheckSame(w, "step " + std::to_string(step) + " after rollback to savepoint"); continue; }
      }
      const std::string sql = g.Statement();
      w.Query(sql);
      ++stmts;
      if (g.R(3) == 0) CheckSame(w, "step " + std::to_string(step) + " in txn after: " + sql);
    }
    if (g.R(60) == 0) { w.Query("TRUNCATE pre"); CheckSame(w, "step " + std::to_string(step) + " after TRUNCATE"); }
    const bool commit = g.R(4) != 0;
    CheckSame(w, "step " + std::to_string(step) + " before end");
    w.Query(commit ? "COMMIT" : "ROLLBACK");
    (commit ? commits : rollbacks)++;
    const std::string lbl = std::string("step ") + std::to_string(step) + (commit ? " after commit" : " after rollback");
    CheckSame(w, lbl);
    if (step % 3 == 0) CheckSame(other, lbl + " (second connection)");
    if (step % 40 == 39) { // a fresh session builds from scratch
      Conn fresh(ConnWithDb(kScratchDb));
      CheckSame(fresh, lbl + " (fresh session)");
    }
  }
  std::cout << "PASS random seed=" << seed << " steps=" << steps << " statements=" << stmts << " commits=" << commits
            << " rollbacks=" << rollbacks << std::endl;
  return 0;
}

// ---------------------------------------------------------------------------
// Concurrency / isolation
// ---------------------------------------------------------------------------

void RunIsolation() {
  Conn a(ConnWithDb(kScratchDb)), b(ConnWithDb(kScratchDb));
  CheckSame(a, "isolation: start");
  a.Query("BEGIN ISOLATION LEVEL REPEATABLE READ");
  const Rows before = a.Query(kAccelSql);
  if (before != a.Query(kRefSql)) Fail("isolation: snapshot start differs");
  // Uncommitted writes of b are invisible to a (and to a's accelerator).
  b.Query("BEGIN");
  b.Query("INSERT INTO pre SELECT * FROM pre WHERE stmt IS NOT NULL LIMIT 500");
  if (a.Query(kAccelSql) != before) Fail("isolation: uncommitted write visible to accelerator");
  b.Query("COMMIT");
  // Committed after a's snapshot: still invisible to a, in both answers.
  if (a.Query(kAccelSql) != before) Fail("isolation: later commit visible in a's snapshot (accelerator)");
  if (a.Query(kRefSql) != before) Fail("isolation: later commit visible in a's snapshot (SQL)");
  a.Query("COMMIT");
  CheckSame(a, "isolation: after a's commit");
  // A reader that started before a writer's rolled back transaction never sees it.
  a.Query("BEGIN ISOLATION LEVEL REPEATABLE READ");
  const Rows snap = a.Query(kAccelSql);
  b.Query("BEGIN");
  b.Query("DELETE FROM pre WHERE stmt = 'BS'");
  b.Query("ROLLBACK");
  if (a.Query(kAccelSql) != snap) Fail("isolation: rolled back delete visible");
  a.Query("COMMIT");
  CheckSame(a, "isolation: after rolled back delete");
  CheckSame(b, "isolation: writer after rollback");
  // Own uncommitted writes are visible to the writer's own accelerator call.
  b.Query("BEGIN");
  b.Query("DELETE FROM pre WHERE stmt = 'BS'");
  CheckSame(b, "isolation: own uncommitted delete");
  b.Query("ROLLBACK");
  CheckSame(b, "isolation: after own rolled back delete");
  std::cout << "PASS isolation" << std::endl;
}

void RunConcurrent(int seconds) {
  SetupScratch();
  RunIsolation();
  std::atomic<bool> stop{false};
  std::atomic<long> reads{0}, writes{0};
  std::mutex err_mutex;
  std::string error;
  auto fail = [&](const std::string &m) {
    std::lock_guard<std::mutex> lock(err_mutex);
    if (error.empty()) error = m;
    stop = true;
  };
  std::vector<std::thread> threads;
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&, t] { // writers
      try {
        Conn c(ConnWithDb(kScratchDb));
        Gen g(1000 + t);
        // Seed the accelerator in this session so its own commits maintain the state.
        if (t == 0) c.Query(kAccelSql);
        while (!stop) {
          try {
            c.Query("BEGIN");
            for (int i = 0, n = 1 + (int)g.R(3); i < n; ++i) c.Query(g.Statement());
            if (t == 0 && g.R(3) == 0) c.Query(kAccelSql);
            c.Query(g.R(4) ? "COMMIT" : "ROLLBACK");
            ++writes;
          } catch (const std::exception &e) {
            // Writers contend on the same rows: a deadlock aborts the transaction, nothing else may fail.
            if (std::string(e.what()).find("deadlock") == std::string::npos) throw;
            c.TryExec("ROLLBACK");
          }
        }
      } catch (const std::exception &e) { fail(std::string("writer: ") + e.what()); }
    });
  }
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] { // readers: accelerator == SQL inside one snapshot
      try {
        Conn c(ConnWithDb(kScratchDb));
        while (!stop) {
          c.Query("BEGIN ISOLATION LEVEL REPEATABLE READ");
          const Rows acc = c.Query(kAccelSql);
          const Rows ref = c.Query(kRefSql);
          const Rows acc2 = c.Query(kAccelSql);
          c.Query("COMMIT");
          if (acc != ref || acc2 != ref) fail("concurrent: accelerator differs from SQL\nSQL:\n" + Dump(ref) + "accelerator:\n" + Dump(acc));
          ++reads;
        }
      } catch (const std::exception &e) { fail(std::string("reader: ") + e.what()); }
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop = true;
  for (auto &th : threads) th.join();
  if (!error.empty()) Fail(error);
  { Conn c(ConnWithDb(kScratchDb)); CheckSame(c, "concurrent: final"); }
  std::cout << "PASS concurrent reads=" << reads << " write_txns=" << writes << std::endl;
}

// ---------------------------------------------------------------------------
// Full data
// ---------------------------------------------------------------------------

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

void RunSf3() {
  Conn c(BaseConn());
  const Rows n = c.Query("SELECT count(*) FROM pre");
  std::cout << "pre rows: " << n[0][0] << std::endl;
  auto t0 = std::chrono::steady_clock::now();
  CheckSame(c, "sf3: first read (includes build)");
  auto t1 = std::chrono::steady_clock::now();
  CheckSame(c, "sf3: second read");
  auto t2 = std::chrono::steady_clock::now();
  std::cout << "first compare (build + both): " << Ms(t0, t1) << " ms; warm compare: " << Ms(t1, t2) << " ms" << std::endl;
  Conn other(BaseConn());
  CheckSame(other, "sf3: second connection");
  std::mt19937_64 rng(5);
  for (int cycle = 0; cycle < 6; ++cycle) {
    const int k = 1 + (int)(rng() % 3000);
    // Predicates on adsh only, so a change of line/stmt does not alter the row set.
    const std::string rows = "adsh IN (SELECT DISTINCT adsh FROM pre ORDER BY adsh LIMIT " + std::to_string(k) + ")";
    if (cycle % 2) {
      // Destructive changes, rolled back.
      c.Query("BEGIN");
      c.Query("UPDATE pre SET stmt = 'ZZ' WHERE " + rows);
      CheckSame(c, "sf3: in txn after stmt move");
      c.Query("INSERT INTO pre SELECT * FROM pre WHERE stmt = 'ZZ' LIMIT 1000");
      c.Query("DELETE FROM pre WHERE stmt = 'ZZ' AND report % 2 = 0");
      CheckSame(c, "sf3: in txn after insert/delete");
      c.Query("ROLLBACK");
      CheckSame(c, "sf3: after rollback");
      CheckSame(other, "sf3: second connection after rollback");
    } else {
      // Net-zero committed change: line + 7, compare, line - 7, compare.
      c.Query("BEGIN");
      c.Query("UPDATE pre SET line = line + 7 WHERE " + rows);
      CheckSame(c, "sf3: in txn after update");
      c.Query("COMMIT");
      CheckSame(c, "sf3: after committed update");
      CheckSame(other, "sf3: second connection after committed update");
      c.Query("UPDATE pre SET line = line - 7 WHERE " + rows);
      CheckSame(c, "sf3: after revert");
      CheckSame(other, "sf3: second connection after revert");
    }
  }
  std::cout << "PASS sf3" << std::endl;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::string mode = argc > 1 ? argv[1] : "random";
    if (mode == "random") return RunRandom(argc > 2 ? std::stoull(argv[2]) : 1, argc > 3 ? std::stoi(argv[3]) : 200);
    if (mode == "concurrent") { RunConcurrent(argc > 2 ? std::stoi(argv[2]) : 20); return 0; }
    if (mode == "sf3") { RunSf3(); return 0; }
    std::cerr << "usage: " << argv[0] << " random [seed] [steps] | concurrent [seconds] | sf3\n";
    return 2;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    return 1;
  }
}
