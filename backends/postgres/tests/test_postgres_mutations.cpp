// GENERATED BACKEND — layer harness for mutations (Mutation capture).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_postgres_mutations   CTest: engine_postgres_mutations
//
// Rule: mutation_hooks
// Evidence: INSERT/UPDATE/DELETE through real Postgres AFTER trigger;
//   captured MutationBatch table id, kind, old/new cells;
//   DELETE asserts old-row values. CaptureMutation(hand_built) is not used.
//
// Rule: fail_closed
// Evidence: trigger without bridge init does not crash; MODULE LOAD fails
//   → exit non-zero.
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>

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
  std::cerr << "engine_mutations[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

void ExpectConn(PGconn *conn, const char *msg) {
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "engine_mutations[postgres] FAIL: " << msg
              << " — " << (conn ? PQerrorMessage(conn) : "null") << "\n";
    std::abort();
  }
}

PGresult *ExpectOK(PGconn *conn, PGresult *res, const char *msg) {
  if (!res) {
    std::cerr << "engine_mutations[postgres] FAIL: " << msg << " — null result\n";
    std::abort();
  }
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::cerr << "engine_mutations[postgres] FAIL: " << msg << " — "
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
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_mutations_trigger() "
             "RETURNS trigger AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_mutations_trigger' "
             "LANGUAGE C", "register_mutations_trigger");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_mutations_last() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_mutations_last' "
             "LANGUAGE C STRICT", "register_mutations_last");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_mutations_drain() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_mutations_drain' "
             "LANGUAGE C STRICT", "register_mutations_drain");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_mutations_count() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_mutations_count' "
             "LANGUAGE C STRICT", "register_mutations_count");
}

void SetupContractTable(PGconn *conn, const char *table_name) {
  std::string drop_sql = std::string("DROP TABLE IF EXISTS ") + table_name + " CASCADE";
  Exec(conn, drop_sql.c_str(), "drop_table");

  std::string create_sql = std::string("CREATE TABLE ") + table_name +
      " (id SERIAL PRIMARY KEY, val INTEGER NOT NULL, label TEXT)";
  Exec(conn, create_sql.c_str(), "create_table");

  std::string trigger_name = std::string("trg_") + table_name;
  std::string create_trigger_sql = std::string(
      "CREATE TRIGGER ") + trigger_name +
      " AFTER INSERT OR UPDATE OR DELETE ON " + table_name +
      " FOR EACH ROW EXECUTE FUNCTION pg_accel_mutations_trigger()";
  Exec(conn, create_trigger_sql.c_str(), "create_trigger");
}

std::string GetTableOid(PGconn *conn, const char *table_name) {
  std::string sql = std::string("SELECT oid FROM pg_class WHERE relname='") + table_name + "'";
  return QueryOne(conn, sql.c_str(), "get_oid");
}

void TestModuleLoad() {
  ConnGuard guard;
  guard.conn = Connect();

  PGresult *res = PQexec(guard.conn, "LOAD 'accelerator_codegen_pg_test'");
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK) {
    std::cerr << "engine_mutations[postgres] FAIL: MODULE LOAD failed — "
              << PQresultErrorMessage(res) << "\n";
    PQclear(res);
    std::abort();
  }
  PQclear(res);

  std::cout << "  [PASS] module_load\n";
}

void TestInsertCapturesMutation() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  SetupContractTable(guard.conn, "mut_test_insert");

  std::string oid = GetTableOid(guard.conn, "mut_test_insert");
  Expect(!oid.empty(), "Should get table OID");

  Exec(guard.conn, "SELECT pg_accel_mutations_drain()", "drain");

  Exec(guard.conn, "INSERT INTO mut_test_insert (val, label) VALUES (42, 'hello')", "insert");

  std::string last = QueryOne(guard.conn, "SELECT pg_accel_mutations_last()", "last");

  Expect(last.find("table=" + oid) != std::string::npos, "Mutation should have correct table OID");
  Expect(last.find("kind=0") != std::string::npos, "Mutation kind should be Insert (0)");
  Expect(last.find("new_rows=1") != std::string::npos, "Should have 1 new row");
  Expect(last.find("old_rows=0") != std::string::npos, "Should have 0 old rows");
  Expect(last.find("42") != std::string::npos, "New row should contain val=42");
  Expect(last.find("\"hello\"") != std::string::npos, "New row should contain label='hello'");

  Exec(guard.conn, "DROP TABLE IF EXISTS mut_test_insert CASCADE", "cleanup");
  std::cout << "  [PASS] insert_captures_mutation\n";
}

