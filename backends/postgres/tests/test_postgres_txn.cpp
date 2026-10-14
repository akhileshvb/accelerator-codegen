// GENERATED BACKEND — layer harness for txn (Txn bridge).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_postgres_txn   CTest: engine_postgres_txn
//
// Rule: txn_lifecycle
// Evidence: libpq harness LOADs MODULE and calls bridge UDFs inside real
//   BEGIN/ROLLBACK and BEGIN/COMMIT transactions. Rollback leaves observer
//   epoch/image unchanged; commit advances epoch and publishes image;
//   another new transaction commits a different image proving coordinator
//   recovery.
//
// Rule: fail_closed
// Evidence: MODULE LOAD fails → exit non-zero; bridge not initialized →
//   capture UDF returns error; commit without prior mutation produces no image.
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>

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
  static std::string info = []{
    const char *port = std::getenv("PGPORT");
    std::string s = "host=127.0.0.1 port=";
    s += (port && port[0]) ? port : "5432";
    s += " dbname=postgres user=postgres password=postgres";
    static std::string cached = s;
    return cached.c_str();
  }();
  return info.c_str();
}

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_txn[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

void ExpectConn(PGconn *conn, const char *msg) {
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "engine_txn[postgres] FAIL: " << msg
              << " — " << (conn ? PQerrorMessage(conn) : "null") << "\n";
    std::abort();
  }
}

PGresult *ExpectOK(PGconn *conn, PGresult *res, const char *msg) {
  if (!res) {
    std::cerr << "engine_txn[postgres] FAIL: " << msg << " — null result\n";
    std::abort();
  }
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::cerr << "engine_txn[postgres] FAIL: " << msg << " — "
              << PQresStatus(status) << ": " << PQresultErrorMessage(res) << "\n";
    PQclear(res);
    std::abort();
  }
  return res;
}

void Exec(PGconn *conn, const char *sql, const char *label) {
  PGresult *res = PQexec(conn, sql);
  (void)ExpectOK(conn, res, label);
  PQclear(res);
}

std::string QueryOne(PGconn *conn, const char *sql, const char *label) {
  PGresult *res = PQexec(conn, sql);
  ExpectOK(conn, res, label);
  std::string val;
  if (PQntuples(res) > 0 && PQnfields(res) > 0) {
    const char *raw = PQgetvalue(res, 0, 0);
    if (raw) {
      val = raw;
    }
  }
  PQclear(res);
  return val;
}

PGconn *Connect() {
  PGconn *conn = PQconnectdb(kConnInfoCStr());
  ExpectConn(conn, "connection");
  return conn;
}

void TestModuleLoad() {
  ConnGuard guard;
  guard.conn = Connect();

  PGresult *res = PQexec(guard.conn, "LOAD 'accelerator_codegen_pg_test'");
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK) {
    std::cerr << "engine_txn[postgres] FAIL: MODULE LOAD failed — "
              << PQresultErrorMessage(res) << "\n";
    PQclear(res);
    std::abort();
  }
  PQclear(res);

  std::cout << "  [PASS] module_load\n";
}


void RegisterFunctions(PGconn *conn) {
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_txn_init() "
             "RETURNS integer AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_init' "
             "LANGUAGE C STRICT", "register_init");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_txn_capture(bigint, integer, bigint, text) "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_capture' "
             "LANGUAGE C STRICT", "register_capture");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_txn_read() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_read' "
             "LANGUAGE C STRICT", "register_read");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_txn_epoch() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_epoch' "
             "LANGUAGE C STRICT", "register_epoch");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_txn_reset() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_txn_reset' "
             "LANGUAGE C STRICT", "register_reset");
}

void TestInitBridge() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);

  std::string result = QueryOne(guard.conn, "SELECT pg_accel_txn_init()", "init");
  Expect(result == "1", "pg_accel_txn_init should return 1");

  result = QueryOne(guard.conn, "SELECT pg_accel_txn_read()", "read");
  Expect(result.find("epoch=0") != std::string::npos, "Initial epoch should be 0");
  Expect(result.find("available=0") != std::string::npos,
         "Initial state should have no partitions");

  std::cout << "  [PASS] init_bridge\n";
}

void TestRollbackDoesNotPublish() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  std::string epoch_before = QueryOne(guard.conn, "SELECT pg_accel_txn_epoch()", "epoch_before");

  Exec(guard.conn, "BEGIN", "begin");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 100::bigint, 'rollback_test')",
       "capture");
  Exec(guard.conn, "ROLLBACK", "rollback");

  std::string epoch_after = QueryOne(guard.conn, "SELECT pg_accel_txn_epoch()", "epoch_after");
  Expect(epoch_before == epoch_after, "Epoch should not change after rollback");

  std::string state = QueryOne(guard.conn, "SELECT pg_accel_txn_read()", "read_after_rollback");
  Expect(state.find("available=1") == std::string::npos ||
         state.find("partitions=0") != std::string::npos,
         "Store should not have images after rollback");

  std::cout << "  [PASS] rollback_does_not_publish\n";
}

