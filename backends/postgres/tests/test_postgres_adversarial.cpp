// VALIDATION-OWNED — engine <-> substrate ADVERSARIAL harness for postgres.
// Spec: pipeline/prompts/engine_backend/adversarial_tests.md
// Build: test_postgres_adversarial   CTest: engine_postgres_adversarial
//
// Unseen by implementation layer harnesses. Cases below are not copies of
// test_postgres_{store,aux_backend,txn,mutations,scan,runtime,visibility,host}.cpp.
// Plain libpq harness: LOADs the test-only module accelerator_codegen_pg_test
// and drives the test UDFs (scan / txn bridge / runtime bridge) against the
// live server, like the other postgres layer harnesses.
//
// Mapping to test_mysql_adversarial.cpp:
//   1 empty_seeded_table_scan          <- TestEmptySeededTableScan
//   2 zero_column_projection           <- TestZeroColumnProjection (+ unknown column)
//   3 scan_follows_txn_snapshot        (postgres-specific: native table-AM scan
//                                       under the active snapshot)
//   4 wrong_table_capture_no_publish   <- TestUnrelatedTableDoesNotPublish +
//                                         TestSqlWrongTableCaptureDoesNotPublish
//   5 capture_needs_commit             <- TestSqlCaptureNeedsExplicitCommit
//   6 abort_does_not_leak              <- TestAbortDoesNotLeakIntoLaterCommit
//   7 epoch_stable_then_advances       <- TestBehindTokenThenInvalidate (epoch
//                                         gating, observable subset through the
//                                         txn bridge)
//   8 runtime_udfs_fail_closed         <- TestRuntimeUdfsFailClosedWithoutInit
// Not portable over libpq (the MySQL harness links the library in-process;
// the postgres library needs server symbols, so those are covered only by
// test_postgres_store / test_postgres_aux_backend, which link it with stubs):
//   TestEmptyPublishIsNoop, TestMatchingVersionOverwrite (StateStore API).
// Also not covered: savepoint rollback. The production module registers a
// SubXactCallback, but the test-only bridge (extension_test_engine.cpp) only
// registers an XactCallback, so a savepoint probe would test the toy bridge,
// not the backend.
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include <libpq-fe.h>

namespace {

struct ConnGuard {
  PGconn *conn = nullptr;
  ~ConnGuard() {
    if (conn) {
      PQfinish(conn);
    }
  }
};

const char *kConnInfoCStr() {
  static std::string info = [] {
    const char *port = std::getenv("PGPORT");
    std::string s = "host=127.0.0.1 port=";
    s += (port && port[0]) ? port : "5432";
    s += " dbname=postgres user=postgres password=postgres";
    return s;
  }();
  return info.c_str();
}

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_adversarial[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

PGconn *Connect() {
  PGconn *conn = PQconnectdb(kConnInfoCStr());
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "engine_adversarial[postgres] FAIL: connection — "
              << (conn ? PQerrorMessage(conn) : "null") << "\n";
    std::abort();
  }
  return conn;
}

PGresult *ExpectOK(PGresult *res, const char *msg) {
  if (!res) {
    std::cerr << "engine_adversarial[postgres] FAIL: " << msg << " — null result\n";
    std::abort();
  }
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::cerr << "engine_adversarial[postgres] FAIL: " << msg << " — "
              << PQresStatus(status) << ": " << PQresultErrorMessage(res) << "\n";
    PQclear(res);
    std::abort();
  }
  return res;
}

void Exec(PGconn *conn, const std::string &sql, const char *label) {
  PQclear(ExpectOK(PQexec(conn, sql.c_str()), label));
}

std::string QueryOne(PGconn *conn, const std::string &sql, const char *label) {
  PGresult *res = ExpectOK(PQexec(conn, sql.c_str()), label);
  std::string val;
  if (PQntuples(res) > 0 && PQnfields(res) > 0 && PQgetvalue(res, 0, 0)) {
    val = PQgetvalue(res, 0, 0);
  }
  PQclear(res);
  return val;
}

std::vector<std::string> Column0(PGconn *conn, const std::string &sql,
                                 const char *label) {
  PGresult *res = ExpectOK(PQexec(conn, sql.c_str()), label);
  std::vector<std::string> rows;
  for (int i = 0; i < PQntuples(res); ++i) {
    const char *v = PQgetvalue(res, i, 0);
    rows.push_back(v ? v : "NULL");
  }
  PQclear(res);
  return rows;
}

bool Has(const std::string &hay, const char *needle) {
  return hay.find(needle) != std::string::npos;
}

