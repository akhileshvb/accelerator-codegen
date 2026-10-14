// GENERATED BACKEND — layer harness for runtime (Toy Runtime).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_postgres_runtime   CTest: engine_postgres_runtime
//
// Rule: runtime
// Evidence: Toy Runtime = store + coordinator + backend + named scan + ToyModule.
//   Build scans via named helper then publishes through coordinator.
//   Execute reads the store with CurrentVisibility().
//   Seed → Build → Execute matches golden rows.
//   DML from mutations layer → commit → Execute rematch.
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
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
  std::cerr << "engine_runtime[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) Fail(msg);
}

void ExpectConn(PGconn *conn, const char *msg) {
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "engine_runtime[postgres] FAIL: " << msg
              << " — " << (conn ? PQerrorMessage(conn) : "null") << "\n";
    std::abort();
  }
}

PGresult *ExpectOK(PGconn *conn, PGresult *res, const char *msg) {
  if (!res) {
    std::cerr << "engine_runtime[postgres] FAIL: " << msg << " — null result\n";
    std::abort();
  }
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::cerr << "engine_runtime[postgres] FAIL: " << msg << " — "
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

void RegisterRuntimeFunctions(PGconn *conn) {
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_init(text) "
             "RETURNS integer AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_init' "
             "LANGUAGE C STRICT", "register_runtime_init");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_build() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_build' "
             "LANGUAGE C STRICT", "register_runtime_build");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_execute() "
             "RETURNS SETOF text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_execute' "
             "LANGUAGE C STRICT", "register_runtime_execute");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_read() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_read' "
             "LANGUAGE C STRICT", "register_runtime_read");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_reset() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg_test', 'pg_accel_runtime_reset' "
             "LANGUAGE C STRICT", "register_runtime_reset");
}

std::vector<std::string> CollectAll(PGconn *conn, const char *sql, const char *label) {
  PGresult *res = PQexec(conn, sql);
  ExpectOK(conn, res, label);
  std::vector<std::string> rows;
  for (int i = 0; i < PQntuples(res); ++i) {
    for (int j = 0; j < PQnfields(res); ++j) {
      const char *val = PQgetvalue(res, i, j);
      rows.push_back(val ? val : "NULL");
    }
  }
  PQclear(res);
  return rows;
}

void TestModuleLoad() {
  ConnGuard guard;
  guard.conn = Connect();
  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  std::cout << "  [PASS] module_load\n";
}

void TestSeedBuildExecuteGoldenRows() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_golden CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_golden (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO rt_test_golden VALUES (1, 'alpha', 100)", "insert1");
  Exec(guard.conn, "INSERT INTO rt_test_golden VALUES (2, 'beta', 200)", "insert2");
  Exec(guard.conn, "INSERT INTO rt_test_golden VALUES (3, 'gamma', 300)", "insert3");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_golden')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute");
  Expect(rows.size() == 3, "Should have 3 rows");

  std::sort(rows.begin(), rows.end());

  bool has_1 = false, has_2 = false, has_3 = false;
  for (const auto &r : rows) {
    if (r == "1,alpha,100") has_1 = true;
    if (r == "2,beta,200") has_2 = true;
    if (r == "3,gamma,300") has_3 = true;
  }

  Expect(has_1, "Missing row: 1,alpha,100");
  Expect(has_2, "Missing row: 2,beta,200");
  Expect(has_3, "Missing row: 3,gamma,300");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_golden CASCADE", "cleanup");
  std::cout << "  [PASS] seed_build_execute_golden_rows\n";
}

void TestReadStateAfterBuild() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_readstate CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_readstate (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO rt_test_readstate VALUES (1, 'hello', 42)", "insert");
  Exec(guard.conn, "INSERT INTO rt_test_readstate VALUES (2, 'world', 84)", "insert2");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_readstate')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  std::string state = QueryOne(guard.conn, "SELECT pg_accel_runtime_read()", "read");
  Expect(state.find("available=1") != std::string::npos, "State should be available");
  Expect(state.find("partitions=2") != std::string::npos, "Should have 2 partitions");
  Expect(state.find("id=1") != std::string::npos, "Should have id=1");
  Expect(state.find("name=hello") != std::string::npos, "Should have name=hello");
  Expect(state.find("val=42") != std::string::npos, "Should have val=42");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_readstate CASCADE", "cleanup");
  std::cout << "  [PASS] read_state_after_build\n";
}