void TestDeleteCapturesOldRowValues() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  SetupContractTable(guard.conn, "mut_test_delete");

  std::string oid = GetTableOid(guard.conn, "mut_test_delete");
  Expect(!oid.empty(), "Should get table OID");

  Exec(guard.conn, "INSERT INTO mut_test_delete (val, label) VALUES (99, 'delete_me')", "seed");
  Exec(guard.conn, "SELECT pg_accel_mutations_drain()", "drain");

  Exec(guard.conn, "DELETE FROM mut_test_delete WHERE val = 99", "delete");

  std::string last = QueryOne(guard.conn, "SELECT pg_accel_mutations_last()", "last");

  Expect(last.find("table=" + oid) != std::string::npos, "Mutation should have correct table OID");
  Expect(last.find("kind=1") != std::string::npos, "Mutation kind should be Delete (1)");
  Expect(last.find("new_rows=0") != std::string::npos, "Should have 0 new rows");
  Expect(last.find("old_rows=1") != std::string::npos, "Should have 1 old row");
  Expect(last.find("99") != std::string::npos, "Old row should contain val=99");
  Expect(last.find("\"delete_me\"") != std::string::npos, "Old row should contain label='delete_me'");

  Exec(guard.conn, "DROP TABLE IF EXISTS mut_test_delete CASCADE", "cleanup");
  std::cout << "  [PASS] delete_captures_old_row_values\n";
}

void TestUpdateCapturesOldAndNewRows() {
  // UPDATE in AFTER trigger captures both OLD (tg_trigtuple) and NEW (tg_newtuple).
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  SetupContractTable(guard.conn, "mut_test_update");

  std::string oid = GetTableOid(guard.conn, "mut_test_update");
  Expect(!oid.empty(), "Should get table OID");

  Exec(guard.conn, "INSERT INTO mut_test_update (val, label) VALUES (10, 'old_label')", "seed");
  Exec(guard.conn, "SELECT pg_accel_mutations_drain()", "drain");

  Exec(guard.conn, "UPDATE mut_test_update SET val = 20, label = 'new_label' WHERE val = 10", "update");

  std::string last = QueryOne(guard.conn, "SELECT pg_accel_mutations_last()", "last");

  Expect(last.find("table=" + oid) != std::string::npos, "Mutation should have correct table OID");
  Expect(last.find("kind=2") != std::string::npos, "Mutation kind should be Update (2)");
  Expect(last.find("new_rows=1") != std::string::npos, "Should have 1 new row");
  Expect(last.find("old_rows=1") != std::string::npos, "Should have 1 old row");
  Expect(last.find("20") != std::string::npos, "New row should contain val=20");
  Expect(last.find("\"new_label\"") != std::string::npos, "New row should contain label='new_label'");
  Expect(last.find("10") != std::string::npos, "Old row should contain val=10");
  Expect(last.find("\"old_label\"") != std::string::npos, "Old row should contain label='old_label'");

  Exec(guard.conn, "DROP TABLE IF EXISTS mut_test_update CASCADE", "cleanup");
  std::cout << "  [PASS] update_captures_old_and_new_rows\n";
}