// Unique per-process suffix for scratch tables (no fixed shared names).
std::string Uniq(const char *base) {
  return std::string(base) + "_" + std::to_string(static_cast<long>(::getpid()));
}

void LoadModule(PGconn *conn) {
  Exec(conn, "LOAD 'accelerator_codegen_pg_test'", "load");
}

void RegisterScan(PGconn *conn) {
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_scan_init() RETURNS integer AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_scan_init' "
       "LANGUAGE C STRICT",
       "register_scan_init");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_scan_rows(text, text, text, text) "
       "RETURNS SETOF text AS '$libdir/accelerator_codegen_pg_test', "
       "'pg_accel_scan_rows' LANGUAGE C STRICT",
       "register_scan_rows");
}

void RegisterTxn(PGconn *conn) {
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_txn_init() RETURNS integer AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_init' "
       "LANGUAGE C STRICT",
       "register_txn_init");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_txn_capture(bigint, integer, "
       "bigint, text) RETURNS text AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_capture' "
       "LANGUAGE C STRICT",
       "register_txn_capture");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_txn_read() RETURNS text AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_read' "
       "LANGUAGE C STRICT",
       "register_txn_read");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_txn_epoch() RETURNS text AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_epoch' "
       "LANGUAGE C STRICT",
       "register_txn_epoch");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_txn_reset() RETURNS text AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_reset' "
       "LANGUAGE C STRICT",
       "register_txn_reset");
}

void RegisterRuntime(PGconn *conn) {
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_runtime_init(text) RETURNS integer "
       "AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_init' "
       "LANGUAGE C STRICT",
       "register_rt_init");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_runtime_build() RETURNS text AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_build' "
       "LANGUAGE C STRICT",
       "register_rt_build");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_runtime_execute() RETURNS SETOF "
       "text AS '$libdir/accelerator_codegen_pg_test', "
       "'pg_accel_runtime_execute' LANGUAGE C STRICT",
       "register_rt_execute");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_runtime_read() RETURNS text AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_read' "
       "LANGUAGE C STRICT",
       "register_rt_read");
  Exec(conn,
       "CREATE OR REPLACE FUNCTION pg_accel_runtime_reset() RETURNS text AS "
       "'$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_reset' "
       "LANGUAGE C STRICT",
       "register_rt_reset");
}

// Fresh session with a freshly initialised txn bridge.
void OpenTxnSession(ConnGuard &g) {
  g.conn = Connect();
  LoadModule(g.conn);
  RegisterTxn(g.conn);
  Exec(g.conn, "SELECT pg_accel_txn_reset()", "txn_reset");
  Exec(g.conn, "SELECT pg_accel_txn_init()", "txn_init");
}

void OpenScanSession(ConnGuard &g) {
  g.conn = Connect();
  LoadModule(g.conn);
  RegisterScan(g.conn);
  Exec(g.conn, "SELECT pg_accel_scan_init()", "scan_init");
}

// 1. Scan: an existing but empty relation yields zero rows.
void TestEmptySeededTableScan() {
  ConnGuard g;
  OpenScanSession(g);
  const std::string t = Uniq("adv_empty");
  Exec(g.conn, "DROP TABLE IF EXISTS " + t, "cleanup");
  Exec(g.conn, "CREATE TABLE " + t + " (id INTEGER, name TEXT)", "create");
  auto rows = Column0(
      g.conn, "SELECT pg_accel_scan_rows('" + t + "', 'id,name', '', '')",
      "scan_empty");
  Expect(rows.empty(), "Empty table must yield 0 scan rows");
  Exec(g.conn, "DROP TABLE " + t, "cleanup");
  std::cout << "  [PASS] empty_seeded_table_scan\n";
}

// 2. Scan: zero-column projection and unknown projected column neither crash
// the backend nor corrupt later scans on the same session.
void TestZeroColumnProjection() {
  ConnGuard g;
  OpenScanSession(g);
  const std::string t = Uniq("adv_zero_proj");
  Exec(g.conn, "DROP TABLE IF EXISTS " + t, "cleanup");
  Exec(g.conn, "CREATE TABLE " + t + " (a INTEGER, b TEXT)", "create");
  Exec(g.conn, "INSERT INTO " + t + " VALUES (1,'x'),(2,'y')", "insert");

  // Zero projected columns: rows carry no cells (no "a=" / "b=").
  auto zero = Column0(
      g.conn, "SELECT pg_accel_scan_rows('" + t + "', '', '', '')", "scan_zero");
  for (const auto &r : zero) {
    Expect(!Has(r, "a=") && !Has(r, "b="),
           "Zero-column projection must not emit cells");
  }

  // Unknown column among known ones: known cells still correct, nothing
  // fabricated for the unknown one.
  auto mixed = Column0(
      g.conn,
      "SELECT pg_accel_scan_rows('" + t + "', 'a,no_such_col', '', '')",
      "scan_unknown_col");
  Expect(mixed.size() == 2, "Unknown column must not change row count");
  Expect(Has(mixed[0], "a=1") && Has(mixed[1], "a=2"),
         "Known column values preserved with unknown column present");
  Expect(!Has(mixed[0], "no_such_col") && !Has(mixed[1], "no_such_col"),
         "Unknown column must not be fabricated");

  // Session still healthy afterwards.
  auto again = Column0(
      g.conn, "SELECT pg_accel_scan_rows('" + t + "', 'a,b', '', '')",
      "scan_after");
  Expect(again.size() == 2 && Has(again[1], "b=y"),
         "Scan after odd projections still correct");
  Exec(g.conn, "DROP TABLE " + t, "cleanup");
  std::cout << "  [PASS] zero_column_projection\n";
}

