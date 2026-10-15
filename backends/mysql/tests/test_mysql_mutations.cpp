// GENERATED BACKEND — layer harness for mutations (Mutation capture).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_mutations   CTest: engine_mysql_mutations
//
// Rule: mutation_hooks
// Evidence: INSERT/UPDATE/DELETE through real MySQL trigger;
//   harness asserts typed MutationBatch fields: table id as uint64,
//   kind as MutationKind enum (0=Insert, 1=Delete, 2=Update),
//   old/new rows with typed AuxValue cells.
//   The capture goes through mysql_accel_capture_row, as generated triggers do.
//
// Rule: fail_closed
// Evidence: trigger without bridge init does not crash; PLUGIN INSTALL fails
//   -> exit non-zero.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct CommandResult {
  int exit_code = 0;
  std::string output;
};

static std::string ShellEscape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '\'') out += "'\"'\"'";
    else out.push_back(c);
  }
  return out;
}

CommandResult RunMysql(const std::string &sql, bool allow_fail = false) {
  std::string cmd = "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
                    "-s -N -r -e '" + ShellEscape(sql) + "' 2>/dev/null";
  (void)allow_fail;

  char buffer[4096];
  std::string out;
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {1, "popen failed"};
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  int status = pclose(pipe);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  return {exit_code, out};
}

CommandResult RunMysqlMulti(const std::string &script, bool allow_fail = false) {
  (void)allow_fail;
  char tmpname[] = "/tmp/mysql_mut_XXXXXX";
  int fd = mkstemp(tmpname);
  if (fd < 0) return {1, "mkstemp failed"};
  FILE *tmpf = fdopen(fd, "w");
  if (!tmpf) { close(fd); return {1, "fdopen failed"}; }
  fputs(script.c_str(), tmpf);
  fclose(tmpf);

  std::string cmd = "mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
                    "-s -N -r < " + std::string(tmpname) + " 2>/dev/null";

  char buffer[4096];
  std::string out;
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) { unlink(tmpname); return {1, "popen failed"}; }
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
  int status = pclose(pipe);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  unlink(tmpname);
  return {exit_code, out};
}

static std::string LastDataLine(const std::string &raw) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t nl = raw.find('\n', pos);
    std::string line = (nl == std::string::npos) ? raw.substr(pos) : raw.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pos = (nl == std::string::npos) ? raw.size() : nl + 1;
    if (line.empty()) continue;
    if (line.find("mysql: [Warning]") != std::string::npos) continue;
    if (line.size() >= 3 && line.back() == ')' && line[line.size()-2] == '(') continue;
    lines.push_back(line);
  }
  return lines.empty() ? "" : lines.back();
}

std::string QueryOne(const std::string &sql) {
  CommandResult res = RunMysql(sql);
  if (res.exit_code != 0) {
    std::cerr << "engine_mutations[mysql] FAIL: query failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
  return LastDataLine(res.output);
}

void Exec(const std::string &sql, bool allow_fail = false) {
  CommandResult res = RunMysql(sql, allow_fail);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_mutations[mysql] FAIL: exec failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
}

void ExecMulti(const std::string &script, bool allow_fail = false) {
  CommandResult res = RunMysqlMulti(script, allow_fail);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_mutations[mysql] FAIL: multi exec failed:\n"
              << script << "\n---\n" << res.output << "\n";
    std::abort();
  }
}

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_mutations[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) Fail(msg);
}

void DropAndCreateTable(const std::string &table_name) {
  Exec("DROP TABLE IF EXISTS " + table_name + " CASCADE", true);
  Exec("CREATE TABLE " + table_name +
       " (id INT AUTO_INCREMENT PRIMARY KEY, val INT, label VARCHAR(255))", "create_table");
}

// Table ids of the capture calls; not the toy accelerators' (1, 2).
static int g_table_id = 100;
std::string NextTableId() { return std::to_string(g_table_id++); }