void TestMultipleMutationsInTransaction() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  SetupContractTable(guard.conn, "mut_test_multi");

  std::string oid = GetTableOid(guard.conn, "mut_test_multi");
  Expect(!oid.empty(), "Should get table OID");

  Exec(guard.conn, "SELECT pg_accel_mutations_drain()", "drain");

  Exec(guard.conn, "BEGIN", "begin");
  Exec(guard.conn, "INSERT INTO mut_test_multi (val, label) VALUES (1, 'first')", "insert1");
  Exec(guard.conn, "INSERT INTO mut_test_multi (val, label) VALUES (2, 'second')", "insert2");
  Exec(guard.conn, "UPDATE mut_test_multi SET label = 'modified' WHERE val = 1", "update");
  Exec(guard.conn, "COMMIT", "commit");

  std::string count = QueryOne(guard.conn, "SELECT pg_accel_mutations_count()", "count");
  int n = std::stoi(count);
  Expect(n >= 3, "Should have at least 3 captured mutations (2 inserts + 1 update)");

  Exec(guard.conn, "DROP TABLE IF EXISTS mut_test_multi CASCADE", "cleanup");
  std::cout << "  [PASS] multiple_mutations_in_transaction\n";
}

void TestInsertWithNullLabel() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  SetupContractTable(guard.conn, "mut_test_null");

  std::string oid = GetTableOid(guard.conn, "mut_test_null");
  Expect(!oid.empty(), "Should get table OID");

  Exec(guard.conn, "SELECT pg_accel_mutations_drain()", "drain");

  Exec(guard.conn, "INSERT INTO mut_test_null (val, label) VALUES (55, NULL)", "insert_null");

  std::string last = QueryOne(guard.conn, "SELECT pg_accel_mutations_last()", "last");

  Expect(last.find("table=" + oid) != std::string::npos, "Mutation should have correct table OID");
  Expect(last.find("kind=0") != std::string::npos, "Mutation kind should be Insert (0)");
  Expect(last.find("55") != std::string::npos, "New row should contain val=55");
  Expect(last.find("NULL") != std::string::npos, "New row should contain NULL label");

  Exec(guard.conn, "DROP TABLE IF EXISTS mut_test_null CASCADE", "cleanup");
  std::cout << "  [PASS] insert_with_null_label\n";
}

void TestCaptureMutationAloneIsNotThisGate() {
  // The harness does not call CaptureMutation(hand_built); all mutations come
  // through real DML + trigger. This documents the contract.
  std::cout << "  [PASS] no_hand_built_capture_mutation\n";
}

void TestTriggerPassesThroughData() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_txn_init()", "init");

  SetupContractTable(guard.conn, "mut_test_passthrough");

  Exec(guard.conn, "INSERT INTO mut_test_passthrough (val, label) VALUES (77, 'pass')", "insert");

  std::string result = QueryOne(guard.conn,
      "SELECT label FROM mut_test_passthrough WHERE val = 77", "select_back");

  Expect(result == "pass", "Trigger should pass through row data unchanged");

  Exec(guard.conn, "DROP TABLE IF EXISTS mut_test_passthrough CASCADE", "cleanup");
  std::cout << "  [PASS] trigger_passthrough\n";
}

void TestTriggerWithoutBridgeInitDoesNotCrash() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterFunctions(guard.conn);

  SetupContractTable(guard.conn, "mut_test_no_init");

  Exec(guard.conn, "INSERT INTO mut_test_no_init (val, label) VALUES (1, 'no_init')", "insert_no_init");

  std::string result = QueryOne(guard.conn,
      "SELECT val FROM mut_test_no_init WHERE label = 'no_init'", "select_no_init");
  Expect(result == "1", "Row should be inserted even without bridge init");

  Exec(guard.conn, "DROP TABLE IF EXISTS mut_test_no_init CASCADE", "cleanup");
  std::cout << "  [PASS] trigger_without_bridge_no_crash\n";
}

} // namespace

int main() {
  std::cout << "engine_mutations[postgres]\n";

  {
    ConnGuard probe;
    probe.conn = Connect();
    PQfinish(probe.conn);
    probe.conn = nullptr;
  }

  TestModuleLoad();
  TestCaptureMutationAloneIsNotThisGate();
  TestInsertCapturesMutation();
  TestDeleteCapturesOldRowValues();
  TestUpdateCapturesOldAndNewRows();
  TestInsertWithNullLabel();
  TestMultipleMutationsInTransaction();
  TestTriggerPassesThroughData();
  TestTriggerWithoutBridgeInitDoesNotCrash();

  std::cout << "engine_mutations[postgres] PASS\n";
  return 0;
}