void TestResetClearsState() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_reset CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_reset (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO rt_test_reset VALUES (1, 'test', 99)", "insert");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_reset')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  std::string state = QueryOne(guard.conn, "SELECT pg_accel_runtime_read()", "read");
  Expect(state.find("available=1") != std::string::npos, "Should be available before reset");

  Exec(guard.conn, "SELECT pg_accel_runtime_reset()", "reset");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_reset')", "init2");
  state = QueryOne(guard.conn, "SELECT pg_accel_runtime_read()", "read2");
  Expect(state.find("available=1") == std::string::npos || state.find("partitions=0") != std::string::npos,
         "Should be empty after reset and re-init");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_reset CASCADE", "cleanup");
  std::cout << "  [PASS] reset_clears_state\n";
}

void TestNullValues() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_null CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_null (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO rt_test_null VALUES (1, NULL, 100)", "insert1");
  Exec(guard.conn, "INSERT INTO rt_test_null VALUES (2, 'present', NULL)", "insert2");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_null')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute_null");
  Expect(rows.size() == 2, "Should have 2 rows");

  bool found_null_name = false;
  bool found_null_val = false;
  for (const auto &row : rows) {
    if (row.find("1,") == 0 && row.find("NULL") != std::string::npos &&
        row.find(",100") != std::string::npos) found_null_name = true;
    if (row.find("2,") == 0 && row.find("NULL") != std::string::npos) found_null_val = true;
  }
  Expect(found_null_name, "Row with NULL name should be present");
  Expect(found_null_val, "Row with NULL val should be present");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_null CASCADE", "cleanup");
  std::cout << "  [PASS] null_values\n";
}

void TestSingleRowTable() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_single CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_single (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO rt_test_single VALUES (42, 'only', 777)", "insert");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_single')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute_single");
  Expect(rows.size() == 1, "Should have 1 row");
  Expect(rows[0] == "42,only,777", "Single row should match exactly");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_single CASCADE", "cleanup");
  std::cout << "  [PASS] single_row_table\n";
}

void TestInitWithoutTable() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "SELECT pg_accel_runtime_init('nonexistent_table_xyz')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute_empty");
  Expect(rows.empty(), "Should return 0 rows for non-existent table");

  std::cout << "  [PASS] init_without_table\n";
}

void TestRuntimeUsesCoordinatorNotStoreBackdoor() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_coord CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_coord (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO rt_test_coord VALUES (10, 'coord_test', 50)", "insert");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_coord')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  std::string state = QueryOne(guard.conn, "SELECT pg_accel_runtime_read()", "read");
  Expect(state.find("available=1") != std::string::npos, "State should be available");
  Expect(state.find("runtime_toy") != std::string::npos, "Accelerator should be runtime_toy");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_coord CASCADE", "cleanup");
  std::cout << "  [PASS] runtime_uses_coordinator_not_store_backdoor\n";
}

void TestBuildThenExecuteMatchesGoldenRows() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_match CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_match (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO rt_test_match VALUES (100, 'hundred', 1000)", "insert1");
  Exec(guard.conn, "INSERT INTO rt_test_match VALUES (200, 'two_hundred', 2000)", "insert2");
  Exec(guard.conn, "INSERT INTO rt_test_match VALUES (300, 'three_hundred', 3000)", "insert3");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_match')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute");
  Expect(rows.size() == 3, "Should have 3 rows");

  PGresult *sql_res = PQexec(guard.conn, "SELECT id, name, val FROM rt_test_match ORDER BY id");
  ExpectOK(guard.conn, sql_res, "sql_query");

  int sql_nrows = PQntuples(sql_res);
  Expect(sql_nrows == 3, "SQL query should have 3 rows");

  for (int i = 0; i < sql_nrows; ++i) {
    std::string expected;
    for (int j = 0; j < PQnfields(sql_res); ++j) {
      if (j > 0) expected += ",";
      const char *v = PQgetvalue(sql_res, i, j);
      expected += v ? v : "NULL";
    }
    bool found = false;
    for (const auto &r : rows) {
      if (r == expected) { found = true; break; }
    }
    Expect(found, ("Runtime execute missing row: " + expected).c_str());
  }
  PQclear(sql_res);

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_match CASCADE", "cleanup");
  std::cout << "  [PASS] build_then_execute_matches_golden_rows\n";
}

} // namespace


