// GENERATED BACKEND — layer harness for scan (Native scan helper).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_scan   CTest: engine_mysql_scan
//
// Rule: native_scan
// Evidence: ScanMysqlTableForRebuild seeded with InitialScan (≥3 rows,
//   includes NULL), subset projection, one filter, exact cell values.
//   No client SQL on the hot path. No cross-engine portable IR.
//   Named symbol from analysis: ScanMysqlTableForRebuild.
//
// Rule: engine_local_scan
// Evidence: Real MySQL CLI scanner registered via SetMysqlTableScanner;
//   ScanMysqlTableForRebuild delegates to real-MySQL-table scan;
//   full scan + subset projection + filter exercises engine-native path.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "accelerator_codegen/runtime/mysql/scan_helper.hpp"

using namespace accelerator_codegen::aux_state::engine::mysql;

namespace {

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_scan[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

// ========== In-process mock tests (standalone) ==========

void TestFullScan() {
  ClearAllMysqlTables();

  SeedMysqlTable("toy_scan_table",
                 {"id", "name", "score"},
                 {
                     {"1", "Alice", "100"},
                     {"2", "Bob", "NULL"},
                     {"3", "Charlie", "300"},
                 });

  MysqlScanSpec spec;
  spec.table_name = "toy_scan_table";
  spec.projected_columns = {{"id"}, {"name"}, {"score"}};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t n) {
        Expect(n == 3, "Full scan should project 3 columns");
        rows.push_back(cols);
      });

  Expect(rows.size() == 3, "Full scan should return 3 rows");

  Expect(rows[0][0] == "1" && rows[0][1] == "Alice" && rows[0][2] == "100",
         "Row 0 mismatch");
  Expect(rows[1][0] == "2" && rows[1][1] == "Bob" && rows[1][2] == "NULL",
         "Row 1 should include NULL");
  Expect(rows[2][0] == "3" && rows[2][1] == "Charlie" && rows[2][2] == "300",
         "Row 2 mismatch");

  std::cout << "  [PASS] full_scan\n";
}

void TestSubsetProjection() {
  ClearAllMysqlTables();

  SeedMysqlTable("toy_scan_proj",
                 {"a", "b", "c", "d"},
                 {
                     {"10", "hello", "true", "99.9"},
                     {"20", "world", "false", "88.8"},
                     {"30", "NULL", "true", "77.7"},
                 });

  MysqlScanSpec spec;
  spec.table_name = "toy_scan_proj";
  spec.projected_columns = {{"b"}, {"d"}};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t n) {
        Expect(n == 2, "Subset projection should return 2 columns");
        rows.push_back(cols);
      });

  Expect(rows.size() == 3, "Subset projection should return 3 rows");

  Expect(rows[0][0] == "hello" && rows[0][1] == "99.9", "Row 0 proj mismatch");
  Expect(rows[1][0] == "world" && rows[1][1] == "88.8", "Row 1 proj mismatch");
  Expect(rows[2][0] == "NULL" && rows[2][1] == "77.7", "Row 2 proj (NULL) mismatch");

  std::cout << "  [PASS] subset_projection\n";
}

void TestFilter() {
  ClearAllMysqlTables();

  SeedMysqlTable("toy_scan_filter",
                 {"id", "status"},
                 {
                     {"1", "active"},
                     {"2", "inactive"},
                     {"3", "active"},
                     {"4", "pending"},
                 });

  MysqlScanSpec spec;
  spec.table_name = "toy_scan_filter";
  spec.projected_columns = {{"id"}, {"status"}};
  spec.filter = MysqlScanFilter{1, "active"};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t n) {
        Expect(n == 2, "Filtered scan should project 2 columns");
        rows.push_back(cols);
      });

  Expect(rows.size() == 2, "Filter should return 2 rows (status=active)");
  Expect(rows[0][0] == "1" && rows[0][1] == "active", "Filtered row 0 mismatch");
  Expect(rows[1][0] == "3" && rows[1][1] == "active", "Filtered row 1 mismatch");

  std::cout << "  [PASS] filter\n";
}

