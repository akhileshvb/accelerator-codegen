// GENERATED BACKEND — layer harness for scan (Native scan helper).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_postgres_scan   CTest: engine_postgres_scan
//
// Rule: native_scan
// Evidence: ≥3 rows seeded, subset projection, one filter, exact cell values
//   including NULL. PG_MODULE UDF calls ScanNativeTable via table-AM.
//
// Rule: value_checked_scan
// Evidence: column order preserved, filter applied, NULL handled.
//
// Rule: fail_closed
// Evidence: MODULE LOAD fails → exit non-zero.
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
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
  std::cerr << "engine_scan[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

void ExpectConn(PGconn *conn, const char *msg) {
  if (!conn || PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "engine_scan[postgres] FAIL: " << msg
              << " — " << (conn ? PQerrorMessage(conn) : "null") << "\n";
    std::abort();
  }
}

PGresult *ExpectOK(PGconn *conn, PGresult *res, const char *msg) {
  if (!res) {
    std::cerr << "engine_scan[postgres] FAIL: " << msg << " — null result\n";
    std::abort();
  }
  auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    std::cerr << "engine_scan[postgres] FAIL: " << msg << " — "
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
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_scan_init() "
             "RETURNS integer AS '$libdir/accelerator_codegen_pg', 'pg_accel_scan_init' "
             "LANGUAGE C STRICT", "register_init");
  Exec(conn, "CREATE OR REPLACE FUNCTION pg_accel_scan_rows(text, text, text, text) "
             "RETURNS SETOF text AS '$libdir/accelerator_codegen_pg', 'pg_accel_scan_rows' "
             "LANGUAGE C STRICT", "register_scan_rows");
}

void TestModuleLoad() {
  ConnGuard guard;
  guard.conn = Connect();
  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  std::cout << "  [PASS] module_load\n";
}

void TestScanThreeRowsFullProjection() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_three CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE scan_test_three (id INTEGER, name TEXT, val INTEGER)",
       "create");
  Exec(guard.conn,
       "INSERT INTO scan_test_three VALUES (1, 'alpha', 100)",
       "insert1");
  Exec(guard.conn,
       "INSERT INTO scan_test_three VALUES (2, 'beta', 200)",
       "insert2");
  Exec(guard.conn,
       "INSERT INTO scan_test_three VALUES (3, 'gamma', 300)",
       "insert3");

  // Scan with full projection, no filter
  // UDF: pg_accel_scan_rows(table, columns, filter_col, filter_val)
  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('scan_test_three', 'id,name,val', '', '')");
  ExpectOK(guard.conn, res, "scan_full");

  int nrows = PQntuples(res);
  Expect(nrows == 3, "Should have 3 rows");

  // Collect rows to verify
  std::vector<std::string> rows;
  for (int i = 0; i < nrows; ++i) {
    const char *val = PQgetvalue(res, i, 0);
    if (val) {
      rows.push_back(std::string(val));
    }
  }
  PQclear(res);

  Expect(rows.size() == 3, "Should have 3 row strings");

  // Row format: id=1,name=alpha,val=100
  Expect(rows[0].find("id=1") != std::string::npos, "Row 0 id should be 1");
  Expect(rows[0].find("name=alpha") != std::string::npos, "Row 0 name should be alpha");
  Expect(rows[0].find("val=100") != std::string::npos, "Row 0 val should be 100");

  Expect(rows[1].find("id=2") != std::string::npos, "Row 1 id should be 2");
  Expect(rows[1].find("name=beta") != std::string::npos, "Row 1 name should be beta");

  Expect(rows[2].find("id=3") != std::string::npos, "Row 2 id should be 3");
  Expect(rows[2].find("name=gamma") != std::string::npos, "Row 2 name should be gamma");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_three CASCADE", "cleanup");
  std::cout << "  [PASS] scan_three_rows_full_projection\n";
}

void TestScanNullHandling() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_null CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE scan_test_null (id INTEGER, label TEXT, score INTEGER)",
       "create");
  Exec(guard.conn,
       "INSERT INTO scan_test_null VALUES (1, 'first', NULL)",
       "insert1");
  Exec(guard.conn,
       "INSERT INTO scan_test_null VALUES (2, NULL, 500)",
       "insert2");
  Exec(guard.conn,
       "INSERT INTO scan_test_null VALUES (3, 'third', 700)",
       "insert3");

  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('scan_test_null', 'id,label,score', '', '')");
  ExpectOK(guard.conn, res, "scan_null");

  int nrows = PQntuples(res);
  Expect(nrows == 3, "Should have 3 rows");

  std::vector<std::string> rows;
  for (int i = 0; i < nrows; ++i) {
    const char *val = PQgetvalue(res, i, 0);
    if (val) {
      rows.push_back(std::string(val));
    }
  }
  PQclear(res);

  // Row 0: id=1,label=first,score=NULL
  Expect(rows[0].find("id=1") != std::string::npos, "Row 0 id=1");
  Expect(rows[0].find("label=first") != std::string::npos, "Row 0 label=first");
  Expect(rows[0].find("score=NULL") != std::string::npos, "Row 0 score=NULL");

  // Row 1: id=2,label=NULL,score=500
  Expect(rows[1].find("id=2") != std::string::npos, "Row 1 id=2");
  Expect(rows[1].find("label=NULL") != std::string::npos, "Row 1 label=NULL");
  Expect(rows[1].find("score=500") != std::string::npos, "Row 1 score=500");

  // Row 2: id=3,label=third,score=700
  Expect(rows[2].find("id=3") != std::string::npos, "Row 2 id=3");
  Expect(rows[2].find("label=third") != std::string::npos, "Row 2 label=third");
  Expect(rows[2].find("score=700") != std::string::npos, "Row 2 score=700");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_null CASCADE", "cleanup");
  std::cout << "  [PASS] scan_null_handling\n";
}