// 3. Scan runs under the active snapshot: own uncommitted rows are visible,
// rolled-back rows are not, and a missing table yields nothing (fail closed).
void TestScanFollowsTxnSnapshot() {
  ConnGuard g;
  OpenScanSession(g);
  const std::string t = Uniq("adv_snap");
  Exec(g.conn, "DROP TABLE IF EXISTS " + t, "cleanup");
  Exec(g.conn, "CREATE TABLE " + t + " (id INTEGER)", "create");
  Exec(g.conn, "INSERT INTO " + t + " VALUES (1)", "seed");

  Exec(g.conn, "BEGIN", "begin");
  Exec(g.conn, "INSERT INTO " + t + " VALUES (2)", "insert_in_txn");
  auto inside = Column0(
      g.conn, "SELECT pg_accel_scan_rows('" + t + "', 'id', '', '')",
      "scan_inside");
  Expect(inside.size() == 2, "Scan must see own uncommitted row inside txn");
  Exec(g.conn, "ROLLBACK", "rollback");

  auto after = Column0(
      g.conn, "SELECT pg_accel_scan_rows('" + t + "', 'id', '', '')",
      "scan_after_rollback");
  Expect(after.size() == 1 && Has(after[0], "id=1"),
         "Rolled-back row must not be scanned");

  auto missing = Column0(
      g.conn,
      "SELECT pg_accel_scan_rows('" + Uniq("adv_no_such_table") +
          "', 'id', '', '')",
      "scan_missing");
  Expect(missing.empty(), "Missing table must scan as empty");
  Exec(g.conn, "DROP TABLE " + t, "cleanup");
  std::cout << "  [PASS] scan_follows_txn_snapshot\n";
}

// 4. Txn: mutation for a table the toy module does not depend on must not
// stage/publish (layer tests only capture table 1).
void TestWrongTableCaptureDoesNotPublish() {
  ConnGuard g;
  OpenTxnSession(g);
  Exec(g.conn, "BEGIN", "begin");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(99::bigint, 0, 1::bigint, 'nope')",
       "capture_99");
  Exec(g.conn, "COMMIT", "commit");
  std::string state = QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read");
  Expect(!Has(state, "available=1"), "Wrong table_id commit must not publish");
  Expect(!Has(state, "label=staged"), "No staged image for wrong table_id");
  std::cout << "  [PASS] wrong_table_capture_no_publish\n";
}

// 5. Txn: capture inside an open transaction is invisible until COMMIT, and
// stays unpublished if that transaction later rolls back.
void TestCaptureNeedsCommit() {
  ConnGuard g;
  OpenTxnSession(g);
  Exec(g.conn, "BEGIN", "begin");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 5::bigint, 'pending')",
       "capture");
  std::string mid = QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read_mid");
  Expect(!Has(mid, "available=1"), "Capture before COMMIT must not publish");
  Exec(g.conn, "ROLLBACK", "rollback");
  std::string after =
      QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read_after_rollback");
  Expect(!Has(after, "available=1"),
         "Rolled-back capture must stay unpublished");

  // Autocommit statement: the implicit transaction commits at statement end.
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 6::bigint, 'autocommit')",
       "capture_autocommit");
  std::string pub =
      QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read_autocommit");
  Expect(Has(pub, "available=1"),
         "Autocommit capture must publish when its implicit txn commits");
  std::cout << "  [PASS] capture_needs_commit\n";
}

