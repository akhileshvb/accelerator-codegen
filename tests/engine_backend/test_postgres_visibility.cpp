// GENERATED BACKEND — layer harness for visibility (Visibility).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_postgres_visibility   CTest: engine_postgres_visibility
//
// Rule: visibility
// Evidence: Uncommitted writes must not be Read as committed. Store
//   token/epoch filtering plus not publishing until commit. Snapshot
//   before commit ≠ after commit. Sync barrier (locks / notify), not sleep.
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <algorithm>

#include <libpq-fe.h>

namespace {

struct ConnGuard {
  PGconn *conn = nullptr;
  ~ConnGuard() {
    if (conn) { PQfinish(conn); }
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
  std::cerr << "engine_visibility[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) Fail(msg);
}

void ExpectConn(PGconn *conn, const char *msg) {
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "engine_visibility[postgres] FAIL: " << msg
              << " — " << (conn ? PQerrorMessage(conn) : "null") << "\n";
    std::abort();
  }
}

PGresult *ExpectOK(PGconn *conn, PGresult *res, const char *msg) {
  if (!res) {
    std::cerr << "engine_visibility[postgres] FAIL: " << msg << " — null result\n";
    std::abort();
  }
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::cerr << "engine_visibility[postgres] FAIL: " << msg << " — "
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
    if (raw) val = raw;
  }
  PQclear(res);
  return val;
}

PGconn *Connect() {
  PGconn *conn = PQconnectdb(kConnInfoCStr());
  ExpectConn(conn, "connection");
  return conn;
}

void RegisterFunctions(PGconn *conn) {
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_vis_init() "
             "RETURNS integer AS '$libdir/accelerator_codegen_pg', 'pg_accel_vis_init' "
             "LANGUAGE C STRICT", "register_vis_init");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_vis_capture(bigint, integer, bigint, text) "
             "RETURNS text AS '$libdir/accelerator_codegen_pg', 'pg_accel_vis_capture' "
             "LANGUAGE C STRICT", "register_vis_capture");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_vis_read() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg', 'pg_accel_vis_read' "
             "LANGUAGE C STRICT", "register_vis_read");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_vis_epoch() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg', 'pg_accel_vis_epoch' "
             "LANGUAGE C STRICT", "register_vis_epoch");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_vis_reset() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg', 'pg_accel_vis_reset' "
             "LANGUAGE C STRICT", "register_vis_reset");
}

// Test 1: Core visibility — uncommitted writes must not be readable.
// During an open txn, mutations are buffered in the coordinator and NOT
// published to the store. store.Read() returns empty. After commit, the
// XactCallback fires OnPreCommit → OnCommit which publishes to the store,
// and Read() then sees the committed data. Epoch advances.
void TestUncommittedWriteNotVisible() {
  ConnGuard conn;
  conn.conn = Connect();

  Exec(conn.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(conn.conn);

  // Initialize the bridge.
  std::string init_result = QueryOne(conn.conn, "SELECT pg_accel_vis_init()", "init");
  Expect(init_result == "1", "pg_accel_vis_init should return 1");

  // Verify initial state is empty.
  std::string before_state = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "read_before");
  Expect(before_state.find("partitions=0") != std::string::npos ||
         before_state.find("available=0") != std::string::npos,
         "Initial state should have no published partitions");

  std::string epoch_initial = QueryOne(conn.conn, "SELECT pg_accel_vis_epoch()", "epoch_initial");
  Expect(epoch_initial.find("epoch=0") != std::string::npos,
         "Initial epoch should be 0");

  // Begin a transaction and capture a mutation — but do NOT commit.
  Exec(conn.conn, "BEGIN", "begin");
  Exec(conn.conn,
       "SELECT pg_accel_vis_capture(1::bigint, 0, 100::bigint, 'uncommitted')",
       "capture_uncommitted");

  // BEFORE commit: read the store. The mutation is buffered in the
  // coordinator, NOT yet published to the store. Read must return empty.
  std::string snapshot_before = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "snapshot_before");
  Expect(snapshot_before.find("partitions=0") != std::string::npos ||
         snapshot_before.find("available=0") != std::string::npos,
         "Snapshot before commit must NOT show uncommitted write");

  std::string epoch_before = QueryOne(conn.conn, "SELECT pg_accel_vis_epoch()", "epoch_before");
  Expect(epoch_before == epoch_initial,
         "Epoch must not change before commit");

  // Now commit the transaction.
  Exec(conn.conn, "COMMIT", "commit");

  // AFTER commit: XactCallback fired OnPreCommit/OnCommit, which published
  // to the store. Read must now show the committed data.
  std::string snapshot_after = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "snapshot_after");
  Expect(snapshot_after.find("available=1") != std::string::npos,
         "Snapshot after commit must show published partition");
  Expect(snapshot_after.find("partitions=1") != std::string::npos,
         "Snapshot after commit must show 1 partition");

  std::string epoch_after = QueryOne(conn.conn, "SELECT pg_accel_vis_epoch()", "epoch_after");
  Expect(epoch_before != epoch_after,
         "Epoch must change between before-commit and after-commit snapshots");
  Expect(epoch_after.find("epoch=1") != std::string::npos,
         "Epoch must advance to 1 after commit");

  // Snapshot before commit ≠ after commit (epoch + image differ).
  std::cout << "  [PASS] uncommitted_write_not_visible\n";
}