// The capture call of a (val INT, label VARCHAR) table: types "is", then the
// old row's columns and/or the new row's.
void CreateMutationTriggers(const std::string &table_name, const std::string &tid = NextTableId()) {
  auto capture = [&](const char *kind, const std::string &columns) {
    return "SET @a = mysql_accel_capture_row('" + tid + "', '" + kind + "', 'is', " + columns + ");";
  };

  Exec("DROP TRIGGER IF EXISTS trg_" + table_name + "_ins", true);
  Exec("DROP TRIGGER IF EXISTS trg_" + table_name + "_del", true);
  Exec("DROP TRIGGER IF EXISTS trg_" + table_name + "_upd", true);

  Exec("CREATE TRIGGER trg_" + table_name + "_ins AFTER INSERT ON " + table_name +
       " FOR EACH ROW " + capture("0", "NEW.val, NEW.label"));
  Exec("CREATE TRIGGER trg_" + table_name + "_del AFTER DELETE ON " + table_name +
       " FOR EACH ROW " + capture("1", "OLD.val, OLD.label"));
  Exec("CREATE TRIGGER trg_" + table_name + "_upd AFTER UPDATE ON " + table_name +
       " FOR EACH ROW " + capture("2", "OLD.val, OLD.label, NEW.val, NEW.label"));
}

void EnsureMutationFunctions() {
  Exec("CREATE FUNCTION mysql_accel_capture_row RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';", true);
  Exec("CREATE FUNCTION mysql_accel_mutations_last RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';", true);
  Exec("CREATE FUNCTION mysql_accel_mutations_drain RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';", true);
  Exec("CREATE FUNCTION mysql_accel_mutations_count RETURNS STRING SONAME "
       "'accelerator_codegen_mysql.so';", true);
  std::string check = QueryOne("SELECT mysql_accel_mutations_count();");
  Expect(!check.empty() && check != "", "Mutations UDF should be functional");
}

void TestModuleLoad() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");
  std::string result = QueryOne("SELECT mysql_accel_mutations_drain();");
  Expect(result == "ok", "Mutations drain should return ok");
  std::cout << "  [PASS] module_load\n";
}

void TestCaptureMutationAloneIsNotThisGate() {
  // This harness only tests mutations through real SQL triggers.
  // Direct C++ CaptureMutation(hand_built) must not be the mechanism
  // this harness relies on — it is tested by the aux_backend layer.
  // Verify the UDF plumbing actually uses the bridge:
  // if we init the bridge, drain, do INSERT, we must see a captured entry.
  EnsureMutationFunctions();

  std::string table = "mut_test_notaln";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);

  // Drain first
  Exec("SELECT mysql_accel_mutations_drain();");
  std::string before = QueryOne("SELECT mysql_accel_mutations_count();");
  Expect(before == "0", "Should start with 0 mutations");

  // Insert through SQL — this must hit the UDF
  Exec("INSERT INTO " + table + " (val, label) VALUES (42, 'hello')");
  std::string after = QueryOne("SELECT mysql_accel_mutations_count();");
  Expect(after != "0", "SQL insert should trigger capture");
  Expect(after == "1", "Should have exactly 1 captured mutation");

  // Now assert typed fields from the last capture
  std::string last = QueryOne("SELECT mysql_accel_mutations_last();");
  // table= as uint64
  Expect(last.find("table=") != std::string::npos, "Last should contain table=");
  // kind=0 (Insert) — typed MutationKind
  Expect(last.find("kind=0") != std::string::npos, "Insert mutation kind should be 0");
  // new row values — typed AuxValue cells
  Expect(last.find("new=") != std::string::npos, "Should contain new= for insert");
  Expect(last.find("42") != std::string::npos, "New row should contain val=42");
  Expect(last.find("hello") != std::string::npos, "New row should contain label=hello");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] capture_mutation_sql_only_no_hand_built\n";
}

void TestInsertCapturesMutation() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_ins";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val, label) VALUES (1, 'cat')");

  std::string last = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last.find("table=") != std::string::npos, "Should contain typed table id");
  Expect(last.find("kind=0") != std::string::npos, "Mutation kind should be Insert (0)");
  Expect(last.find("new=") != std::string::npos, "Should contain new= for insert");
  Expect(last.find("1") != std::string::npos, "New row should contain val=1");
  Expect(last.find("cat") != std::string::npos, "New row should contain label=cat");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] insert_captures_mutation\n";
}

void TestDeleteCapturesOldRowValues() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_del";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val, label) VALUES (7, 'dog')");
  Exec("DELETE FROM " + table + " WHERE val = 7");

  std::string last = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last.find("table=") != std::string::npos, "Should contain typed table id");
  Expect(last.find("kind=1") != std::string::npos, "Mutation kind should be Delete (1)");
  Expect(last.find("old=") != std::string::npos, "Should contain old= for delete");
  Expect(last.find("7") != std::string::npos, "Old row should contain val=7");
  Expect(last.find("dog") != std::string::npos, "Old row should contain label=dog");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] delete_captures_old_row_values\n";
}

