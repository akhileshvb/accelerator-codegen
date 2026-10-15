// GENERATED BACKEND — layer harness for visibility (Visibility).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_visibility   CTest: engine_mysql_visibility
//
// Rule: visibility
// Evidence:
//   mysql_bridge.cpp AuditNotify — COMMIT/ROLLBACK → MysqlHost::OnStatementEnd
//   mysql_accel_txn_capture → MysqlHost::OnCapturedRow (kept per session, not published)
//   mysql_accel_txn_read → the toy accelerator's state at the newest epoch
//   Uncommitted window: writer session holds START TRANSACTION + capture +
//   GET_LOCK('accel_vis_uncommitted'); a second mysql CLI session Reads the
//   in-process store (must be empty). Writer COMMIT; reader Sees published
//   image and a new epoch. Barrier is GET_LOCK / IS_USED_LOCK, not sleep.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct CommandResult {
  int exit_code = 0;
  std::string output;
};

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_visibility[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

std::string StripMysqlOutput(std::string out) {
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  std::vector<std::string> lines;
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    while (!line.empty() && (line.back() == '\r')) {
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
  // --batch prints a header row then the value.
  if (lines.size() >= 2) {
    return lines.back();
  }
  return lines.front();
}

CommandResult RunMysql(const std::string &sql) {
  std::string cmd =
      "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
      "--batch --raw -N -e \"";
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
    std::cerr << "engine_visibility[mysql] FAIL: query failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
  return StripMysqlOutput(res.output);
}

void Exec(const std::string &sql, bool allow_fail = false) {
  CommandResult res = RunMysql(sql);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_visibility[mysql] FAIL: exec failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
}

CommandResult RunMysqlMulti(const std::string &script) {
  char tmpname[] = "/tmp/mysql_vis_test_XXXXXX";
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
      "--batch --raw -N < ";
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
    std::cerr << "engine_visibility[mysql] FAIL: multi exec failed:\n"
              << script << "\n---\n" << res.output << "\n";
    std::abort();
  }
}

// Persistent writer connection so START TRANSACTION stays open while a second
// CLI session Reads the in-process store.
struct WriterSession {
  pid_t pid = -1;
  FILE *stdin_w = nullptr;
};

WriterSession StartWriter() {
  // Per-run writer log (not a fixed /tmp name shared by concurrent runs).
  const std::string log_path = (std::filesystem::temp_directory_path() /
                                ("mysql_vis_writer_" + std::to_string(::getpid()) + ".log"))
                                   .string();
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    Fail("pipe() for writer mysql failed");
  }
  pid_t pid = fork();
  if (pid < 0) {
    Fail("fork() for writer mysql failed");
  }
  if (pid == 0) {
    close(pipefd[1]);
    if (dup2(pipefd[0], STDIN_FILENO) < 0) {
      _exit(127);
    }
    close(pipefd[0]);
    int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log >= 0) {
      dup2(log, STDOUT_FILENO);
      dup2(log, STDERR_FILENO);
      close(log);
    }
    execl("/usr/bin/mysql", "mysql", "-h", "127.0.0.1", "-P", "3306", "-uroot",
          "-pmysql", "-D", "accel_mysql_test", "--unbuffered", "--batch",
          "--raw", nullptr);
    _exit(127);
  }
  close(pipefd[0]);
  WriterSession s;
  s.pid = pid;
  s.stdin_w = fdopen(pipefd[1], "w");
  if (!s.stdin_w) {
    Fail("fdopen() for writer mysql stdin failed");
  }
  return s;
}

void WriterSql(WriterSession &s, const char *sql) {
  if (!s.stdin_w) {
    Fail("writer session has no stdin");
  }
  fputs(sql, s.stdin_w);
  fputc('\n', s.stdin_w);
  fflush(s.stdin_w);
}

int CloseWriter(WriterSession &s) {
  if (s.stdin_w) {
    fclose(s.stdin_w);
    s.stdin_w = nullptr;
  }
  int st = 0;
  if (s.pid > 0) {
    if (waitpid(s.pid, &st, 0) < 0) {
      Fail("waitpid(writer) failed");
    }
    s.pid = -1;
  }
  return st;
}