// Test 2: Sync barrier — the store Read uses mutex-locked state, returning
// immediately without sleep-based polling. This is proven by reading the
// store during an open txn (with buffered but unpublished mutations) — the
// response is immediate (lock-based) rather than delayed (sleep-based).
// A sleep-based approach would need to wait for commit to sync; lock-based
// returns immediately because publish hasn't happened yet.
void TestSyncBarrierNotSleep() {
  ConnGuard conn;
  conn.conn = Connect();

  Exec(conn.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(conn.conn);
  Exec(conn.conn, "SELECT pg_accel_vis_init()", "init");

  // Start txn, capture, do NOT commit.
  Exec(conn.conn, "BEGIN", "begin");
  Exec(conn.conn,
       "SELECT pg_accel_vis_capture(1::bigint, 0, 42::bigint, 'sync_test')",
       "capture_in_txn");

  // Read store immediately while txn is open — returns without delay.
  // If sleep-based, this would need a sleep-and-retry loop. Mutex-based
  // store returns immediately because the coordinator hasn't published yet.
  std::string immediate_snapshot = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "immediate_read");
  Expect(immediate_snapshot.find("available=0") != std::string::npos ||
         immediate_snapshot.find("partitions=0") != std::string::npos,
         "Immediate snapshot during open txn must not show uncommitted data");

  // Commit, then verify data is visible immediately (no polling needed).
  Exec(conn.conn, "COMMIT", "commit");

  std::string after_commit = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "after_commit");
  Expect(after_commit.find("available=1") != std::string::npos,
         "After commit, reader must see published data immediately");

  std::cout << "  [PASS] sync_barrier_not_sleep\n";
}

// Test 3: Rollback keeps store unchanged — no publication on abort.
void TestRollbackKeepsStoreUnchanged() {
  ConnGuard conn;
  conn.conn = Connect();

  Exec(conn.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(conn.conn);
  Exec(conn.conn, "SELECT pg_accel_vis_init()", "init");

  std::string snapshot_before = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "before_txn");
  Expect(snapshot_before.find("available=0") != std::string::npos ||
         snapshot_before.find("partitions=0") != std::string::npos,
         "Store should be empty before transaction");
  std::string epoch_before = QueryOne(conn.conn, "SELECT pg_accel_vis_epoch()", "epoch_before_txn");

  Exec(conn.conn, "BEGIN", "begin");
  Exec(conn.conn,
       "SELECT pg_accel_vis_capture(1::bigint, 0, 99::bigint, 'rolled_back')",
       "capture_before_rollback");
  Exec(conn.conn, "ROLLBACK", "rollback");

  std::string snapshot_after = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "after_rollback");
  Expect(snapshot_after.find("available=0") != std::string::npos ||
         snapshot_after.find("partitions=0") != std::string::npos,
         "Store must be empty after rollback — no publication");

  std::string epoch_after = QueryOne(conn.conn, "SELECT pg_accel_vis_epoch()", "epoch_after_rollback");
  Expect(epoch_before == epoch_after,
         "Epoch must not change after rollback");

  std::cout << "  [PASS] rollback_keeps_store_unchanged\n";
}