void TestScanSubsetProjection() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_subset CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE scan_test_subset (a INTEGER, b TEXT, c INTEGER, d TEXT)",
       "create");
  Exec(guard.conn,
       "INSERT INTO scan_test_subset VALUES (10, 'x', 100, 'xx')",
       "insert1");
  Exec(guard.conn,
       "INSERT INTO scan_test_subset VALUES (20, 'y', 200, 'yy')",
       "insert2");
  Exec(guard.conn,
       "INSERT INTO scan_test_subset VALUES (30, 'z', 300, 'zz')",
       "insert3");

  // Only project a,c (subset)
  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('scan_test_subset', 'a,c', '', '')");
  ExpectOK(guard.conn, res, "scan_subset");

  int nrows = PQntuples(res);
  Expect(nrows == 3, "Should have 3 rows");

  std::vector<std::string> rows;
  for (int i = 0; i < nrows; ++i) {
    const char *val = PQgetvalue(res, i, 0);
    if (val) {
      rows.push_back(std::string(val));
    }
  }
  PQclear(res);

  // Should only have a and c, not b or d
  Expect(rows[0].find("a=10") != std::string::npos, "Row 0 a=10");
  Expect(rows[0].find("c=100") != std::string::npos, "Row 0 c=100");
  Expect(rows[0].find("b=") == std::string::npos, "Row 0 should NOT have b");
  Expect(rows[0].find("d=") == std::string::npos, "Row 0 should NOT have d");

  Expect(rows[1].find("a=20") != std::string::npos, "Row 1 a=20");
  Expect(rows[1].find("c=200") != std::string::npos, "Row 1 c=200");

  Expect(rows[2].find("a=30") != std::string::npos, "Row 2 a=30");
  Expect(rows[2].find("c=300") != std::string::npos, "Row 2 c=300");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_subset CASCADE", "cleanup");
  std::cout << "  [PASS] scan_subset_projection\n";
}

void TestScanWithFilter() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_filter CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE scan_test_filter (id INTEGER, tag TEXT, value INTEGER)",
       "create");
  Exec(guard.conn,
       "INSERT INTO scan_test_filter VALUES (1, 'apple', 10)",
       "insert1");
  Exec(guard.conn,
       "INSERT INTO scan_test_filter VALUES (2, 'banana', 20)",
       "insert2");
  Exec(guard.conn,
       "INSERT INTO scan_test_filter VALUES (3, 'apple', 30)",
       "insert3");
  Exec(guard.conn,
       "INSERT INTO scan_test_filter VALUES (4, 'cherry', 40)",
       "insert4");

  // Filter: tag = 'apple' — should get rows 1 and 3
  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('scan_test_filter', 'id,tag,value', 'tag', 'apple')");
  ExpectOK(guard.conn, res, "scan_filter");

  int nrows = PQntuples(res);
  Expect(nrows == 2, "Should have 2 rows matching tag='apple'");

  std::vector<std::string> rows;
  for (int i = 0; i < nrows; ++i) {
    const char *val = PQgetvalue(res, i, 0);
    if (val) {
      rows.push_back(std::string(val));
    }
  }
  PQclear(res);

  Expect(rows[0].find("id=1") != std::string::npos, "Row 0 id=1");
  Expect(rows[0].find("tag=apple") != std::string::npos, "Row 0 tag=apple");
  Expect(rows[0].find("value=10") != std::string::npos, "Row 0 value=10");

  Expect(rows[1].find("id=3") != std::string::npos, "Row 1 id=3");
  Expect(rows[1].find("tag=apple") != std::string::npos, "Row 1 tag=apple");
  Expect(rows[1].find("value=30") != std::string::npos, "Row 1 value=30");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_filter CASCADE", "cleanup");
  std::cout << "  [PASS] scan_with_filter\n";
}