// 6. Txn: an aborted capture must not leak into a later commit that itself
// has nothing relevant to publish.
void TestAbortDoesNotLeakIntoLaterCommit() {
  ConnGuard g;
  OpenTxnSession(g);
  Exec(g.conn, "BEGIN", "begin1");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 1::bigint, 'aborted')",
       "capture_aborted");
  Exec(g.conn, "ROLLBACK", "rollback");

  Exec(g.conn, "BEGIN", "begin2");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(99::bigint, 0, 2::bigint, 'irrelevant')",
       "capture_irrelevant");
  Exec(g.conn, "COMMIT", "commit");
  std::string state = QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read");
  Expect(!Has(state, "available=1"),
         "Aborted capture leaked into a later unrelated commit");

  // And a plain empty BEGIN/COMMIT afterwards is still a no-op.
  Exec(g.conn, "BEGIN", "begin3");
  Exec(g.conn, "COMMIT", "commit_empty");
  state = QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read_empty");
  Expect(!Has(state, "available=1"), "Empty commit must not publish");
  std::cout << "  [PASS] abort_does_not_leak\n";
}

// 7. Visibility: epoch is stable across an aborted and an unrelated commit,
// advances on a relevant commit, and a following abort does not move it back
// or erase the published image.
void TestEpochStableThenAdvances() {
  ConnGuard g;
  OpenTxnSession(g);
  std::string e0 = QueryOne(g.conn, "SELECT pg_accel_txn_epoch()", "e0");

  Exec(g.conn, "BEGIN", "b1");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 1::bigint, 'x')", "c1");
  Exec(g.conn, "ROLLBACK", "r1");
  Expect(QueryOne(g.conn, "SELECT pg_accel_txn_epoch()", "e1") == e0,
         "Epoch must not move on abort");

  Exec(g.conn, "BEGIN", "b2");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(99::bigint, 0, 1::bigint, 'x')", "c2");
  Exec(g.conn, "COMMIT", "k2");
  std::string state = QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read2");
  Expect(!Has(state, "available=1"), "Unrelated commit must not publish");

  Exec(g.conn, "BEGIN", "b3");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 9::bigint, 'real')", "c3");
  Exec(g.conn, "COMMIT", "k3");
  std::string e3 = QueryOne(g.conn, "SELECT pg_accel_txn_epoch()", "e3");
  Expect(e3 != e0, "Epoch must advance on relevant commit");
  Expect(Has(QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read3"),
             "available=1"),
         "Relevant commit must publish");

  Exec(g.conn, "BEGIN", "b4");
  Exec(g.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 10::bigint, 'again')", "c4");
  Exec(g.conn, "ROLLBACK", "r4");
  Expect(QueryOne(g.conn, "SELECT pg_accel_txn_epoch()", "e4") == e3,
         "Abort after publish must not move the epoch");
  Expect(Has(QueryOne(g.conn, "SELECT pg_accel_txn_read()", "read4"),
             "available=1"),
         "Abort after publish must not erase the published image");
  std::cout << "  [PASS] epoch_stable_then_advances\n";
}

// 8. Runtime UDFs fail closed: build/execute/read without init do not crash
// the backend and do not return a golden image; reset is idempotent.
void TestRuntimeUdfsFailClosedWithoutInit() {
  ConnGuard g;
  g.conn = Connect();
  LoadModule(g.conn);
  RegisterRuntime(g.conn);
  Exec(g.conn, "SELECT pg_accel_runtime_reset()", "reset");
  Exec(g.conn, "SELECT pg_accel_runtime_reset()", "reset_again");

  std::string built = QueryOne(g.conn, "SELECT pg_accel_runtime_build()", "build");
  Expect(built != "ok", "build without init must not report ok");

  auto rows = Column0(g.conn, "SELECT pg_accel_runtime_execute()", "execute");
  for (const auto &r : rows) {
    Expect(!Has(r, "one") && !Has(r, "available=1"),
           "execute without init must not return a golden image");
  }

  std::string read = QueryOne(g.conn, "SELECT pg_accel_runtime_read()", "read");
  Expect(!Has(read, "available=1"),
         "read without init must not report a published image");

  // Backend survived: trivial query still works.
  Expect(QueryOne(g.conn, "SELECT 1", "alive") == "1", "Backend must survive");
  std::cout << "  [PASS] runtime_udfs_fail_closed\n";
}

}  // namespace

int main() {
  std::cout << "engine_adversarial[postgres]\n";
  TestEmptySeededTableScan();
  TestZeroColumnProjection();
  TestScanFollowsTxnSnapshot();
  TestWrongTableCaptureDoesNotPublish();
  TestCaptureNeedsCommit();
  TestAbortDoesNotLeakIntoLaterCommit();
  TestEpochStableThenAdvances();
  TestRuntimeUdfsFailClosedWithoutInit();
  std::cout << "engine_adversarial[postgres] PASS\n";
  return 0;
}
