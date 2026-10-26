// VALIDATION-OWNED — engine ↔ substrate ADVERSARIAL harness for mysql.
// Spec: pipeline/prompts/engine_backend/docs/adversarial_tests.md
// Build: test_mysql_adversarial   CTest: engine_mysql_adversarial
//
// Unseen by implementation layer harnesses. Cases below are not copies of
// test_mysql_{txn,mutations,scan,visibility,host}.cpp.
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

#include "backends/mysql/engine/scan_helper.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::engine::mysql;

namespace {

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_adversarial[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

struct CommandResult {
  int exit_code = 0;
  std::string output;
};

std::string StripMysqlOutput(std::string out) {
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  std::vector<std::string> lines;
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    while (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.find("Using a password") != std::string::npos) {
      continue;
    }
    if (line.rfind("mysql:", 0) == 0) {
      continue;
    }
    if (line.empty()) {
      continue;
    }
    lines.push_back(line);
  }
  if (lines.empty()) {
    return {};
  }
  return lines.back();
}

CommandResult RunMysql(const std::string &sql) {
  std::string cmd =
      "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
      "--batch --raw --skip-column-names -e \"";
  for (char c : sql) {
    if (c == '\\' || c == '"') {
      cmd.push_back('\\');
    }
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
  return {exit_code, out};
}

std::string QueryOne(const std::string &sql) {
  CommandResult res = RunMysql(sql);
  if (res.exit_code != 0) {
    std::cerr << "engine_adversarial[mysql] FAIL: query failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
  return StripMysqlOutput(res.output);
}

void Exec(const std::string &sql, bool allow_fail = false) {
  CommandResult res = RunMysql(sql);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_adversarial[mysql] FAIL: exec failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
}

CommandResult RunMysqlMulti(const std::string &script) {
  char tmpname[] = "/tmp/mysql_adv_XXXXXX";
  int fd = mkstemp(tmpname);
  if (fd < 0) {
    return {1, "mkstemp failed"};
  }
  FILE *tmpf = fdopen(fd, "w");
  if (!tmpf) {
    close(fd);
    return {1, "fdopen failed"};
  }
  fputs(script.c_str(), tmpf);
  fclose(tmpf);

  std::string cmd =
      "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
      "--batch --raw --skip-column-names < ";
  cmd += tmpname;
  cmd += " 2>&1";

  char buffer[4096];
  std::string out;
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    unlink(tmpname);
    return {1, "popen failed"};
  }
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    out += buffer;
  }
  int status = pclose(pipe);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  unlink(tmpname);
  return {exit_code, out};
}

void ExecMulti(const std::string &script, bool allow_fail = false) {
  CommandResult res = RunMysqlMulti(script);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_adversarial[mysql] FAIL: multi exec failed:\n"
              << script << "\n---\n" << res.output << "\n";
    std::abort();
  }
}

void RegisterUdf(const std::string &name, int ret_type) {
  Exec("INSERT INTO mysql.func (name, ret, dl, type) VALUES ('" + name + "', " +
           std::to_string(ret_type) +
           ", 'accelerator_codegen_mysql.so', 'function') "
           "ON DUPLICATE KEY UPDATE dl='accelerator_codegen_mysql.so';",
       true);
}

void EnsureTxnAndRuntimeUdfs() {
  std::string plugins = QueryOne(
      "SELECT plugin_status FROM information_schema.plugins "
      "WHERE plugin_name='accelerator_codegen_audit'");
  Expect(plugins.find("ACTIVE") != std::string::npos,
         "accelerator_codegen_audit must be ACTIVE");

  Exec("CREATE FUNCTION mysql_accel_txn_init RETURNS INTEGER SONAME "
       "'accelerator_codegen_mysql.so';",
       true);
  Exec("CREATE FUNCTION mysql_accel_txn_capture RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';",
       true);
  Exec("CREATE FUNCTION mysql_accel_txn_read RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';",
       true);
  Exec("CREATE FUNCTION mysql_accel_txn_epoch RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';",
       true);
  Exec("CREATE FUNCTION mysql_accel_txn_reset RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';",
       true);
  RegisterUdf("mysql_accel_runtime_init", 0);
  RegisterUdf("mysql_accel_runtime_build", 0);
  RegisterUdf("mysql_accel_runtime_execute", 0);
  RegisterUdf("mysql_accel_runtime_read", 0);
  RegisterUdf("mysql_accel_runtime_reset", 0);
}