void TestScanFilterMatchNone() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_nomatch CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE scan_test_nomatch (id INTEGER, tag TEXT)",
       "create");
  Exec(guard.conn,
       "INSERT INTO scan_test_nomatch VALUES (1, 'hello')",
       "insert1");
  Exec(guard.conn,
       "INSERT INTO scan_test_nomatch VALUES (2, 'world')",
       "insert2");

  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('scan_test_nomatch', 'id,tag', 'tag', 'nonexistent')");
  ExpectOK(guard.conn, res, "scan_nomatch");

  int nrows = PQntuples(res);
  Expect(nrows == 0, "Should have 0 rows when filter matches nothing");

  PQclear(res);

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_nomatch CASCADE", "cleanup");
  std::cout << "  [PASS] scan_filter_match_none\n";
}

void TestScanFilterOnIntegerColumn() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_intfilt CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE scan_test_intfilt (id INTEGER, val INTEGER)",
       "create");
  Exec(guard.conn,
       "INSERT INTO scan_test_intfilt VALUES (1, 100)",
       "insert1");
  Exec(guard.conn,
       "INSERT INTO scan_test_intfilt VALUES (2, 200)",
       "insert2");
  Exec(guard.conn,
       "INSERT INTO scan_test_intfilt VALUES (3, 100)",
       "insert3");

  // Filter: val = 100
  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('scan_test_intfilt', 'id,val', 'val', '100')");
  ExpectOK(guard.conn, res, "scan_intfilt");

  int nrows = PQntuples(res);
  Expect(nrows == 2, "Should have 2 rows with val=100");

  std::vector<std::string> rows;
  for (int i = 0; i < nrows; ++i) {
    const char *val = PQgetvalue(res, i, 0);
    if (val) {
      rows.push_back(std::string(val));
    }
  }
  PQclear(res);

  Expect(rows[0].find("id=1") != std::string::npos, "Row 0 id=1");
  Expect(rows[0].find("val=100") != std::string::npos, "Row 0 val=100");

  Expect(rows[1].find("id=3") != std::string::npos, "Row 1 id=3");
  Expect(rows[1].find("val=100") != std::string::npos, "Row 1 val=100");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_intfilt CASCADE", "cleanup");
  std::cout << "  [PASS] scan_filter_integer_column\n";
}

void TestScanNonExistentTable() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('no_such_table_xyz', 'id', '', '')");
  ExpectOK(guard.conn, res, "scan_missing_table");

  int nrows = PQntuples(res);
  Expect(nrows == 0, "Should return 0 rows for non-existent table");

  PQclear(res);
  std::cout << "  [PASS] scan_non_existent_table\n";
}

void TestScanColumnOrderPreserved() {
  ConnGuard guard;
  guard.conn = Connect();

  Exec(guard.conn, "LOAD 'accelerator_codegen_pg'", "load");
  RegisterFunctions(guard.conn);
  Exec(guard.conn, "SELECT pg_accel_scan_init()", "init");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_order CASCADE", "cleanup");
  Exec(guard.conn,
       "CREATE TABLE scan_test_order (first_col INTEGER, second_col TEXT, third_col INTEGER)",
       "create");
  Exec(guard.conn,
       "INSERT INTO scan_test_order VALUES (1, 'a', 10)",
       "insert1");
  Exec(guard.conn,
       "INSERT INTO scan_test_order VALUES (2, 'b', 20)",
       "insert2");
  Exec(guard.conn,
       "INSERT INTO scan_test_order VALUES (3, 'c', 30)",
       "insert3");

  // Project in reversed order
  PGresult *res = PQexec(guard.conn,
      "SELECT pg_accel_scan_rows('scan_test_order', 'third_col,first_col', '', '')");
  ExpectOK(guard.conn, res, "scan_order");

  int nrows = PQntuples(res);
  Expect(nrows == 3, "Should have 3 rows");

  std::string row0 = std::string(PQgetvalue(res, 0, 0));
  PQclear(res);

  // Order should be as requested: third_col, then first_col
  auto pos_third = row0.find("third_col=");
  auto pos_first = row0.find("first_col=");
  Expect(pos_third != std::string::npos, "Should have third_col");
  Expect(pos_first != std::string::npos, "Should have first_col");
  Expect(pos_third < pos_first, "third_col should appear before first_col in output");

  Exec(guard.conn, "DROP TABLE IF EXISTS scan_test_order CASCADE", "cleanup");
  std::cout << "  [PASS] scan_column_order_preserved\n";
}

} // namespace

int main() {
  std::cout << "engine_scan[postgres]\n";

  {
    ConnGuard probe;
    probe.conn = Connect();
    PQfinish(probe.conn);
    probe.conn = nullptr;
  }

  TestModuleLoad();
  TestScanThreeRowsFullProjection();
  TestScanNullHandling();
  TestScanSubsetProjection();
  TestScanWithFilter();
  TestScanFilterMatchNone();
  TestScanFilterOnIntegerColumn();
  TestScanNonExistentTable();
  TestScanColumnOrderPreserved();

  std::cout << "engine_scan[postgres] PASS\n";
  return 0;
}