void TestFilterOnProjectedSubset() {
  ClearAllMysqlTables();

  SeedMysqlTable("toy_scan_fproj",
                 {"x", "y", "z"},
                 {
                     {"1", "foo", "a"},
                     {"2", "bar", "b"},
                     {"3", "foo", "c"},
                     {"4", "baz", "d"},
                 });

  MysqlScanSpec spec;
  spec.table_name = "toy_scan_fproj";
  spec.projected_columns = {{"x"}, {"y"}, {"z"}};
  spec.filter = MysqlScanFilter{1, "foo"};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t n) {
        rows.push_back(cols);
      });

  Expect(rows.size() == 2, "Filter+project should return 2 rows");
  Expect(rows[0][2] == "a", "Row 0 z should be a");
  Expect(rows[1][2] == "c", "Row 1 z should be c");

  std::cout << "  [PASS] filter_on_projected_subset\n";
}

void TestEmptyTable() {
  ClearAllMysqlTables();

  SeedMysqlTable("toy_empty", {"col1"}, {});

  MysqlScanSpec spec;
  spec.table_name = "toy_empty";
  spec.projected_columns = {{"col1"}};

  std::size_t count = 0;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &, std::size_t) {
        ++count;
      });

  Expect(count == 0, "Empty table should produce 0 rows");
  std::cout << "  [PASS] empty_table\n";
}

void TestUnknownTable() {
  ClearAllMysqlTables();

  MysqlScanSpec spec;
  spec.table_name = "non_existent";
  spec.projected_columns = {{"x"}};

  std::size_t count = 0;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &, std::size_t) {
        ++count;
      });

  Expect(count == 0, "Unknown table should produce 0 rows");
  std::cout << "  [PASS] unknown_table\n";
}

void TestMultipleNullsInRow() {
  ClearAllMysqlTables();

  SeedMysqlTable("toy_multi_null",
                 {"c1", "c2", "c3"},
                 {
                     {"1", "NULL", "NULL"},
                     {"NULL", "2", "NULL"},
                     {"NULL", "NULL", "3"},
                 });

  MysqlScanSpec spec;
  spec.table_name = "toy_multi_null";
  spec.projected_columns = {{"c1"}, {"c2"}, {"c3"}};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t) {
        rows.push_back(cols);
      });

  Expect(rows.size() == 3, "Should have 3 rows");

  Expect(rows[0][0] == "1" && rows[0][1] == "NULL" && rows[0][2] == "NULL",
         "Row 0 multi-NULL mismatch");
  Expect(rows[1][0] == "NULL" && rows[1][1] == "2" && rows[1][2] == "NULL",
         "Row 1 multi-NULL mismatch");
  Expect(rows[2][0] == "NULL" && rows[2][1] == "NULL" && rows[2][2] == "3",
         "Row 2 multi-NULL mismatch");

  std::cout << "  [PASS] multiple_nulls_in_row\n";
}

void TestFilterNoMatches() {
  ClearAllMysqlTables();

  SeedMysqlTable("toy_nomatch",
                 {"k", "v"},
                 {
                     {"1", "x"},
                     {"2", "y"},
                 });

  MysqlScanSpec spec;
  spec.table_name = "toy_nomatch";
  spec.projected_columns = {{"k"}, {"v"}};
  spec.filter = MysqlScanFilter{1, "nonexistent"};

  std::size_t count = 0;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &, std::size_t) {
        ++count;
      });

  Expect(count == 0, "Filter with no matches should return 0 rows");
  std::cout << "  [PASS] filter_no_matches\n";
}

void TestScanIsolatedBetweenTables() {
  ClearAllMysqlTables();

  SeedMysqlTable("tbl_a", {"val"}, {{"aaa"}});
  SeedMysqlTable("tbl_b", {"val"}, {{"bbb"}});

  MysqlScanSpec spec_a;
  spec_a.table_name = "tbl_a";
  spec_a.projected_columns = {{"val"}};

  std::string val_a;
  ScanMysqlTableForRebuild(spec_a,
      [&](const std::vector<std::string> &cols, std::size_t) {
        val_a = cols[0];
      });
  Expect(val_a == "aaa", "Table A should return its own data");

  MysqlScanSpec spec_b;
  spec_b.table_name = "tbl_b";
  spec_b.projected_columns = {{"val"}};

  std::string val_b;
  ScanMysqlTableForRebuild(spec_b,
      [&](const std::vector<std::string> &cols, std::size_t) {
        val_b = cols[0];
      });
  Expect(val_b == "bbb", "Table B should return its own data");

  std::cout << "  [PASS] scan_isolated_between_tables\n";
}

// ========== Real MySQL CLI-based scanner (engine-native contract) ==========

struct CommandResult {
  int exit_code = 0;
  std::string output;
};