void TestDmlCommitRematch() {
  // DML + commit + rematch: after DML changes are committed to the base table,
  // RuntimeBuild rescans through the coordinator path and RuntimeExecute
  // returns updated golden rows matching the current SQL data.
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg_test'", "load");
  RegisterRuntimeFunctions(guard.conn);

  // Ensure clean bridge state before this test.
  Exec(guard.conn, "SELECT pg_accel_runtime_reset()", "reset_before_dml");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_dml CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE rt_test_dml (id INTEGER, name TEXT, val INTEGER)",
       "create");

  // Seed initial data.
  Exec(guard.conn, "INSERT INTO rt_test_dml VALUES (1, 'alpha', 100)", "seed1");
  Exec(guard.conn, "INSERT INTO rt_test_dml VALUES (2, 'beta', 200)", "seed2");
  Exec(guard.conn, "INSERT INTO rt_test_dml VALUES (3, 'gamma', 300)", "seed3");

  // Build and verify initial state.
  Exec(guard.conn, "SELECT pg_accel_runtime_init('rt_test_dml')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build_initial");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute_initial");
  Expect(rows.size() == 3, "Initial should have 3 rows");

  // Verify golden rows match SQL using PQexec directly (CollectAll flattens all cells).
  PGresult *sql_res = PQexec(guard.conn, "SELECT id, name, val FROM rt_test_dml ORDER BY id");
  ExpectOK(guard.conn, sql_res, "sql_initial");
  int sql_nrows = PQntuples(sql_res);
  Expect(sql_nrows == 3, "SQL initial should have 3 rows");
  for (int i = 0; i < sql_nrows; ++i) {
    std::string expected;
    for (int j = 0; j < PQnfields(sql_res); ++j) {
      if (j > 0) expected += ",";
      const char *v = PQgetvalue(sql_res, i, j);
      expected += v ? v : "NULL";
    }
    bool found = false;
    for (const auto &r : rows) {
      if (r == expected) { found = true; break; }
    }
    Expect(found, ("Initial execute missing row: " + expected).c_str());
  }
  PQclear(sql_res);

  // Perform DML: INSERT, UPDATE, DELETE.
  Exec(guard.conn, "INSERT INTO rt_test_dml VALUES (4, 'delta', 400)", "dml_insert");
  Exec(guard.conn, "UPDATE rt_test_dml SET val = 250 WHERE id = 2", "dml_update");
  Exec(guard.conn, "DELETE FROM rt_test_dml WHERE id = 3", "dml_delete");

  // Rebuild after DML to rescan the base table through the coordinator path.
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build_after_dml");

  // Verify updated state matches current table contents.
  auto rows_after = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute_after_dml");

  PGresult *sql_after_res = PQexec(guard.conn, "SELECT id, name, val FROM rt_test_dml ORDER BY id");
  ExpectOK(guard.conn, sql_after_res, "sql_after_dml");
  int sql_after_nrows = PQntuples(sql_after_res);
  Expect(sql_after_nrows == (int)rows_after.size(), "Row count after DML should match SQL");
  for (int i = 0; i < sql_after_nrows; ++i) {
    std::string expected;
    for (int j = 0; j < PQnfields(sql_after_res); ++j) {
      if (j > 0) expected += ",";
      const char *v = PQgetvalue(sql_after_res, i, j);
      expected += v ? v : "NULL";
    }
    bool found = false;
    for (const auto &r : rows_after) {
      if (r == expected) { found = true; break; }
    }
    Expect(found, ("After DML execute missing row: " + expected).c_str());
  }
  PQclear(sql_after_res);

  // Verify specific expected values.
  Expect(std::find(rows_after.begin(), rows_after.end(), "1,alpha,100") != rows_after.end(),
         "Row 1 should still be alpha,100");
  Expect(std::find(rows_after.begin(), rows_after.end(), "2,beta,250") != rows_after.end(),
         "Row 2 should be beta,250 after UPDATE");
  Expect(std::find(rows_after.begin(), rows_after.end(), "3,gamma,300") == rows_after.end(),
         "Row 3 should be gone after DELETE");
  Expect(std::find(rows_after.begin(), rows_after.end(), "4,delta,400") != rows_after.end(),
         "Row 4 should be present after INSERT");

  Exec(guard.conn, "DROP TABLE IF EXISTS rt_test_dml CASCADE", "cleanup");
  std::cout << "  [PASS] dml_commit_rematch" << std::endl;
}

int main() {
  std::cout << "engine_runtime[postgres]\n";

  {
    ConnGuard probe;
    probe.conn = Connect();
    PQfinish(probe.conn);
    probe.conn = nullptr;
  }

  TestModuleLoad();
  TestSeedBuildExecuteGoldenRows();
  TestReadStateAfterBuild();
  TestResetClearsState();
  TestNullValues();
  TestSingleRowTable();
  TestInitWithoutTable();
  TestRuntimeUsesCoordinatorNotStoreBackdoor();
  TestBuildThenExecuteMatchesGoldenRows();
  TestDmlCommitRematch();

  std::cout << "engine_runtime[postgres] PASS\n";
  return 0;
}
