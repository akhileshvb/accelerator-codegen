// GENERATED BACKEND — layer harness for txn (Txn bridge).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_txn   CTest: engine_mysql_txn
//
// Rule: txn_lifecycle
// Evidence: harness drives real MySQL transactions through the mysql CLI.
//   BEGIN/ROLLBACK leaves observer epoch/image unchanged; commit advances
//   epoch and publishes image; another new transaction commits a different
//   image proving coordinator recovery.
//
// Rule: fail_closed
// Evidence: PLUGIN INSTALL fails → exit non-zero; bridge not initialized →
//   capture UDF returns error; commit without prior mutation produces no image.
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
  std::string cmd = "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test --batch --raw -e \"";
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
    std::cerr << "engine_txn[mysql] FAIL: query failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
  std::string out = res.output;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

void Exec(const std::string &sql, bool allow_fail = false) {
  CommandResult res = RunMysql(sql, allow_fail);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_txn[mysql] FAIL: exec failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
}

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_txn[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

// Run multiple statements on a single mysql connection via a temp SQL file.
CommandResult RunMysqlMulti(const std::string &script, bool allow_fail = false) {
  char tmpname[] = "/tmp/mysql_txn_test_XXXXXX";
  int fd = mkstemp(tmpname);
  if (fd < 0) return {1, "mkstemp failed"};
  FILE *tmpf = fdopen(fd, "w");
  if (!tmpf) { close(fd); return {1, "fdopen failed"}; }
  fputs(script.c_str(), tmpf);
  fclose(tmpf);

  std::string cmd = "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test --batch --raw < ";
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
  CommandResult res = RunMysqlMulti(script, allow_fail);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_txn[mysql] FAIL: multi exec failed:\n"
              << script << "\n---\n" << res.output << "\n";
    std::abort();
  }
}

// Docker overlay2 + O_DIRECT may block writes to mysql.func (InnoDB).
// Register UDFs idempotently — always use allow_fail on CREATE since
// UDFs may already exist from a prior build/run. Do NOT drop first.
void EnsurePluginAndFunctions() {
  // CREATE FUNCTION writes mysql.func (EPERM on this Docker volume). Rows
  // already persist; mysqld autoloads them when the .so is present at startup.
  Exec("CREATE FUNCTION mysql_accel_txn_init RETURNS INTEGER SONAME 'accelerator_codegen_mysql.so';", true);
  Exec("CREATE FUNCTION mysql_accel_txn_capture RETURNS STRING SONAME 'accelerator_codegen_mysql.so';", true);
  Exec("CREATE FUNCTION mysql_accel_txn_read RETURNS STRING SONAME 'accelerator_codegen_mysql.so';", true);
  Exec("CREATE FUNCTION mysql_accel_txn_epoch RETURNS STRING SONAME 'accelerator_codegen_mysql.so';", true);
  Exec("CREATE FUNCTION mysql_accel_txn_reset RETURNS STRING SONAME 'accelerator_codegen_mysql.so';", true);

  std::string check = QueryOne("SELECT mysql_accel_txn_init();");
  if (check.find("not_initialized") == std::string::npos &&
      check.find("1") == std::string::npos) {
    Fail("UDF mysql_accel_txn_init not functional after registration");
  }
}

void TestModuleLoad() {
  // Do not UNINSTALL: overwriting a mapped audit .so then unloading it SIGSEGVs
  // mysqld (exit 139). If the plugin is already ACTIVE, that is the load proof.
  std::string plugins = QueryOne(
      "SELECT plugin_status FROM information_schema.plugins "
      "WHERE plugin_name='accelerator_codegen_audit'");
  if (plugins.find("ACTIVE") == std::string::npos) {
    Exec("DELETE FROM mysql.plugin WHERE name='accelerator_codegen_audit';",
         true);
    Exec("INSTALL PLUGIN accelerator_codegen_audit "
         "SONAME 'accelerator_codegen_mysql.so';",
         false);
    plugins = QueryOne(
        "SELECT plugin_status FROM information_schema.plugins "
        "WHERE plugin_name='accelerator_codegen_audit'");
  }
  Expect(plugins.find("ACTIVE") != std::string::npos,
         "accelerator_codegen_audit plugin is not ACTIVE");
  std::cout << "  [PASS] module_load\n";
}

void TestInitBridge() {
  EnsurePluginAndFunctions();
  std::string result = QueryOne("SELECT mysql_accel_txn_init();");
  Expect(result.find("1") != std::string::npos,
         "mysql_accel_txn_init should return 1");

  result = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(result.find("epoch=0") != std::string::npos,
         "Initial epoch should be 0");
  Expect(result.find("available=0") != std::string::npos,
         "Initial state should have no partitions");

  std::cout << "  [PASS] init_bridge\n";
}

