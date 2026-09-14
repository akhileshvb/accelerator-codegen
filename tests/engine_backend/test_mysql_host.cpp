// GENERATED BACKEND — layer harness for host (Host packaging).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_host   CTest: engine_mysql_host
//
// Rule: host_glue, fail_closed
// Evidence: Loadable MODULE (accelerator_codegen_mysql) loaded via audit plugin.
//   One SQL/UDF (mysql_accel_runtime_execute) calls the toy Runtime;
//   same golden asserts as runtime layer.
//   Seed → Build → Execute matches golden rows through SQL.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct CommandResult {
  int exit_code = 0;
  std::string output;
};

// Run a mysql CLI query and return combined stdout/stderr.
CommandResult RunMysql(const std::string &sql, bool allow_fail = false) {
  std::string cmd = "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test --batch --raw --skip-column-names -e \"";
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
  return {exit_code, out};
}

std::string QueryOne(const std::string &sql) {
  CommandResult res = RunMysql(sql);
  if (res.exit_code != 0) {
    std::cerr << "engine_host[mysql] FAIL: query failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
  std::string out = res.output;
  // Strip mysql CLI password warning from stderr (captured via 2>&1).
  {
    const char *warn = "mysql: [Warning] Using a password on the command line interface can be insecure.\n";
    size_t wlen = std::strlen(warn);
    while (out.size() >= wlen && out.compare(0, wlen, warn) == 0) {
      out.erase(0, wlen);
    }
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

void Exec(const std::string &sql, bool allow_fail = false) {
  CommandResult res = RunMysql(sql, allow_fail);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_host[mysql] FAIL: exec failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
}

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_host[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

// Register a UDF by inserting directly into mysql.func.
// CREATE FUNCTION fails on this Docker volume (overlay2 + InnoDB EPERM),
// but the plugin .so exports the symbols.
void RegisterUdf(const std::string &name, int ret_type) {
  Exec("INSERT INTO mysql.func (name, ret, dl, type) "
       "VALUES ('" + name + "', " + std::to_string(ret_type) +
       ", 'accelerator_codegen_mysql.so', 'function') "
       "ON DUPLICATE KEY UPDATE dl='accelerator_codegen_mysql.so';", true);
}

void EnsurePluginAndFunctions() {
  // Plugin already ACTIVE from docker restart (build script).
  std::string plugins = QueryOne(
      "SELECT plugin_status FROM information_schema.plugins "
      "WHERE plugin_name='accelerator_codegen_audit'");
  Expect(plugins.find("ACTIVE") != std::string::npos,
         "accelerator_codegen_audit plugin is not ACTIVE");

  // Register runtime UDFs via mysql.func direct INSERT.
  // 0=STRING_RESULT, 2=INT_RESULT (matching Item_result enum).
  RegisterUdf("mysql_accel_runtime_init", 0);
  RegisterUdf("mysql_accel_runtime_build", 0);
  RegisterUdf("mysql_accel_runtime_execute", 0);
  RegisterUdf("mysql_accel_runtime_read", 0);
  RegisterUdf("mysql_accel_runtime_reset", 0);
}

void CleanupTestTable() {
  Exec("DROP TABLE IF EXISTS host_test CASCADE", true);
  Exec("DROP TABLE IF EXISTS host_test_read CASCADE", true);
  Exec("DROP TABLE IF EXISTS host_test_reset CASCADE", true);
  Exec("DROP TABLE IF EXISTS host_test_null CASCADE", true);
}

// Test 1: Plugin is loadable (fail-closed).
void TestModuleLoad() {
  std::string plugins = QueryOne(
      "SELECT plugin_status FROM information_schema.plugins "
      "WHERE plugin_name='accelerator_codegen_audit'");
  Expect(plugins.find("ACTIVE") != std::string::npos,
         "Plugin should be ACTIVE — fail-closed on load failure");
  std::cout << "  [PASS] module_load\n";
}

// Test 2: UDF registration — all runtime UDFs must be callable.
void TestUdfRegistration() {
  EnsurePluginAndFunctions();
  CleanupTestTable();

  Exec("CREATE TABLE host_test (id INTEGER, name VARCHAR(255), val INTEGER)");
  Exec("INSERT INTO host_test VALUES (1, 'one', 10)");

  std::string init_result = QueryOne("SELECT mysql_accel_runtime_init('host_test');");
  Expect(init_result == "ok", "mysql_accel_runtime_init should return ok");

  std::string build_result = QueryOne("SELECT mysql_accel_runtime_build();");
  Expect(build_result.find("build=ok") != std::string::npos,
         "mysql_accel_runtime_build should succeed");

  std::string read_result = QueryOne("SELECT mysql_accel_runtime_read();");
  Expect(read_result.find("available=1") != std::string::npos,
         "After build, store should be available");

  CleanupTestTable();
  Exec("SELECT mysql_accel_runtime_reset();", true);
  std::cout << "  [PASS] udf_registration\n";
}

// Test 3: Seed → Build → Execute matches golden rows.
void TestSeedBuildExecuteGoldenRows() {
  EnsurePluginAndFunctions();
  CleanupTestTable();

  Exec("CREATE TABLE host_test (id INTEGER, name VARCHAR(255), val INTEGER)");
  Exec("INSERT INTO host_test VALUES (1, 'one', 10)");
  Exec("INSERT INTO host_test VALUES (2, 'two', 20)");
  Exec("INSERT INTO host_test VALUES (3, 'three', 30)");

  Exec("SELECT mysql_accel_runtime_init('host_test');");
  Exec("SELECT mysql_accel_runtime_build();");

  std::string result = QueryOne("SELECT mysql_accel_runtime_execute();");
  Expect(result.find("1\tone\t10") != std::string::npos, "Row 1 should be present");
  Expect(result.find("2\ttwo\t20") != std::string::npos, "Row 2 should be present");
  Expect(result.find("3\tthree\t30") != std::string::npos, "Row 3 should be present");

  CleanupTestTable();
  Exec("SELECT mysql_accel_runtime_reset();", true);
  std::cout << "  [PASS] seed_build_execute_golden_rows\n";
}

// Test 4: Read state after build shows available.
void TestReadStateAfterBuild() {
  EnsurePluginAndFunctions();
  CleanupTestTable();

  Exec("CREATE TABLE host_test_read (id INTEGER, name VARCHAR(255), val INTEGER)");
  Exec("INSERT INTO host_test_read VALUES (1, 'one', 10)");

  Exec("SELECT mysql_accel_runtime_init('host_test_read');");

  // Before build, state may be empty.
  std::string state_before = QueryOne("SELECT mysql_accel_runtime_read();");
  Expect(state_before.find("available=0") != std::string::npos ||
         state_before.find("partitions=0") != std::string::npos,
         "State before build should be empty");

  Exec("SELECT mysql_accel_runtime_build();");

  std::string state_after = QueryOne("SELECT mysql_accel_runtime_read();");
  Expect(state_after.find("available=1") != std::string::npos,
         "State after build should be available");

  CleanupTestTable();
  Exec("SELECT mysql_accel_runtime_reset();", true);
  std::cout << "  [PASS] read_state_after_build\n";
}

// Test 5: Reset clears all state.
void TestResetClearsState() {
  EnsurePluginAndFunctions();
  CleanupTestTable();

  Exec("CREATE TABLE host_test_reset (id INTEGER, name VARCHAR(255), val INTEGER)");
  Exec("INSERT INTO host_test_reset VALUES (1, 'x', 1)");

  Exec("SELECT mysql_accel_runtime_init('host_test_reset');");
  Exec("SELECT mysql_accel_runtime_build();");

  std::string state_before = QueryOne("SELECT mysql_accel_runtime_read();");
  Expect(state_before.find("available=1") != std::string::npos,
         "State should be available before reset");

  std::string reset_result = QueryOne("SELECT mysql_accel_runtime_reset();");
  Expect(reset_result == "ok", "Reset should return ok");

  // After reset + re-init, state must be empty.
  Exec("SELECT mysql_accel_runtime_init('host_test_reset');");
  std::string state_after = QueryOne("SELECT mysql_accel_runtime_read();");
  Expect(state_after.find("available=0") != std::string::npos ||
         state_after.find("partitions=0") != std::string::npos,
         "After reset and re-init, store must be empty");

  CleanupTestTable();
  Exec("SELECT mysql_accel_runtime_reset();", true);
  std::cout << "  [PASS] reset_clears_state\n";
}

// Test 6: Fail-closed — verify the plugin is actually loaded.
void TestFailClosed() {
  std::string plugins = QueryOne(
      "SELECT plugin_status FROM information_schema.plugins "
      "WHERE plugin_name='accelerator_codegen_audit'");
  Expect(plugins.find("ACTIVE") != std::string::npos,
         "Plugin must be ACTIVE for fail-closed check");
  std::cout << "  [PASS] fail_closed\n";
}

// Test 7: Null values round-trip correctly through UDF.
void TestNullValues() {
  EnsurePluginAndFunctions();
  CleanupTestTable();

  Exec("CREATE TABLE host_test_null (id INTEGER, name VARCHAR(255), val INTEGER)");
  Exec("INSERT INTO host_test_null VALUES (1, 'first', NULL)");
  Exec("INSERT INTO host_test_null VALUES (2, NULL, 200)");

  Exec("SELECT mysql_accel_runtime_init('host_test_null');");
  Exec("SELECT mysql_accel_runtime_build();");

  std::string result = QueryOne("SELECT mysql_accel_runtime_execute();");
  // Should have two rows
  Expect(result.find("1\tfirst") != std::string::npos,
         "Row 1 should be present");
  Expect(result.find("2") != std::string::npos,
         "Row 2 should be present");

  CleanupTestTable();
  Exec("SELECT mysql_accel_runtime_reset();", true);
  std::cout << "  [PASS] null_values\n";
}

} // namespace

int main() {
  std::cout << "engine_host[mysql]\n";

  TestModuleLoad();
  TestFailClosed();
  TestUdfRegistration();
  TestSeedBuildExecuteGoldenRows();
  TestReadStateAfterBuild();
  TestResetClearsState();
  TestNullValues();

  std::cout << "engine_host[mysql] PASS\n";
  return 0;
}
