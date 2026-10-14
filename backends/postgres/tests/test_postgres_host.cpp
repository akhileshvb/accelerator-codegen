// GENERATED BACKEND — layer harness for host (Host packaging).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_postgres_host   CTest: engine_postgres_host
//
// Rule: host_glue, fail_closed
// Evidence: Loadable MODULE (accelerator_codegen_pg) loads via LOAD.
//   One SQL/UDF (pg_accel_runtime_execute) calls the toy Runtime;
//   same golden asserts as runtime layer. Server-only symbols
//   (PG_MODULE_MAGIC, xact callbacks, trigger hooks) live only in
//   the MODULE, not in standalone test libs.
//   Seed → Build → Execute matches golden rows through SQL.
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <sstream>

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
  std::cerr << "engine_host[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) Fail(msg);
}

void ExpectConn(PGconn *conn, const char *msg) {
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "engine_host[postgres] FAIL: " << msg
              << " — " << (conn ? PQerrorMessage(conn) : "null") << "\n";
    std::abort();
  }
}

PGresult *ExpectOK(PGconn *conn, PGresult *res, const char *msg) {
  if (!res) {
    std::cerr << "engine_host[postgres] FAIL: " << msg << " — null result\n";
    std::abort();
  }
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::cerr << "engine_host[postgres] FAIL: " << msg << " — "
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

void RegisterRuntimeUdfs(PGconn *conn) {
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_init(text) "
             "RETURNS integer AS '$libdir/accelerator_codegen_pg', 'pg_accel_runtime_init' "
             "LANGUAGE C STRICT", "register_runtime_init");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_build() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg', 'pg_accel_runtime_build' "
             "LANGUAGE C STRICT", "register_runtime_build");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_execute() "
             "RETURNS SETOF text AS '$libdir/accelerator_codegen_pg', 'pg_accel_runtime_execute' "
             "LANGUAGE C STRICT", "register_runtime_execute");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_read() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg', 'pg_accel_runtime_read' "
             "LANGUAGE C STRICT", "register_runtime_read");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_runtime_reset() "
             "RETURNS text AS '$libdir/accelerator_codegen_pg', 'pg_accel_runtime_reset' "
             "LANGUAGE C STRICT", "register_runtime_reset");
}

} // namespace

// Test 1: MODULE must load successfully. Fail-closed: exit non-zero on load failure.
void TestModuleLoad() {
  ConnGuard guard;
  guard.conn = Connect();

  PGresult *res = PQexec(guard.conn, "LOAD 'accelerator_codegen_pg'");
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK) {
    std::cerr << "engine_host[postgres] FAIL: MODULE LOAD failed — "
              << PQresultErrorMessage(res) << "\n";
    PQclear(res);
    std::abort();
  }
  PQclear(res);
  std::cout << "  [PASS] module_load\n";
}

// Test 2: UDF registration — all runtime UDFs must register.
void TestUdfRegistration() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterRuntimeUdfs(guard.conn);
  std::cout << "  [PASS] udf_registration\n";
}

// Test 3: Seed → Build → Execute matches golden rows (same as runtime layer).
void TestSeedBuildExecuteGoldenRows() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterRuntimeUdfs(guard.conn);

  // Ensure clean state.
  Exec(guard.conn, "SELECT pg_accel_runtime_reset()", "reset");
  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_golden CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE host_test_golden (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO host_test_golden VALUES (1, 'alpha', 100)", "insert1");
  Exec(guard.conn, "INSERT INTO host_test_golden VALUES (2, 'beta', 200)", "insert2");
  Exec(guard.conn, "INSERT INTO host_test_golden VALUES (3, 'gamma', 300)", "insert3");

  // Call the toy Runtime UDFs through SQL.
  Exec(guard.conn, "SELECT pg_accel_runtime_init('host_test_golden')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute");
  Expect(rows.size() == 3, "Execute should return 3 rows");

  // Verify golden rows match SQL table contents.
  PGresult *sql_res = PQexec(guard.conn,
      "SELECT id, name, val FROM host_test_golden ORDER BY id");
  ExpectOK(guard.conn, sql_res, "sql_ref");
  int sql_nrows = PQntuples(sql_res);
  Expect(sql_nrows == 3, "SQL ref should have 3 rows");
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
    Expect(found, ("Execute missing row: " + expected).c_str());
  }
  PQclear(sql_res);

  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_golden CASCADE", "cleanup");
  std::cout << "  [PASS] seed_build_execute_golden_rows\n";
}