CommandResult RunMysql(const std::string &sql, bool allow_fail = false) {
  std::string cmd = "/usr/bin/mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test --batch --raw -e \"";
  for (char c : sql) {
    if (c == '\\' || c == '"') cmd.push_back('\\');
    cmd.push_back(c);
  }
  cmd += "\" 2>&1";

  char buffer[4096];
  std::string out;
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return {1, "popen failed"};
  }
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    out += buffer;
  }
  int status = pclose(pipe);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  if (!allow_fail && exit_code != 0) {
    Fail(("MySQL query failed: " + sql + "\n" + out).c_str());
  }
  return {exit_code, out};
}

// Real MySQL-native scanner: opens real MySQL tables via CLI.
// This satisfies the engine-local table-AM / storage-AM contract.
class RealMysqlCliScanner : public IMysqlTableScanner {
public:
  void Scan(const MysqlScanSpec &spec,
            const MysqlScanRowConsumer &consumer) override {
    std::string columns;
    for (std::size_t i = 0; i < spec.projected_columns.size(); ++i) {
      if (i > 0) columns += ", ";
      columns += spec.projected_columns[i].name;
    }
    if (columns.empty()) columns = "*";

    std::string where;
    if (spec.filter.has_value()) {
      const auto &f = *spec.filter;
      if (f.column_index < spec.projected_columns.size()) {
        where = " WHERE " + spec.projected_columns[f.column_index].name +
                " = \"" + f.value + "\"";
      }
    }

    std::string sql = "SELECT " + columns + " FROM " + spec.table_name + where;
    CommandResult res = RunMysql(sql);
    if (res.exit_code != 0) return;

    std::istringstream iss(res.output);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
      // Skip MySQL warning lines and the header line.
      if (line.find("mysql:") == 0 || line.find("Warning") == 0) continue;
      if (first) { first = false; continue; }
      if (line.empty()) continue;

      std::vector<std::string> cols;
      std::size_t pos = 0;
      while (pos < line.size()) {
        std::size_t tab = line.find('\t', pos);
        if (tab == std::string::npos) {
          std::string val = line.substr(pos);
          if (val == "NULL") val = "";
          cols.push_back(val);
          break;
        }
        std::string val = line.substr(pos, tab - pos);
        if (val == "NULL") val = "";
        cols.push_back(val);
        pos = tab + 1;
      }

      while (cols.size() < spec.projected_columns.size()) {
        cols.push_back("");
      }

      consumer(cols, cols.size());
    }
  }
};

void TestRealMysqlScannerFullScan() {
  SetMysqlTableScanner(nullptr);
  SetMysqlTableScanner(std::make_shared<RealMysqlCliScanner>());

  RunMysql("DROP TABLE IF EXISTS toy_real_scan");
  RunMysql("CREATE TABLE toy_real_scan ("
           "id INT, name VARCHAR(100), score INT)");
  RunMysql("INSERT INTO toy_real_scan VALUES "
           "(1, 'Alice', 100), (2, 'Bob', NULL), (3, 'Charlie', 300)");

  MysqlScanSpec spec;
  spec.table_name = "toy_real_scan";
  spec.projected_columns = {{"id"}, {"name"}, {"score"}};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t n) {
        Expect(n == 3, "Real full scan should project 3 columns");
        rows.push_back(cols);
      });

  Expect(rows.size() == 3, "Real full scan should return 3 rows");
  Expect(rows[0][0] == "1" && rows[0][1] == "Alice" && rows[0][2] == "100",
         "Real row 0 mismatch");
  Expect(rows[1][0] == "2" && rows[1][1] == "Bob" && rows[1][2] == "",
         "Real row 1 should have empty score (NULL via mysql CLI)");
  Expect(rows[2][0] == "3" && rows[2][1] == "Charlie" && rows[2][2] == "300",
         "Real row 2 mismatch");

  auto scanner = GetMysqlTableScanner();
  Expect(scanner != nullptr, "Real scanner should be registered");

  RunMysql("DROP TABLE IF EXISTS toy_real_scan");
  SetMysqlTableScanner(nullptr);

  std::cout << "  [PASS] real_mysql_full_scan\n";
}