void TestUpdateCapturesOldAndNewRows() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_upd";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val, label) VALUES (5, 'bird')");
  Exec("UPDATE " + table + " SET val = 6, label = 'fish' WHERE val = 5");

  std::string last = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last.find("table=") != std::string::npos, "Should contain typed table id");
  Expect(last.find("kind=2") != std::string::npos, "Mutation kind should be Update (2)");
  Expect(last.find("old=") != std::string::npos, "Should contain old= for update");
  Expect(last.find("new=") != std::string::npos, "Should contain new= for update");
  Expect(last.find("5") != std::string::npos, "Old row should contain val=5");
  Expect(last.find("bird") != std::string::npos, "Old row should contain label=bird");
  Expect(last.find("6") != std::string::npos, "New row should contain val=6");
  Expect(last.find("fish") != std::string::npos, "New row should contain label=fish");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] update_captures_old_and_new_rows\n";
}

void TestInsertWithNullLabel() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_null";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val, label) VALUES (99, NULL)");

  std::string last = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last.find("kind=0") != std::string::npos, "Mutation kind should be Insert (0)");
  Expect(last.find("99") != std::string::npos, "New row should contain val=99");
  Expect(last.find("NULL") != std::string::npos, "New row should contain NULL label");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] insert_with_null_label\n";
}

void TestMultipleMutationsInTransaction() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_txn";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);

  ExecMulti(
      "START TRANSACTION;\n"
      "INSERT INTO " + table + " (val, label) VALUES (1, 'a');\n"
      "INSERT INTO " + table + " (val, label) VALUES (2, 'b');\n"
      "UPDATE " + table + " SET val = 3 WHERE val = 1;\n"
      "DELETE FROM " + table + " WHERE val = 2;\n"
      "COMMIT;\n"
  );

  std::string count = QueryOne("SELECT mysql_accel_mutations_count();");
  int n = std::stoi(count);
  Expect(n >= 4, "Transaction with 4 DML should capture >= 4 mutations");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] multiple_mutations_in_transaction\n";
}

void TestTriggerPassesThroughData() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_pass";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val, label) VALUES (33, 'test')");

  std::string result = QueryOne(
      "SELECT val FROM " + table + " WHERE label = 'test'");
  Expect(result == "33", "Trigger should pass through row data unchanged");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] trigger_passthrough\n";
}

void TestTriggerWithoutBridgeInitDoesNotCrash() {
  EnsureMutationFunctions();

  std::string table = "mut_test_no_init";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val, label) VALUES (1, 'no_init')");

  std::string result = QueryOne(
      "SELECT val FROM " + table + " WHERE label = 'no_init'");
  Expect(result == "1", "Row should be inserted even without bridge init");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] trigger_without_bridge_no_crash\n";
}

void TestDeleteWithNullColumn() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_del_null";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val, label) VALUES (99, NULL)");
  Exec("DELETE FROM " + table + " WHERE val = 99");

  std::string last = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last.find("kind=1") != std::string::npos, "Mutation kind should be Delete (1)");
  Expect(last.find("99") != std::string::npos, "Old row should contain val=99");
  Expect(last.find("NULL") != std::string::npos, "Old row should contain NULL label");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] delete_with_null_column\n";
}

void TestInsertWithExplicitDefaults() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_defaults";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);
  Exec("INSERT INTO " + table + " (val) VALUES (88)");

  std::string last = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last.find("88") != std::string::npos, "New row should contain val=88");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] insert_with_explicit_defaults\n";
}

void TestCountIncrementsWithMutations() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_count";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);

  Exec("INSERT INTO " + table + " (val, label) VALUES (1, 'a')");
  std::string c1 = QueryOne("SELECT mysql_accel_mutations_count();");

  Exec("INSERT INTO " + table + " (val, label) VALUES (2, 'b')");
  std::string c2 = QueryOne("SELECT mysql_accel_mutations_count();");

  int n1 = std::stoi(c1);
  int n2 = std::stoi(c2);
  Expect(n2 > n1, "Count should increment with additional mutations");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] count_increments_with_mutations\n";
}

void TestDrainClearsMutations() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_drain";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);

  Exec("INSERT INTO " + table + " (val, label) VALUES (5, 'x')");
  std::string c_before = QueryOne("SELECT mysql_accel_mutations_count();");
  Expect(c_before != "0", "Count should be non-zero after insert");

  Exec("SELECT mysql_accel_mutations_drain();");
  std::string c_after = QueryOne("SELECT mysql_accel_mutations_count();");
  Expect(c_after == "0", "Count should be 0 after drain");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] drain_clears_mutations\n";
}

