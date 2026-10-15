// GENERATED BACKEND - writer-side tests (S1-S4) over libpq
//
// Drives accelerator_codegen_pg_writer_test (test_writer_module.cpp: the writer
// sources against a recording shard-worker stub and two fake accelerators) with
// several sessions: dense commit epochs and the (epoch, xid) ring, the reader
// epoch under READ COMMITTED and REPEATABLE READ with writers stalled at
// commit, gate ordering, aborts after an epoch was taken, subtransactions,
// captured batch contents, TRUNCATE, TransactionWrote, the horizon, ring
// underflow and the thread tokens.
//
// Needs a server with the module installed (PGPORT, default 5432). It creates
// the database wt_test afresh (so the shared registry starts empty) and reads
// scripts/postgres_accel_mutation_triggers.sql from ACCEL_SRC (default
// /accel-src). Exit code: the number of failed checks.
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_failures{0};
std::mutex g_print;

void Check(bool ok, const std::string &what, const std::string &detail = "") {
  if (!ok) {
    std::lock_guard<std::mutex> lock(g_print);
    g_failures++;
    std::cerr << "FAIL: " << what << (detail.empty() ? "" : " (" + detail + ")") << "\n";
  }
}

template <typename T> void CheckEq(const T &actual, const T &expected, const std::string &what) {
  std::ostringstream detail;
  detail << "got " << actual << ", want " << expected;
  Check(actual == expected, what, detail.str());
}

bool Contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string Env(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && *value ? value : fallback;
}

class Conn {
public:
  explicit Conn(const std::string &database) {
    const std::string info = "host=127.0.0.1 port=" + Env("PGPORT", "5432") + " dbname=" + database +
                             " user=postgres password=postgres";
    conn_ = PQconnectdb(info.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
      std::cerr << "connect failed: " << PQerrorMessage(conn_) << "\n";
      std::exit(1);
    }
  }
  ~Conn() { PQfinish(conn_); }
  Conn(const Conn &) = delete;

  // Runs `sql`; the first cell of the first row ("" if none) or "ERROR: ..." text.
  std::string Run(const std::string &sql, bool *ok = nullptr) {
    PGresult *result = PQexec(conn_, sql.c_str());
    return Take(result, ok);
  }
  // Like Run, and a failure is a failed check.
  std::string Ok(const std::string &sql) {
    bool ok = false;
    std::string out = Run(sql, &ok);
    Check(ok, "statement failed: " + sql, out);
    return out;
  }
  long Int(const std::string &sql) {
    const std::string cell = Ok(sql);
    return cell.empty() ? -1 : std::stol(cell);
  }
  // NULL comes back as "" and -1.
  void Send(const std::string &sql) { PQsendQuery(conn_, sql.c_str()); }
  // The error of a statement sent with Send ("" if it succeeded).
  std::string Finish() {
    bool ok = true;
    std::string error;
    while (PGresult *result = PQgetResult(conn_)) {
      bool this_ok = false;
      const std::string out = Take(result, &this_ok);
      if (!this_ok) {
        ok = false;
        error = out;
      }
    }
    return ok ? "" : error;
  }

private:
  std::string Take(PGresult *result, bool *ok) {
    const ExecStatusType status = PQresultStatus(result);
    const bool good = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
    if (ok) {
      *ok = good;
    }
    std::string out;
    if (!good) {
      out = std::string("ERROR: ") + PQerrorMessage(conn_);
    } else if (status == PGRES_TUPLES_OK && PQntuples(result) > 0 && !PQgetisnull(result, 0, 0)) {
      out = PQgetvalue(result, 0, 0);
    }
    PQclear(result);
    return out;
  }

  PGconn *conn_;
};

constexpr const char *kDb = "wt_test";
Conn *g_admin = nullptr; // autocommit, never in a transaction

long Assigned(int accelerator) { return g_admin->Int("SELECT wt_assigned(" + std::to_string(accelerator) + ")"); }

// The reader epoch of the session's current snapshot; -1: older than the ring.
long Epoch(Conn &c, int accelerator) {
  return c.Int("SELECT coalesce(wt_reader_epoch(" + std::to_string(accelerator) + "), -1)");
}