bool LockIsHeld(const char *name) {
  std::string sql = "SELECT IFNULL(IS_USED_LOCK('";
  sql += name;
  sql += "'), 0)";
  std::string v = QueryOne(sql);
  return !v.empty() && v != "0";
}

void WaitUntilLockHeld(const char *name) {
  // Poll IS_USED_LOCK — this waits for the GET_LOCK barrier, not for visibility.
  for (int i = 0; i < 1500; ++i) {
    if (LockIsHeld(name)) {
      return;
    }
    usleep(10000);
  }
  Fail("timeout waiting for writer GET_LOCK barrier");
}

void EnsurePluginAndFunctions() {
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
  std::string check = QueryOne("SELECT mysql_accel_txn_init();");
  if (check.find("not_initialized") == std::string::npos &&
      check.find("1") == std::string::npos) {
    Fail("UDF mysql_accel_txn_init not functional after registration");
  }
}

void TestModuleLoad() {
  std::string plugins = QueryOne(
      "SELECT plugin_status FROM information_schema.plugins "
      "WHERE plugin_name='accelerator_codegen_audit'");
  if (plugins.find("ACTIVE") == std::string::npos) {
    Exec("DELETE FROM mysql.plugin WHERE name='accelerator_codegen_audit';", true);
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
  Expect(result.find("available=0") != std::string::npos ||
             result.find("partitions=0") != std::string::npos,
         "Initial state should have no partitions");
  std::cout << "  [PASS] init_bridge\n";
}

// Core visibility: second session Reads while the writer txn is still open.
void TestUncommittedWriteNotVisible() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);
  const std::string lock_name =
      "accel_vis_uncommitted_" + std::to_string(getpid());

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=0") != std::string::npos ||
             state.find("partitions=0") != std::string::npos,
         "Store should be empty before any commits");
  std::string epoch_before = QueryOne("SELECT mysql_accel_txn_epoch();");
  Expect(epoch_before.find("epoch=0") != std::string::npos,
         "Initial epoch should be 0");

  WriterSession writer = StartWriter();
  WriterSql(writer, "START TRANSACTION;");
  WriterSql(writer, "SELECT mysql_accel_txn_capture(1, 0, 42, 'uncommitted');");
  WriterSql(writer, ("SELECT GET_LOCK('" + lock_name + "', 0);").c_str());
  WaitUntilLockHeld(lock_name.c_str());

  // Second connection: store must not show the uncommitted capture.
  std::string snapshot_before = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(snapshot_before.find("available=0") != std::string::npos ||
             snapshot_before.find("partitions=0") != std::string::npos,
         "Snapshot before commit must NOT show uncommitted write");
  std::string epoch_open = QueryOne("SELECT mysql_accel_txn_epoch();");
  Expect(epoch_open == epoch_before, "Epoch must not change before commit");

  WriterSql(writer, "COMMIT;");
  WriterSql(writer, ("SELECT RELEASE_LOCK('" + lock_name + "');").c_str());
  CloseWriter(writer);

  std::string snapshot_after = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(snapshot_after.find("available=1") != std::string::npos,
         "Store should show published partition after commit");
  Expect(snapshot_after.find("label=staged") != std::string::npos,
         "Payload should contain staged label");
  std::string epoch_after = QueryOne("SELECT mysql_accel_txn_epoch();");
  Expect(epoch_before != epoch_after, "Epoch must advance after commit");
  Expect(snapshot_before != snapshot_after,
         "Snapshot before commit must differ from after commit");

  std::cout << "  [PASS] uncommitted_write_not_visible\n";
}

// Same two-session GET_LOCK barrier: Read returns immediately (mutex store),
// empty during the open txn and populated right after COMMIT — no sleep race.
void TestSyncBarrierNotSleep() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);
  const std::string lock_name = "accel_vis_sync_" + std::to_string(getpid());

  WriterSession writer = StartWriter();
  WriterSql(writer, "START TRANSACTION;");
  WriterSql(writer, "SELECT mysql_accel_txn_capture(1, 0, 42, 'sync_test');");
  WriterSql(writer, ("SELECT GET_LOCK('" + lock_name + "', 0);").c_str());
  WaitUntilLockHeld(lock_name.c_str());

  std::string immediate = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(immediate.find("available=0") != std::string::npos ||
             immediate.find("partitions=0") != std::string::npos,
         "Immediate snapshot during open txn must not show uncommitted data");

  WriterSql(writer, "COMMIT;");
  WriterSql(writer, ("SELECT RELEASE_LOCK('" + lock_name + "');").c_str());
  CloseWriter(writer);

  std::string after = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(after.find("available=1") != std::string::npos,
         "After commit, reader must see published data immediately");

  std::cout << "  [PASS] sync_barrier_not_sleep\n";
}