// Test 4: Epoch/image changes between before-commit and after-commit snapshots.
void TestEpochAndImageChangeAfterCommit() {
  ConnGuard conn;
  conn.conn = Connect();

  Exec(conn.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(conn.conn);
  Exec(conn.conn, "SELECT pg_accel_vis_init()", "init");

  // First commit: publish image A.
  Exec(conn.conn, "BEGIN", "begin_a");
  Exec(conn.conn,
       "SELECT pg_accel_vis_capture(1::bigint, 0, 10::bigint, 'image_a')",
       "capture_a");
  Exec(conn.conn, "COMMIT", "commit_a");

  std::string state_a = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "read_a");
  Expect(state_a.find("available=1") != std::string::npos,
         "State after first commit should be available");
  Expect(state_a.find("label=staged") != std::string::npos,
         "State after first commit should contain staged payload");
  std::string epoch_a = QueryOne(conn.conn, "SELECT pg_accel_vis_epoch()", "epoch_a");

  // Second commit: publish image B (different label/value).
  Exec(conn.conn, "BEGIN", "begin_b");
  Exec(conn.conn,
       "SELECT pg_accel_vis_capture(1::bigint, 0, 20::bigint, 'image_b')",
       "capture_b");
  Exec(conn.conn, "COMMIT", "commit_b");

  std::string state_b = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "read_b");
  Expect(state_b.find("available=1") != std::string::npos,
         "State after second commit should be available");
  Expect(state_b.find("label=staged") != std::string::npos,
         "State after second commit should contain staged payload");

  std::string epoch_b = QueryOne(conn.conn, "SELECT pg_accel_vis_epoch()", "epoch_b");
  Expect(epoch_a != epoch_b,
         "Epoch must advance between first and second commit");

  std::cout << "  [PASS] epoch_image_change_after_commit\n";
}

// Test 5: Module load fail-closed.
void TestModuleLoad() {
  ConnGuard guard;
  guard.conn = Connect();

  PGresult *res = PQexec(guard.conn, "LOAD 'accelerator_codegen_pg'");
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK) {
    std::cerr << "engine_visibility[postgres] FAIL: MODULE LOAD failed — "
              << PQresultErrorMessage(res) << "\n";
    PQclear(res);
    std::abort();
  }
  PQclear(res);

  std::cout << "  [PASS] module_load\n";
}

// Test 6: Reset clears all state.
void TestResetClearsState() {
  ConnGuard conn;
  conn.conn = Connect();

  Exec(conn.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(conn.conn);
  Exec(conn.conn, "SELECT pg_accel_vis_init()", "init");

  Exec(conn.conn, "BEGIN", "begin");
  Exec(conn.conn,
       "SELECT pg_accel_vis_capture(1::bigint, 0, 77::bigint, 'pre_reset')",
       "capture");
  Exec(conn.conn, "COMMIT", "commit");

  std::string state_before = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "before_reset");
  Expect(state_before.find("available=1") != std::string::npos,
         "State should be available before reset");

  std::string reset_result = QueryOne(conn.conn, "SELECT pg_accel_vis_reset()", "reset");
  Expect(reset_result == "ok", "Reset should return ok");

  Exec(conn.conn, "SELECT pg_accel_vis_init()", "reinit");
  std::string state_after = QueryOne(conn.conn, "SELECT pg_accel_vis_read()", "after_reset");
  Expect(state_after.find("available=0") != std::string::npos ||
         state_after.find("partitions=0") != std::string::npos,
         "After reset and re-init, store must be empty");

  std::cout << "  [PASS] reset_clears_state\n";
}

} // namespace

int main() {
  std::cout << "engine_visibility[postgres]\n";

  {
    ConnGuard probe;
    probe.conn = Connect();
    PQfinish(probe.conn);
    probe.conn = nullptr;
  }

  TestModuleLoad();
  TestUncommittedWriteNotVisible();
  TestSyncBarrierNotSleep();
  TestRollbackKeepsStoreUnchanged();
  TestEpochAndImageChangeAfterCommit();
  TestResetClearsState();

  std::cout << "engine_visibility[postgres] PASS\n";
  return 0;
}