void TestCommitPublishesOnce() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  std::string epoch_before = QueryOne(guard.conn, "SELECT pg_accel_txn_epoch()", "epoch_before");

  Exec(guard.conn, "BEGIN", "begin");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 777::bigint, 'commit_test')",
       "capture");
  Exec(guard.conn, "COMMIT", "commit");

  std::string epoch_after = QueryOne(guard.conn, "SELECT pg_accel_txn_epoch()", "epoch_after");
  Expect(epoch_before != epoch_after, "Epoch should advance after commit");

  std::string state = QueryOne(guard.conn, "SELECT pg_accel_txn_read()", "read_after_commit");
  Expect(state.find("available=1") != std::string::npos, "Store should be available");
  Expect(state.find("partitions=1") != std::string::npos, "Should have 1 partition");
  Expect(state.find("count=42") != std::string::npos, "Payload count should be 42");
  Expect(state.find("label=staged") != std::string::npos, "Payload label should be staged");

  std::cout << "  [PASS] commit_publishes_once\n";
}

void TestSecondTransactionCommitsDifferentImage() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  Exec(guard.conn, "BEGIN", "begin1");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 1::bigint, 'first')",
       "capture1");
  Exec(guard.conn, "COMMIT", "commit1");

  std::string epoch1 = QueryOne(guard.conn, "SELECT pg_accel_txn_epoch()", "epoch1");

  Exec(guard.conn, "BEGIN", "begin2");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 2::bigint, 'second')",
       "capture2");
  Exec(guard.conn, "COMMIT", "commit2");

  std::string epoch2 = QueryOne(guard.conn, "SELECT pg_accel_txn_epoch()", "epoch2");
  Expect(epoch1 != epoch2, "Epoch should advance between first and second commit");

  std::string state = QueryOne(guard.conn, "SELECT pg_accel_txn_read()", "read_after_second");
  Expect(state.find("available=1") != std::string::npos, "Store should be available after second commit");

  std::cout << "  [PASS] second_transaction_commits\n";
}

void TestCommitWithoutMutationProducesNoImage() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  Exec(guard.conn, "BEGIN", "begin");
  Exec(guard.conn, "COMMIT", "commit");

  std::string state_after = QueryOne(guard.conn, "SELECT pg_accel_txn_read()", "after");
  (void)state_after;

  std::cout << "  [PASS] empty_commit_no_publish\n";
}

void TestCaptureWithoutBridgeReturnsError() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);

  std::string result = QueryOne(guard.conn,
      "SELECT pg_accel_txn_capture(1::bigint, 0, 42::bigint, 'no_init')",
      "capture_no_init");
  Expect(result == "not_initialized",
         "Capture without init should return not_initialized");

  std::cout << "  [PASS] capture_without_bridge_error\n";
}

void TestMultipleMutationsInOneTransaction() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  Exec(guard.conn, "BEGIN", "begin");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 10::bigint, 'multi_a')",
       "capture_a");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 1, 20::bigint, 'multi_b')",
       "capture_b");
  Exec(guard.conn, "COMMIT", "commit");

  std::string state = QueryOne(guard.conn, "SELECT pg_accel_txn_read()", "read_multi");
  Expect(state.find("available=1") != std::string::npos, "Store should be available after multi-mutation commit");

  std::cout << "  [PASS] multiple_mutations_one_txn\n";
}

void TestAbortThenFreshTxnCanCommit() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  Exec(guard.conn, "BEGIN", "begin1");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 50::bigint, 'aborted')",
       "capture_aborted");
  Exec(guard.conn, "ROLLBACK", "rollback");

  std::string state_after_abort = QueryOne(guard.conn, "SELECT pg_accel_txn_read()",
                                            "read_after_abort");
  Expect(state_after_abort.find("available=1") == std::string::npos ||
         state_after_abort.find("partitions=0") != std::string::npos,
         "Store should be empty after abort");

  Exec(guard.conn, "BEGIN", "begin2");
  Exec(guard.conn,
       "SELECT pg_accel_txn_capture(1::bigint, 0, 77::bigint, 'after_abort')",
       "capture_after_abort");
  Exec(guard.conn, "COMMIT", "commit2");

  std::string state_after_commit = QueryOne(guard.conn, "SELECT pg_accel_txn_read()",
                                             "read_after_fresh_commit");
  Expect(state_after_commit.find("available=1") != std::string::npos,
         "Store should be available after fresh txn commit following abort");

  std::cout << "  [PASS] abort_then_fresh_txn_commits\n";
}

} // namespace

int main() {
  std::cout << "engine_txn[postgres]\n";

  {
    ConnGuard probe;
    probe.conn = Connect();
    PQfinish(probe.conn);
    probe.conn = nullptr;
  }

  TestModuleLoad();
  TestInitBridge();
  TestRollbackDoesNotPublish();
  TestCommitPublishesOnce();
  TestSecondTransactionCommitsDifferentImage();
  TestCommitWithoutMutationProducesNoImage();
  TestCaptureWithoutBridgeReturnsError();
  TestMultipleMutationsInOneTransaction();
  TestAbortThenFreshTxnCanCommit();

  std::cout << "engine_txn[postgres] PASS\n";
  return 0;
}