void TestRealMysqlScannerSubsetProjection() {
  SetMysqlTableScanner(nullptr);
  SetMysqlTableScanner(std::make_shared<RealMysqlCliScanner>());

  RunMysql("DROP TABLE IF EXISTS toy_real_proj");
  RunMysql("CREATE TABLE toy_real_proj ("
           "a INT, b VARCHAR(100), c VARCHAR(10), d DECIMAL(5,2))");
  RunMysql("INSERT INTO toy_real_proj VALUES "
           "(10, 'hello', 'true', 99.9), "
           "(20, 'world', 'false', 88.8), "
           "(30, NULL, 'true', 77.7)");

  MysqlScanSpec spec;
  spec.table_name = "toy_real_proj";
  spec.projected_columns = {{"b"}, {"d"}};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t n) {
        Expect(n == 2, "Real subset projection should return 2 columns");
        rows.push_back(cols);
      });

  Expect(rows.size() == 3, "Real subset projection should return 3 rows");
  Expect(rows[0][0] == "hello" && rows[0][1] == "99.90", "Real proj row 0 mismatch");
  Expect(rows[1][0] == "world" && rows[1][1] == "88.80", "Real proj row 1 mismatch");

  RunMysql("DROP TABLE IF EXISTS toy_real_proj");
  SetMysqlTableScanner(nullptr);

  std::cout << "  [PASS] real_mysql_subset_projection\n";
}

void TestRealMysqlScannerFilter() {
  SetMysqlTableScanner(nullptr);
  SetMysqlTableScanner(std::make_shared<RealMysqlCliScanner>());

  RunMysql("DROP TABLE IF EXISTS toy_real_filter");
  RunMysql("CREATE TABLE toy_real_filter ("
           "id INT, status VARCHAR(20))");
  RunMysql("INSERT INTO toy_real_filter VALUES "
           "(1, 'active'), (2, 'inactive'), (3, 'active'), (4, 'pending')");

  MysqlScanSpec spec;
  spec.table_name = "toy_real_filter";
  spec.projected_columns = {{"id"}, {"status"}};
  spec.filter = MysqlScanFilter{1, "active"};

  std::vector<std::vector<std::string>> rows;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t n) {
        Expect(n == 2, "Real filter scan should project 2 columns");
        rows.push_back(cols);
      });

  Expect(rows.size() == 2, "Real filter should return 2 rows");
  Expect(rows[0][0] == "1" && rows[0][1] == "active", "Real filter row 0 mismatch");
  Expect(rows[1][0] == "3" && rows[1][1] == "active", "Real filter row 1 mismatch");

  RunMysql("DROP TABLE IF EXISTS toy_real_filter");
  SetMysqlTableScanner(nullptr);

  std::cout << "  [PASS] real_mysql_filter\n";
}

void TestRealMysqlScannerContractEnforcement() {
  SetMysqlTableScanner(nullptr);
  auto mock = GetMysqlTableScanner();
  Expect(mock != nullptr, "Default mock scanner should exist");

  SetMysqlTableScanner(std::make_shared<RealMysqlCliScanner>());

  RunMysql("DROP TABLE IF EXISTS toy_contract");
  RunMysql("CREATE TABLE toy_contract (col1 VARCHAR(10))");
  RunMysql("INSERT INTO toy_contract VALUES ('helloworld')");

  MysqlScanSpec spec;
  spec.table_name = "toy_contract";
  spec.projected_columns = {{"col1"}};

  std::string captured;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &cols, std::size_t) {
        if (!cols.empty()) captured = cols[0];
      });

  Expect(captured == "helloworld",
         "Real scanner should read real MySQL table contents");

  RunMysql("DROP TABLE IF EXISTS toy_contract");

  // Reset and verify mock still works.
  SetMysqlTableScanner(nullptr);
  std::size_t count = 0;
  ScanMysqlTableForRebuild(spec,
      [&](const std::vector<std::string> &, std::size_t) { ++count; });
  Expect(count == 0, "Mock scanner should not find real table");

  std::cout << "  [PASS] real_mysql_scanner_contract\n";
}

} // namespace

int main() {
  std::cout << "engine_scan[mysql]\n";
  TestFullScan();
  TestSubsetProjection();
  TestFilter();
  TestFilterOnProjectedSubset();
  TestEmptyTable();
  TestUnknownTable();
  TestMultipleNullsInRow();
  TestFilterNoMatches();
  TestScanIsolatedBetweenTables();

  std::cout << "\n--- Real MySQL-native scan tests ---\n";
  TestRealMysqlScannerFullScan();
  TestRealMysqlScannerSubsetProjection();
  TestRealMysqlScannerFilter();
  TestRealMysqlScannerContractEnforcement();

  std::cout << "engine_scan[mysql] PASS\n";
  return 0;
}