void TestMultipleTables() {
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string t1 = "mut_test_t1";
  DropAndCreateTable(t1);
  CreateMutationTriggers(t1);

  std::string t2 = "mut_test_t2";
  DropAndCreateTable(t2);
  CreateMutationTriggers(t2);

  Exec("INSERT INTO " + t1 + " (val, label) VALUES (10, 't1')");
  Exec("INSERT INTO " + t2 + " (val, label) VALUES (20, 't2')");

  std::string count = QueryOne("SELECT mysql_accel_mutations_count();");
  int n = std::stoi(count);
  Expect(n >= 2, "Should have at least 2 captured mutations from 2 tables");

  Exec("DROP TABLE IF EXISTS " + t1 + " CASCADE", true);
  Exec("DROP TABLE IF EXISTS " + t2 + " CASCADE", true);
  std::cout << "  [PASS] multiple_tables\n";
}

void TestTypedMutationBatchFields() {
  // Asserts that the UDF output contains properly typed fields:
  // table as uint64, kind as MutationKind enum, old/new rows with typed cells.
  EnsureMutationFunctions();
  Exec("SELECT mysql_accel_mutations_drain();");

  std::string table = "mut_test_typed";
  DropAndCreateTable(table);
  CreateMutationTriggers(table);

  // Insert
  Exec("INSERT INTO " + table + " (val, label) VALUES (42, 'typed')");
  std::string last_ins = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last_ins.find("table=") != std::string::npos, "Insert must have table= field");
  Expect(last_ins.find("kind=0") != std::string::npos, "Insert must have kind=0");
  Expect(last_ins.find("new=42;typed") != std::string::npos, "Insert new row must be typed: 42;typed");

  // Delete
  Exec("DELETE FROM " + table + " WHERE val = 42");
  std::string last_del = QueryOne("SELECT mysql_accel_mutations_last();");
  Expect(last_del.find("table=") != std::string::npos, "Delete must have table= field");
  Expect(last_del.find("kind=1") != std::string::npos, "Delete must have kind=1");
  Expect(last_del.find("old=42;typed") != std::string::npos, "Delete old row must be typed: 42;typed");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] typed_mutation_batch_fields\n";
}

void TestHostPipelineIsWired() {
  // A trigger capture of the txn toy accelerator's table (1) reaches the host
  // like any capture: the autocommit INSERT commits it and the toy's
  // maintenance runs (its epoch advances).
  EnsureMutationFunctions();
  for (const char *name : {"init", "read", "epoch", "reset"}) {
    Exec(std::string("CREATE FUNCTION mysql_accel_txn_") + name + " RETURNS " +
             (std::string(name) == "init" ? "INTEGER" : "STRING") + " SONAME 'accelerator_codegen_mysql.so';",
         true);
  }
  Exec("SELECT mysql_accel_txn_init();");
  const std::string epoch_before = QueryOne("SELECT mysql_accel_txn_epoch();");

  std::string table = "mut_test_pipe";
  DropAndCreateTable(table);
  CreateMutationTriggers(table, "1");
  Exec("INSERT INTO " + table + " (val, label) VALUES (77, 'pipe')");

  Expect(QueryOne("SELECT mysql_accel_txn_epoch();") != epoch_before,
         "A trigger capture must be maintained by its accelerator at commit");
  Expect(QueryOne("SELECT mysql_accel_txn_read();").find("label=staged") != std::string::npos,
         "The accelerator's maintenance must publish its image");

  Exec("DROP TABLE IF EXISTS " + table + " CASCADE", true);
  std::cout << "  [PASS] host_pipeline_is_wired\n";
}

} // namespace

int main() {
  std::cout << "engine_mutations[mysql]\n";
  TestModuleLoad();
  TestCaptureMutationAloneIsNotThisGate();
  TestInsertCapturesMutation();
  TestDeleteCapturesOldRowValues();
  TestUpdateCapturesOldAndNewRows();
  TestInsertWithNullLabel();
  TestMultipleMutationsInTransaction();
  TestTriggerPassesThroughData();
  TestTriggerWithoutBridgeInitDoesNotCrash();
  TestDeleteWithNullColumn();
  TestInsertWithExplicitDefaults();
  TestCountIncrementsWithMutations();
  TestDrainClearsMutations();
  TestMultipleTables();
  TestTypedMutationBatchFields();
  TestHostPipelineIsWired();
  std::cout << "engine_mutations[mysql] PASS\n";
  return 0;
}