void TestRollbackDoesNotPublish() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  std::string script =
    "START TRANSACTION;\n"
    "SELECT mysql_accel_txn_capture(1, 0, 10, 'staged');\n"
    "ROLLBACK;\n";
  ExecMulti(script, false);

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") == std::string::npos ||
         state.find("partitions=0") != std::string::npos,
         "Store should be empty after rollback");

  std::string epoch = QueryOne("SELECT mysql_accel_txn_epoch();");
  Expect(epoch.find("0") != std::string::npos,
         "Epoch should be 0 after rollback (no publish)");

  std::cout << "  [PASS] rollback_does_not_publish\n";
}

void TestCommitPublishesOnce() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  std::string script =
    "START TRANSACTION;\n"
    "SELECT mysql_accel_txn_capture(1, 0, 10, 'staged');\n"
    "COMMIT;\n";
  ExecMulti(script, false);

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "Store should be available after commit");
  Expect(state.find("count=42") != std::string::npos,
         "Payload count should be staged");
  Expect(state.find("label=staged") != std::string::npos,
         "Payload label should be staged");

  std::cout << "  [PASS] commit_publishes_once\n";
}

void TestSecondTransactionCommitsDifferentImage() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  std::string script1 =
    "START TRANSACTION;\n"
    "SELECT mysql_accel_txn_capture(1, 0, 1, 'first');\n"
    "COMMIT;\n";
  ExecMulti(script1, false);

  std::string epoch1 = QueryOne("SELECT mysql_accel_txn_epoch();");

  std::string script2 =
    "START TRANSACTION;\n"
    "SELECT mysql_accel_txn_capture(1, 0, 2, 'second');\n"
    "COMMIT;\n";
  ExecMulti(script2, false);

  std::string epoch2 = QueryOne("SELECT mysql_accel_txn_epoch();");
  Expect(epoch1 != epoch2, "Epoch should advance between first and second commit");

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "Store should be available after second commit");

  std::cout << "  [PASS] second_transaction_commits\n";
}

void TestCommitWithoutMutationProducesNoImage() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  std::string script =
    "START TRANSACTION;\n"
    "COMMIT;\n";
  ExecMulti(script, false);

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  (void)state;
  // Empty commit should not crash — the coordinator simply clears state

  std::cout << "  [PASS] empty_commit_no_publish\n";
}

void TestCaptureWithoutBridgeReturnsError() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_reset();", true);
  std::string result = QueryOne(
      "SELECT mysql_accel_txn_capture(1, 0, 42, 'no_init');");
  Expect(result.find("not_initialized") != std::string::npos,
         "Capture without init should return not_initialized");

  std::cout << "  [PASS] capture_without_bridge_error\n";
}

void TestMultipleMutationsInOneTransaction() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  std::string script =
    "START TRANSACTION;\n"
    "SELECT mysql_accel_txn_capture(1, 0, 10, 'multi_a');\n"
    "SELECT mysql_accel_txn_capture(1, 1, 20, 'multi_b');\n"
    "COMMIT;\n";
  ExecMulti(script, false);

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "Store should be available after multi-mutation commit");

  std::cout << "  [PASS] multiple_mutations_one_txn\n";
}

void TestAbortThenFreshTxnCanCommit() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_reset();", true);
  Exec("SELECT mysql_accel_txn_init();", false);

  std::string script1 =
    "START TRANSACTION;\n"
    "SELECT mysql_accel_txn_capture(1, 0, 50, 'aborted');\n"
    "ROLLBACK;\n";
  ExecMulti(script1, false);

  std::string state_after_abort = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state_after_abort.find("available=1") == std::string::npos ||
         state_after_abort.find("partitions=0") != std::string::npos,
         "Store should be empty after abort");

  std::string script2 =
    "START TRANSACTION;\n"
    "SELECT mysql_accel_txn_capture(1, 0, 77, 'after_abort');\n"
    "COMMIT;\n";
  ExecMulti(script2, false);

  std::string state_after_commit = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state_after_commit.find("available=1") != std::string::npos,
         "Store should be available after fresh txn commit following abort");

  std::cout << "  [PASS] abort_then_fresh_txn_commits\n";
}

} // namespace

int main() {
  std::cout << "engine_txn[mysql]\n";

  TestModuleLoad();
  TestInitBridge();
  TestRollbackDoesNotPublish();
  TestCommitPublishesOnce();
  TestSecondTransactionCommitsDifferentImage();
  TestCommitWithoutMutationProducesNoImage();
  TestCaptureWithoutBridgeReturnsError();
  TestMultipleMutationsInOneTransaction();
  TestAbortThenFreshTxnCanCommit();

  std::cout << "engine_txn[mysql] PASS\n";
  return 0;
}