// 1. Scan: seeded empty table yields zero consumer callbacks (layer tests
// cover unknown table / no-match filter, not an empty seeded relation).
void TestEmptySeededTableScan() {
  ClearAllMysqlTables();
  SeedMysqlTable("adv_empty", {"id", "name"}, {});
  MysqlScanSpec spec;
  spec.table_name = "adv_empty";
  spec.projected_columns = {{"id"}, {"name"}};
  std::size_t n = 0;
  ScanMysqlTableForRebuild(spec, [&](const std::vector<std::string> &,
                                     std::size_t) { ++n; });
  Expect(n == 0, "Empty seeded table must yield 0 scan callbacks");
  std::cout << "  [PASS] empty_seeded_table_scan\n";
}

// 2. Scan: zero-column projection still walks rows with arity 0.
void TestZeroColumnProjection() {
  ClearAllMysqlTables();
  SeedMysqlTable("adv_zero_proj", {"a", "b"}, {{"1", "x"}, {"2", "y"}});
  MysqlScanSpec spec;
  spec.table_name = "adv_zero_proj";
  spec.projected_columns = {};
  std::size_t n = 0;
  ScanMysqlTableForRebuild(
      spec, [&](const std::vector<std::string> &cols, std::size_t arity) {
        Expect(arity == 0, "Zero-column projection arity must be 0");
        Expect(cols.empty(), "Zero-column projection must pass empty cells");
        ++n;
      });
  Expect(n == 2, "Zero-column projection must still visit both rows");
  std::cout << "  [PASS] zero_column_projection\n";
}

// 3. SQL: an aborted transaction's capture does not leak into a later commit:
// exactly the later transaction is maintained.
void TestSqlAbortDoesNotLeakIntoLaterCommit() {
  EnsureTxnAndRuntimeUdfs();
  Exec("SELECT mysql_accel_txn_reset();", true);
  Exec("SELECT mysql_accel_txn_init();");
  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 1, 'aborted');\n"
      "ROLLBACK;\n"
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 1, 'kept');\n"
      "COMMIT;\n");
  Expect(QueryOne("SELECT mysql_accel_txn_epoch();") == "epoch=1 alias_epoch=1",
         "Only the committed transaction may be maintained");
  std::cout << "  [PASS] sql_abort_does_not_leak\n";
}

// 5. SQL: capture of table 99 then COMMIT leaves the txn toy store empty.
void TestSqlWrongTableCaptureDoesNotPublish() {
  EnsureTxnAndRuntimeUdfs();
  Exec("SELECT mysql_accel_txn_reset();", true);
  Exec("SELECT mysql_accel_txn_init();");
  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(99, 0, 1, 'nope');\n"
      "COMMIT;\n");
  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") == std::string::npos,
         "Wrong table_id commit must not publish");
  std::cout << "  [PASS] sql_wrong_table_no_publish\n";
}

// 4. SQL: a capture outside START TRANSACTION is an autocommit statement's
// write: committed when the statement ends, no explicit COMMIT needed.
void TestSqlAutocommitCaptureCommitsAtStatementEnd() {
  EnsureTxnAndRuntimeUdfs();
  Exec("SELECT mysql_accel_txn_reset();", true);
  Exec("SELECT mysql_accel_txn_init();");
  Exec("SELECT mysql_accel_txn_capture(1, 0, 1, 'autocommit');");
  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "An autocommit capture must be published when its statement ends");
  std::cout << "  [PASS] sql_autocommit_capture_commits\n";
}

// 6. Host UDFs fail closed: execute/read without init do not crash and do
// not pretend to return a built image.
void TestRuntimeUdfsFailClosedWithoutInit() {
  EnsureTxnAndRuntimeUdfs();
  Exec("SELECT mysql_accel_runtime_reset();", true);
  CommandResult exec_res = RunMysql("SELECT mysql_accel_runtime_execute();");
  Expect(exec_res.exit_code == 0,
         "execute without init must not crash mysqld");
  std::string exec_out = StripMysqlOutput(exec_res.output);
  Expect(exec_out.find("1\tone") == std::string::npos &&
             exec_out.find("available=1") == std::string::npos,
         "execute without init must not return a golden image");

  CommandResult read_res = RunMysql("SELECT mysql_accel_runtime_read();");
  Expect(read_res.exit_code == 0, "read without init must not crash");
  std::string read_out = StripMysqlOutput(read_res.output);
  Expect(read_out.find("available=1") == std::string::npos,
         "read without init must not report a published image");
  std::cout << "  [PASS] runtime_udfs_fail_closed\n";
}

}  // namespace

int main() {
  std::cout << "engine_adversarial[mysql]\n";
  TestEmptySeededTableScan();
  TestZeroColumnProjection();
  TestSqlAbortDoesNotLeakIntoLaterCommit();
  TestSqlWrongTableCaptureDoesNotPublish();
  TestSqlAutocommitCaptureCommitsAtStatementEnd();
  TestRuntimeUdfsFailClosedWithoutInit();
  std::cout << "engine_adversarial[mysql] PASS\n";
  return 0;
}