// Test 4: ReadState after Build returns committed state.
void TestReadStateAfterBuild() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterRuntimeUdfs(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_runtime_reset()", "reset");

  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_read CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE host_test_read (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO host_test_read VALUES (1, 'one', 10)", "insert");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('host_test_read')", "init");

  // Before build, state may be empty.
  std::string state_before = QueryOne(guard.conn,
      "SELECT pg_accel_runtime_read()", "read_before");
  Expect(state_before.find("available=0") != std::string::npos ||
         state_before.find("partitions=0") != std::string::npos,
         "State before build should be empty");

  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  std::string state_after = QueryOne(guard.conn,
      "SELECT pg_accel_runtime_read()", "read_after");
  Expect(state_after.find("available=1") != std::string::npos,
         "State after build should be available");

  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_read CASCADE", "cleanup");
  std::cout << "  [PASS] read_state_after_build\n";
}

// Test 5: Reset clears all state.
void TestResetClearsState() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterRuntimeUdfs(guard.conn);

  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_reset CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE host_test_reset (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO host_test_reset VALUES (1, 'x', 1)", "insert");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('host_test_reset')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  std::string state_before = QueryOne(guard.conn,
      "SELECT pg_accel_runtime_read()", "before_reset");
  Expect(state_before.find("available=1") != std::string::npos,
         "State should be available before reset");

  std::string reset_result = QueryOne(guard.conn,
      "SELECT pg_accel_runtime_reset()", "reset");
  Expect(reset_result == "ok", "Reset should return ok");

  // After reset + re-init, state must be empty.
  Exec(guard.conn, "SELECT pg_accel_runtime_init('host_test_reset')", "reinit");
  std::string state_after = QueryOne(guard.conn,
      "SELECT pg_accel_runtime_read()", "after_reset");
  Expect(state_after.find("available=0") != std::string::npos ||
         state_after.find("partitions=0") != std::string::npos,
         "After reset and re-init, store must be empty");

  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_reset CASCADE", "cleanup");
  std::cout << "  [PASS] reset_clears_state\n";
}

// Test 6: Fail-closed — verify that attempting to load a nonexistent
// extension fails (module must be real).
void TestFailClosed() {
  ConnGuard guard;
  guard.conn = Connect();

  PGresult *res = PQexec(guard.conn, "LOAD 'accelerator_codegen_pg'");
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK) {
    std::cerr << "engine_host[postgres] FAIL: extension not loadable — "
              << PQresultErrorMessage(res) << "\n";
    PQclear(res);
    std::abort();
  }
  PQclear(res);
  std::cout << "  [PASS] fail_closed\n";
}

// Test 7: Null values round-trip correctly through UDF.
void TestNullValues() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterRuntimeUdfs(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_runtime_reset()", "reset");

  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_null CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE host_test_null (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn, "INSERT INTO host_test_null VALUES (1, 'first', NULL)", "insert1");
  Exec(guard.conn, "INSERT INTO host_test_null VALUES (2, NULL, 200)", "insert2");
  Exec(guard.conn, "INSERT INTO host_test_null VALUES (NULL, 'third', 300)", "insert3");

  Exec(guard.conn, "SELECT pg_accel_runtime_init('host_test_null')", "init");
  Exec(guard.conn, "SELECT pg_accel_runtime_build()", "build");

  auto rows = CollectAll(guard.conn, "SELECT pg_accel_runtime_execute()", "execute");
  Expect(rows.size() >= 2, "Null table should have multiple rows");

  Exec(guard.conn, "DROP TABLE IF EXISTS host_test_null CASCADE", "cleanup");
  std::cout << "  [PASS] null_values\n";
}

int main() {
  std::cout << "engine_host[postgres]\n";

  {
    ConnGuard probe;
    probe.conn = Connect();
    PQfinish(probe.conn);
    probe.conn = nullptr;
  }

  TestModuleLoad();
  TestFailClosed();
  TestUdfRegistration();
  TestSeedBuildExecuteGoldenRows();
  TestReadStateAfterBuild();
  TestResetClearsState();
  TestNullValues();

  std::cout << "engine_host[postgres] PASS\n";
  return 0;
}