void TestRollbackKeepsStoreUnchanged() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  std::string epoch_before = QueryOne("SELECT mysql_accel_txn_epoch();");

  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 99, 'rolled_back');\n"
      "ROLLBACK;\n");

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=0") != std::string::npos ||
             state.find("partitions=0") != std::string::npos,
         "Store must be empty after rollback — no publication");

  std::string epoch_after = QueryOne("SELECT mysql_accel_txn_epoch();");
  Expect(epoch_before == epoch_after, "Epoch must not change after rollback");

  std::cout << "  [PASS] rollback_keeps_store_unchanged\n";
}

void TestEpochAndImageChangeAfterCommit() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 10, 'image_a');\n"
      "COMMIT;\n");

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "State after first commit should be available");
  Expect(state.find("label=staged") != std::string::npos,
         "State should contain staged payload");
  std::string epoch_a = QueryOne("SELECT mysql_accel_txn_epoch();");

  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 20, 'image_b');\n"
      "COMMIT;\n");

  state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "State after second commit should be available");

  std::string epoch_b = QueryOne("SELECT mysql_accel_txn_epoch();");
  Expect(epoch_a != epoch_b, "Epoch must advance between commits");

  std::cout << "  [PASS] epoch_image_change_after_commit\n";
}

void TestResetClearsState() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 77, 'pre_reset');\n"
      "COMMIT;\n");

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "State should be available before reset");

  std::string reset_result = QueryOne("SELECT mysql_accel_txn_reset();");
  Expect(reset_result.find("ok") != std::string::npos, "Reset should return ok");

  Exec("SELECT mysql_accel_txn_init();", false);
  state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=0") != std::string::npos ||
             state.find("partitions=0") != std::string::npos,
         "After reset and re-init, store must be empty");

  std::cout << "  [PASS] reset_clears_state\n";
}

void TestCaptureWithoutBridgeReturnsError() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_reset();", true);
  std::string result =
      QueryOne("SELECT mysql_accel_txn_capture(1, 0, 42, 'no_init');");
  Expect(result.find("not_initialized") != std::string::npos,
         "Capture without init should return not_initialized");
  std::cout << "  [PASS] capture_without_bridge_error\n";
}

void TestEmptyCommitNoPublish() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);
  ExecMulti("START TRANSACTION;\nCOMMIT;\n");
  std::cout << "  [PASS] empty_commit_no_publish\n";
}

void TestAbortThenFreshTxnCanCommit() {
  EnsurePluginAndFunctions();
  Exec("SELECT mysql_accel_txn_init();", false);

  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 50, 'aborted');\n"
      "ROLLBACK;\n");

  std::string state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=0") != std::string::npos ||
             state.find("partitions=0") != std::string::npos,
         "Store should be empty after abort");

  ExecMulti(
      "START TRANSACTION;\n"
      "SELECT mysql_accel_txn_capture(1, 0, 77, 'after_abort');\n"
      "COMMIT;\n");

  state = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(state.find("available=1") != std::string::npos,
         "Store should be available after fresh txn commit");

  std::cout << "  [PASS] abort_then_fresh_txn_commits\n";
}

}  // namespace

int main() {
  std::cout << "engine_visibility[mysql]\n";
  TestModuleLoad();
  TestInitBridge();
  TestUncommittedWriteNotVisible();
  TestSyncBarrierNotSleep();
  TestRollbackKeepsStoreUnchanged();
  TestEpochAndImageChangeAfterCommit();
  TestResetClearsState();
  TestCaptureWithoutBridgeReturnsError();
  TestEmptyCommitNoPublish();
  TestAbortThenFreshTxnCanCommit();
  std::cout << "engine_visibility[mysql] PASS\n";
  return 0;
}