// Number of sessions waiting for an advisory-class lock.
void WaitBlocked(int count) {
  for (int i = 0; i < 500; i++) {
    if (g_admin->Int("SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") == count) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Check(false, "sessions blocked on gates", "wanted " + std::to_string(count));
}

void Setup() {
  Conn postgres("postgres");
  postgres.Run(std::string("DROP DATABASE IF EXISTS ") + kDb + " WITH (FORCE)");
  postgres.Ok(std::string("CREATE DATABASE ") + kDb);
  static Conn admin(kDb);
  g_admin = &admin;

  const std::string module = "accelerator_codegen_pg_writer_test";
  std::ostringstream sql;
  auto function = [&](const std::string &name, const std::string &args, const std::string &returns) {
    sql << "CREATE FUNCTION " << name << "(" << args << ") RETURNS " << returns << " AS '" << module << "', '"
        << name << "' LANGUAGE C;\n";
  };
  function("wt_activate", "text", "int");
  function("wt_find", "text", "int");
  function("wt_id_of", "int", "text");
  function("wt_generation", "", "bigint");
  function("wt_ensure_calls", "", "int");
  function("wt_assigned", "int", "bigint");
  function("wt_reader_epoch", "int", "bigint");
  function("wt_horizon", "int", "bigint");
  function("wt_wrote", "bigint", "bool");
  function("wt_lock", "int, int", "void");
  function("wt_unlock", "int, int", "void");
  function("wt_sent", "", "text");
  function("wt_sent_reset", "", "void");
  function("wt_tokens_take", "int", "int");
  function("wt_tokens_release", "", "void");
  function("wt_threads", "", "int");
  sql << "CREATE FUNCTION accel_aux_capture_transition() RETURNS trigger AS '" << module
      << "', 'AcceleratorCodegenCaptureTransition' LANGUAGE C;\n"
      << "CREATE TABLE t1(id int, a int, b numeric(15,2), c text, d date, f float8, w numeric(38,10), g int);\n"
      << "CREATE TABLE t2(x int, y text, n numeric, bo boolean, ts timestamp);\n"
      << "CREATE TABLE t3(z int);\n"
      << "CREATE TABLE t5(kind text);\n" // one row per stress commit
      << "CREATE TABLE t7(seen bigint);\n"
      << "CREATE PROCEDURE wt_burst(n int) LANGUAGE plpgsql AS $$ BEGIN\n"
      << "  FOR i IN 1..n LOOP INSERT INTO t1(id) VALUES (0); COMMIT; END LOOP; END $$;\n";
  const std::string triggers_path = Env("ACCEL_SRC", "/accel-src") + "/scripts/postgres_accel_mutation_triggers.sql";
  std::ifstream triggers(triggers_path);
  Check(triggers.good(), "read " + triggers_path);
  sql << triggers.rdbuf();
  sql << "SELECT create_accel_mutation_triggers('t1', 'wt');\n"
      << "SELECT create_accel_mutation_triggers('t2', 'wt');\n"
      << "SELECT create_accel_mutation_triggers('t3', 'wt');\n";
  admin.Ok(sql.str());
}

// ---- registry ---------------------------------------------------------------------
void TestRegistry() {
  // A session opened before any accelerator is active picks the registry up later.
  static Conn early(kDb);
  early.Ok("INSERT INTO t1(id) VALUES (-1)");
  CheckEq(Assigned(0), 0L, "no epoch without an active accelerator");

  CheckEq(g_admin->Int("SELECT wt_find('fake_a')"), -1L, "unknown accelerator");
  const long generation = g_admin->Int("SELECT wt_generation()");
  CheckEq(g_admin->Int("SELECT wt_activate('fake_a')"), 0L, "first index");
  CheckEq(g_admin->Int("SELECT wt_activate('fake_a')"), 0L, "activation is idempotent");
  CheckEq(g_admin->Int("SELECT wt_activate('fake_b')"), 1L, "second index");
  CheckEq(g_admin->Int("SELECT wt_find('fake_b')"), 1L, "find");
  CheckEq(g_admin->Ok("SELECT wt_id_of(1)"), std::string("fake_b"), "id of index");
  CheckEq(g_admin->Int("SELECT wt_generation()") - generation, 2L, "generation counts activations");
  CheckEq(g_admin->Int("SELECT wt_ensure_calls()"), 3L, "EnsureWorker on each activation");
  CheckEq(g_admin->Int("SELECT wt_activate('ghost')"), 2L, "an accelerator this build does not know");
  CheckEq(g_admin->Int("SELECT wt_activate(repeat('x', 200))"), -1L, "id too long");

  early.Ok("SELECT wt_sent_reset()");
  early.Ok("INSERT INTO t1(id) VALUES (-2)");
  CheckEq(Assigned(0), 1L, "the early session sees the new accelerator");
  CheckEq(Assigned(1), 1L, "... and both of them");
}

// ---- dense epochs, the (epoch, xid) ring, k under stalled commits ---------------------
void TestEpochs() {
  const long base0 = Assigned(0);
  const long base1 = Assigned(1);
  Conn w(kDb);
  w.Ok("SELECT wt_sent_reset()");
  std::vector<std::unique_ptr<Conn>> snapshots; // snapshot i was taken right after commit i
  for (int i = 1; i <= 5; i++) {
    w.Ok("INSERT INTO t1(id) VALUES (" + std::to_string(i) + ")");
    snapshots.push_back(std::make_unique<Conn>(kDb));
    snapshots.back()->Ok("BEGIN ISOLATION LEVEL REPEATABLE READ");
    CheckEq(Epoch(*snapshots.back(), 0), base0 + i, "k right after commit " + std::to_string(i));
  }
  w.Ok("INSERT INTO t2(x) VALUES (1)");
  w.Ok("INSERT INTO t2(x) VALUES (2)");
  CheckEq(Assigned(0), base0 + 5, "t2 does not touch fake_a");
  CheckEq(Assigned(1), base1 + 7, "fake_b depends on t1 and t2");
  w.Ok("INSERT INTO t3 VALUES (1)");
  CheckEq(Assigned(1), base1 + 7, "an unwatched table takes no epoch");

  const std::string sent = w.Ok("SELECT wt_sent()");
  for (int i = 1; i <= 5; i++) {
    Check(Contains(sent, "commit acc=0 epoch=" + std::to_string(base0 + i) + " empty=0"), "acc 0 ships epoch " + std::to_string(base0 + i));
  }
  for (int i = 1; i <= 7; i++) {
    Check(Contains(sent, "commit acc=1 epoch=" + std::to_string(base1 + i) + " empty=0"), "acc 1 ships epoch " + std::to_string(base1 + i));
  }
  // The pairs are right: every old snapshot still places itself at its own epoch.
  for (int i = 1; i <= 5; i++) {
    CheckEq(Epoch(*snapshots[i - 1], 0), base0 + i, "snapshot " + std::to_string(i) + " keeps its k");
    snapshots[i - 1]->Ok("COMMIT");
  }

  // A writer stalled after it took its epoch (xid in progress), another one
  // queued behind its gate.
  Conn stalled(kDb), queued(kDb), rc(kDb), rr(kDb), later(kDb);
  const long n = Assigned(0);
  g_admin->Ok("SELECT wt_lock(1, 0)");
  stalled.Ok("BEGIN");
  stalled.Ok("SET LOCAL wt.block_precommit = on");
  stalled.Ok("INSERT INTO t1(id) VALUES (10)");
  stalled.Send("COMMIT");
  WaitBlocked(1);
  CheckEq(Assigned(0), n + 1, "the stalled commit has its epoch");
  CheckEq(Epoch(rc, 0), n, "READ COMMITTED reader does not see the stalled commit");
  rr.Ok("BEGIN ISOLATION LEVEL REPEATABLE READ");
  CheckEq(Epoch(rr, 0), n, "REPEATABLE READ reader, taken while it stalls");
  queued.Ok("BEGIN");
  queued.Ok("INSERT INTO t1(id) VALUES (11)");
  queued.Send("COMMIT");
  WaitBlocked(2);
  CheckEq(Assigned(0), n + 1, "the queued commit waits for the gate: no epoch yet");
  CheckEq(Epoch(rc, 0), n, "still n");
  g_admin->Ok("SELECT wt_unlock(1, 0)");
  CheckEq(stalled.Finish(), std::string(), "stalled commit finishes");
  CheckEq(queued.Finish(), std::string(), "queued commit finishes");
  CheckEq(Assigned(0), n + 2, "queued took the next epoch");
  CheckEq(Epoch(rc, 0), n + 2, "READ COMMITTED reader sees both");
  CheckEq(Epoch(rr, 0), n, "the earlier REPEATABLE READ snapshot stays at n");
  CheckEq(Epoch(later, 0), n + 2, "a new snapshot sees n+2");
  rr.Ok("COMMIT");

  // Gates are taken in index order: nobody deadlocks over two accelerators.
  Conn both(kDb), only_b(kDb);
  g_admin->Ok("SELECT wt_lock(0, 1)");
  both.Ok("BEGIN");
  both.Ok("INSERT INTO t1(id) VALUES (12)");
  both.Send("COMMIT"); // holds gate 0, waits for gate 1
  only_b.Ok("BEGIN");
  only_b.Ok("INSERT INTO t2(x) VALUES (12)");
  only_b.Send("COMMIT");
  WaitBlocked(2);
  g_admin->Ok("SELECT wt_unlock(0, 1)");
  CheckEq(both.Finish(), std::string(), "two-accelerator commit finishes");
  CheckEq(only_b.Finish(), std::string(), "single-accelerator commit finishes");
}

// ---- readers and writers racing --------------------------------------------------------
// Every stress transaction inserts one row in t5 and touches fake_a (kind A),
// fake_b only (B) or both (C). A snapshot that sees k_0 commits of fake_a sees
// exactly k_0 rows of t5 with kind <> 'B' (dense epochs, prefix visibility),
// and k_1 rows in all.
void TestStress() {
  Conn probe(kDb);
  const long base_a = probe.Int("SELECT count(*) FROM t5 WHERE kind <> 'B'");
  const long base_all = probe.Int("SELECT count(*) FROM t5");
  const long k_a = Assigned(0);
  const long k_b = Assigned(1);
  constexpr int kWriters = 4, kPerWriter = 150;
  std::atomic<bool> done{false};
  std::atomic<long> checked{0};

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; w++) {
    threads.emplace_back([&, w] {
      Conn c(kDb);
      for (int i = 0; i < kPerWriter; i++) {
        const char kind = "ABC"[(w + i) % 3];
        std::string sql = "BEGIN; INSERT INTO t5 VALUES ('" + std::string(1, kind) + "');";
        if (kind != 'B') {
          sql += "INSERT INTO t1(id) VALUES (" + std::to_string(w * 1000 + i) + ");";
        }
        if (kind != 'A') {
          sql += "INSERT INTO t2(x) VALUES (" + std::to_string(w * 1000 + i) + ");";
        }
        c.Ok(sql + "COMMIT");
      }
    });
  }
  const char *kQuery =
      "SELECT (SELECT count(*) FROM t5 WHERE kind <> 'B') || ',' || (SELECT count(*) FROM t5) || ',' || "
      "coalesce(wt_reader_epoch(0), -1) || ',' || coalesce(wt_reader_epoch(1), -1)";
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; r++) {
    readers.emplace_back([&, r] {
      Conn c(kDb);
      while (!done) {
        const bool repeatable = r == 2;
        if (repeatable) {
          c.Ok("BEGIN ISOLATION LEVEL REPEATABLE READ");
        }
        std::string cells = c.Ok(kQuery);
        long a, all, ka, kb;
        char comma;
        std::istringstream in(cells);
        in >> a >> comma >> all >> comma >> ka >> comma >> kb;
        Check(a - base_a == ka - k_a, "k_0 matches the visible commits", cells);
        Check(all - base_all == kb - k_b, "k_1 matches the visible commits", cells);
        if (repeatable) { // the same snapshot answers the same way later
          Check(c.Ok(kQuery) == cells, "REPEATABLE READ snapshot is stable", cells);
          c.Ok("COMMIT");
        }
        checked++;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  done = true;
  for (auto &t : readers) {
    t.join();
  }
  Check(checked > 50, "readers ran during the writes", std::to_string(checked.load()));
  std::cout << "stress: " << kWriters * kPerWriter << " commits, " << checked << " reader snapshots checked\n";
  const long a_commits = [&] {
    long total = 0;
    for (int w = 0; w < kWriters; w++) {
      for (int i = 0; i < kPerWriter; i++) {
        total += "ABC"[(w + i) % 3] != 'B';
      }
    }
    return total;
  }();
  CheckEq(Assigned(0) - k_a, a_commits, "fake_a epochs are dense");
  CheckEq(Assigned(1) - k_b, static_cast<long>(kWriters * kPerWriter), "fake_b epochs are dense");
}

// ---- aborts, subtransactions, TransactionWrote -----------------------------------------------
void TestAborts() {
  Conn c(kDb);
  const long n0 = Assigned(0), n1 = Assigned(1);

  c.Ok("SELECT wt_sent_reset()");
  c.Ok("BEGIN");
  c.Ok("INSERT INTO t1(id) VALUES (20)");
  c.Ok("ROLLBACK");
  CheckEq(Assigned(0), n0, "a rollback before commit takes no epoch");
  CheckEq(c.Ok("SELECT wt_sent()"), std::string(), "... and ships nothing");

  Conn reader(kDb);
  c.Ok("BEGIN");
  c.Ok("SET LOCAL wt.fail_precommit = on");
  c.Ok("INSERT INTO t1(id) VALUES (21)");
  c.Ok("INSERT INTO t2(x) VALUES (21)");
  Check(Contains(c.Run("COMMIT"), "wt.fail_precommit"), "commit fails after the epochs were taken");
  CheckEq(Assigned(0), n0 + 1, "the aborted transaction consumed an epoch (fake_a)");
  CheckEq(Assigned(1), n1 + 1, "... and one of fake_b");
  const std::string sent = c.Ok("SELECT wt_sent()");
  Check(Contains(sent, "commit acc=0 epoch=" + std::to_string(n0 + 1) + " empty=1 invalidates=0 batches=0"), "empty commit for fake_a", sent);
  Check(Contains(sent, "commit acc=1 epoch=" + std::to_string(n1 + 1) + " empty=1 invalidates=0 batches=0"), "empty commit for fake_b", sent);
  CheckEq(Epoch(reader, 0), n0, "the aborted epoch is not a reader's k");
  c.Ok("INSERT INTO t1(id) VALUES (22)");
  CheckEq(Epoch(reader, 0), n0 + 2, "the next commit's epoch is k, the aborted one counted as empty");

  // The gates were released: another commit goes through at once.
  Conn other(kDb);
  other.Ok("INSERT INTO t1(id) VALUES (23)");
  CheckEq(Assigned(0), n0 + 3, "no gate was left behind");
}

void TestSubtransactions() {
  Conn c(kDb);
  c.Ok("SELECT wt_sent_reset()");
  c.Ok("BEGIN");
  CheckEq(c.Ok("SELECT wt_wrote(1)"), std::string("f"), "nothing written yet");
  c.Ok("INSERT INTO t1(id) VALUES (101)");
  CheckEq(c.Ok("SELECT wt_wrote(1)"), std::string("t"), "t1 written");
  c.Ok("SAVEPOINT s");
  c.Ok("INSERT INTO t1(id) VALUES (102)");
  c.Ok("INSERT INTO t2(x) VALUES (102)");
  CheckEq(c.Ok("SELECT wt_wrote(2)"), std::string("t"), "t2 written in the savepoint");
  c.Ok("ROLLBACK TO s");
  CheckEq(c.Ok("SELECT wt_wrote(2)"), std::string("f"), "a rolled-back write does not count");
  c.Ok("SAVEPOINT s2");
  c.Ok("INSERT INTO t1(id) VALUES (103)");
  c.Ok("SAVEPOINT s3");
  c.Ok("INSERT INTO t1(id) VALUES (104)");
  c.Ok("RELEASE s3");
  c.Ok("RELEASE s2");
  c.Ok("INSERT INTO t3 VALUES (5)");
  CheckEq(c.Ok("SELECT wt_wrote(3)"), std::string("t"), "a table without an active accelerator counts too");
  c.Ok("COMMIT");
  CheckEq(c.Ok("SELECT wt_wrote(1)"), std::string("f"), "nothing is written after the commit");
  const std::string sent = c.Ok("SELECT wt_sent()");
  Check(Contains(sent, "batches=3"), "three batches shipped", sent);
  const auto a = sent.find("new 0=[i101]"), b = sent.find("new 0=[i103]"), d = sent.find("new 0=[i104]");
  Check(a != std::string::npos && b != std::string::npos && d != std::string::npos && a < b && b < d, "batches 101, 103, 104 in statement order", sent);
  Check(!Contains(sent, "i102"), "the rolled-back batch is gone", sent);
  Check(!Contains(sent, "table=t2"), "so is the rolled-back write to t2", sent);

  // Rolling back the whole transaction after a released savepoint.
  c.Ok("SELECT wt_sent_reset()");
  const long n = Assigned(0);
  c.Ok("BEGIN");
  c.Ok("SAVEPOINT s");
  c.Ok("INSERT INTO t1(id) VALUES (105)");
  c.Ok("RELEASE s");
  c.Ok("ROLLBACK");
  CheckEq(Assigned(0), n, "no epoch after ROLLBACK");
  CheckEq(c.Ok("SELECT wt_sent()"), std::string(), "nothing shipped after ROLLBACK");

  // PREPARE is refused (the commit would not be shipped at the right time).
  c.Ok("BEGIN");
  c.Ok("INSERT INTO t1(id) VALUES (106)");
  Check(Contains(c.Run("PREPARE TRANSACTION 'wt1'"), "cannot PREPARE"), "prepared transactions on tracked tables are refused");
}

// ---- captured batches ------------------------------------------------------------------------
void TestCapture() {
  Conn c(kDb);
  auto sent = [&](const std::string &statement) {
    c.Ok("SELECT wt_sent_reset()");
    c.Ok(statement);
    return c.Ok("SELECT wt_sent()");
  };
  c.Ok("TRUNCATE t1"); // known contents from here on
  const std::string insert = sent(
      "INSERT INTO t1 VALUES (1, 10, 12.50, 'hello', '2020-01-02', 1.5, 1234567890123456789012.0123456789, 7),"
      " (2, NULL, 0.05, NULL, NULL, NULL, NULL, 8)");
  // The union of fake_a (0-3) and fake_b (2-6): column 7 is not captured.
  Check(Contains(insert, "batch table=t1 kind=insert rows=2 updated=[]"), "insert batch", insert);
  Check(Contains(insert, "new 0=[i1,i2] 1=[i10,NULL] 2=[d12.50,d0.05] 3=[shello,NULL] 4=[D18263,NULL] 5=[f1.5,NULL] "
                         "6=[w1234567890123456789012.0123456789,NULL]"),
        "insert columns and types", insert);
  Check(Contains(insert, "commit acc=0") && Contains(insert, "commit acc=1"), "both accelerators get it", insert);

  const std::string update = sent("UPDATE t1 SET a = a + 1 WHERE id = 1");
  Check(Contains(update, "kind=update rows=1 updated=[1]"), "update batch", update);
  Check(Contains(update, "old 0=[i1] 1=[i10]") && Contains(update, "new 0=[i1] 1=[i11]"), "old and new rows", update);

  const std::string unwatched = sent("UPDATE t1 SET g = 9");
  Check(Contains(unwatched, "rows=2 updated=[-1]"), "an unwatched column is 'outside the projection'", unwatched);
  const std::string mixed = sent("UPDATE t1 SET a = a + 1, g = 0 WHERE id = 1");
  Check(Contains(mixed, "updated=[1,-1]"), "watched and unwatched columns", mixed);

  const long n = Assigned(0);
  CheckEq(sent("UPDATE t1 SET a = 1 WHERE false"), std::string(), "a statement that changed nothing ships nothing");
  CheckEq(Assigned(0), n, "... and takes no epoch");

  const std::string del = sent("DELETE FROM t1 WHERE id = 2");
  Check(Contains(del, "kind=delete rows=1") && Contains(del, "old 0=[i2] 1=[NULL] 2=[d0.05]"), "delete batch", del);
  Check(!Contains(del, "new "), "a delete has no new rows", del);

  // numeric without a typmod: one scale per column, as precise as its most precise value.
  const std::string t2 = sent("INSERT INTO t2 VALUES (1, 'a', 1.5, true, '2020-01-02 03:04:05'), (2, 'b', 2.25, false, NULL),"
                              " (3, 'c', NULL, NULL, NULL), (4, 'd', 100, true, '1999-12-31 00:00:00')");
  Check(Contains(t2, "commit acc=1") && !Contains(t2, "commit acc=0"), "t2 only reaches fake_b", t2);
  Check(Contains(t2, "new 0=[i1,i2,i3,i4] 1=[sa,sb,sc,sd] 2=[d1.50,d2.25,NULL,d100.00] 3=[b1,b0,NULL,b1] "
                     "4=[s2020-01-02 03:04:05,NULL,NULL,s1999-12-31 00:00:00]"),
        "whole-row capture, mixed numeric scales, bool and timestamp", t2);

  // A numeric the accelerators cannot hold fails the statement (and ships nothing).
  c.Ok("SELECT wt_sent_reset()");
  const long m = Assigned(1);
  Check(Contains(c.Run("INSERT INTO t2(x, n) VALUES (9, 1e40)"), "numeric"), "numeric beyond 38 digits is refused");
  Check(Contains(c.Run("INSERT INTO t2(x, n) VALUES (9, 'NaN')"), "numeric"), "NaN is refused");
  CheckEq(Assigned(1), m, "... without an epoch");

  // TRUNCATE ships `invalidates` to the accelerators on that table.
  const std::string truncate_t2 = sent("TRUNCATE t2");
  Check(Contains(truncate_t2, "commit acc=1 epoch=") && Contains(truncate_t2, "invalidates=1 batches=0"), "TRUNCATE t2 invalidates fake_b", truncate_t2);
  Check(!Contains(truncate_t2, "commit acc=0"), "... but not fake_a", truncate_t2);
  const std::string truncate_t1 = sent("TRUNCATE t1");
  Check(Contains(truncate_t1, "commit acc=0") && Contains(truncate_t1, "commit acc=1"), "TRUNCATE t1 reaches both", truncate_t1);
  c.Ok("SELECT wt_sent_reset()");
  c.Ok("BEGIN");
  c.Ok("SAVEPOINT s");
  c.Ok("TRUNCATE t1");
  c.Ok("ROLLBACK TO s");
  c.Ok("INSERT INTO t1(id) VALUES (50)");
  c.Ok("COMMIT");
  Check(!Contains(c.Ok("SELECT wt_sent()"), "invalidates=1"), "a rolled-back TRUNCATE invalidates nothing");

  // Transition tables are shared with other triggers: both read them in full.
  c.Ok("CREATE FUNCTION wt_probe() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
       "INSERT INTO t7 SELECT count(*) FROM new_rows; RETURN NULL; END $$");
  c.Ok("CREATE TRIGGER a_probe AFTER INSERT ON t1 REFERENCING NEW TABLE AS new_rows FOR EACH STATEMENT EXECUTE FUNCTION wt_probe()");
  c.Ok("CREATE TRIGGER z_probe AFTER INSERT ON t1 REFERENCING NEW TABLE AS new_rows FOR EACH STATEMENT EXECUTE FUNCTION wt_probe()");
  const std::string shared = sent("INSERT INTO t1(id) SELECT generate_series(1, 3)");
  Check(Contains(shared, "rows=3") && Contains(shared, "new 0=[i1,i2,i3]"), "capture between two other readers", shared);
  CheckEq(c.Ok("SELECT string_agg(seen::text, ',') FROM t7"), std::string("3,3"), "both probes saw all rows");
  c.Ok("DROP TRIGGER a_probe ON t1");
  c.Ok("DROP TRIGGER z_probe ON t1");
}

// ---- horizon and ring underflow ----------------------------------------------------------------
void TestHorizonAndRing() {
  Conn c(kDb), old_reader(kDb), probe(kDb);
  c.Ok("INSERT INTO t1(id) VALUES (60)");
  CheckEq(probe.Int("SELECT wt_horizon(0)"), Assigned(0), "no open snapshots: every commit is below the horizon");

  old_reader.Ok("BEGIN ISOLATION LEVEL REPEATABLE READ");
  const long k = Epoch(old_reader, 0);
  c.Ok("INSERT INTO t1(id) VALUES (61)");
  c.Ok("INSERT INTO t1(id) VALUES (62)");
  CheckEq(probe.Int("SELECT wt_horizon(0)"), k, "an open snapshot holds the horizon at its k");
  CheckEq(Epoch(old_reader, 0), k, "... and still reads k");
  old_reader.Ok("COMMIT");
  CheckEq(probe.Int("SELECT wt_horizon(0)"), Assigned(0), "released: the horizon follows");

  // A snapshot before any commit reads 0 (not underflow) while the ring is young.
  // The registry is fresh for fake_a only if no commit happened, so use a ghost.
  CheckEq(Epoch(probe, 2), 0L, "no commits: epoch 0");

  // Within the ring: served. Beyond it: SQL fallback.
  Conn within(kDb), beyond(kDb);
  within.Ok("BEGIN ISOLATION LEVEL REPEATABLE READ");
  const long k_within = Epoch(within, 0);
  c.Ok("CALL wt_burst(1000)");
  CheckEq(Epoch(within, 0), k_within, "a snapshot 1000 commits back is still in the ring");
  beyond.Ok("BEGIN ISOLATION LEVEL REPEATABLE READ");
  const long k_beyond = Epoch(beyond, 0);
  c.Ok("CALL wt_burst(1100)");
  CheckEq(Epoch(beyond, 0), -1L, "a snapshot 1100 commits back is older than the ring");
  CheckEq(Epoch(within, 0), -1L, "... as is the first one by now");
  CheckEq(Epoch(probe, 0), Assigned(0), "a fresh snapshot is at the newest epoch");
  CheckEq(probe.Int("SELECT wt_horizon(0)"), Assigned(0) - 1023, "the horizon is at the oldest epoch the ring remembers");
  Check(k_beyond < Assigned(0) - 1023, "(the old snapshot was out of range)");
  beyond.Ok("COMMIT");
  within.Ok("COMMIT");
  CheckEq(probe.Int("SELECT wt_horizon(0)"), Assigned(0), "released again");
}

// ---- thread tokens --------------------------------------------------------------------------------
void TestTokens() {
  g_admin->Ok("ALTER SYSTEM SET accel.threads = 4");
  g_admin->Ok("SELECT pg_reload_conf()");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  Conn a(kDb), b(kDb);
  CheckEq(a.Int("SELECT wt_threads()"), 4L, "accel.threads");
  CheckEq(a.Int("SELECT wt_tokens_take(3)"), 3L, "three tokens");
  CheckEq(b.Int("SELECT wt_tokens_take(3)"), 1L, "the pool is shared across sessions: one left");
  CheckEq(a.Int("SELECT wt_tokens_take(1)"), 0L, "none left");
  a.Ok("SELECT wt_tokens_release()");
  CheckEq(b.Int("SELECT wt_tokens_take(3)"), 3L, "released tokens are available again");
  b.Ok("SELECT wt_tokens_release()");
  g_admin->Ok("ALTER SYSTEM RESET accel.threads");
  g_admin->Ok("SELECT pg_reload_conf()");
}

} // namespace

int main() {
  Setup();
  TestRegistry();
  TestEpochs();
  TestStress();
  TestAborts();
  TestSubtransactions();
  TestCapture();
  TestHorizonAndRing();
  TestTokens();
  std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILED") << " (" << g_failures << " failed checks)\n";
  return g_failures;
}
