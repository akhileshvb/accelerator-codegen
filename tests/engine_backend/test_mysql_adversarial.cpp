// VALIDATION-OWNED — engine ↔ substrate ADVERSARIAL harness for mysql.
// Spec: pipeline/prompts/engine_backend/adversarial_tests.md
// Build: test_mysql_adversarial   CTest: engine_mysql_adversarial
//
// Unseen by implementation layer harnesses. Cases below are not copies of
// test_mysql_{store,aux_backend,txn,mutations,scan,runtime,visibility,host}.cpp.
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
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_state_store.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::engine::mysql;
using namespace accelerator_codegen::aux_state::core;

namespace {

struct ToyPayload {
  int count;
  std::string label;
};

class AdvToyModule final : public AuxQueryModule {
public:
  AcceleratorId Id() const override { return "adv_toy"; }

  std::vector<TableDependency> Dependencies() const override {
    TableDependency dep;
    dep.table = 1;
    return {dep};
  }

  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<BasicAuxTxnBuffer>();
  }

  void Build(AuxEngineServices &, StateBuilder &builder) override {
    auto payload = std::make_shared<const ToyPayload>(ToyPayload{1, "built"});
    builder.Write(StateImage::Make(Id(), 1, 1, payload));
    builder.Finish();
  }

  InvalidationPlan InterpretMutation(const MutationBatch &) const override {
    InvalidationPlan plan;
    plan.scope = InvalidationScope::PartitionSet;
    plan.partitions.insert(1);
    return plan;
  }

  void AccumulateMutation(const MutationBatch &, AuxEngineServices &,
                          AuxTxnBuffer &) override {}

  StagedMaintenanceOutcome StageMaintenance(AuxEngineServices &,
                                            const InvalidationPlan &,
                                            AuxTxnBuffer &) override {
    StagedMaintenanceOutcome outcome;
    outcome.action = MaintenanceAction::ApplyDelta;
    auto payload = std::make_shared<const ToyPayload>(ToyPayload{7, "adv"});
    outcome.updates.push_back(StateImage::Make(Id(), 1, 1, payload));
    return outcome;
  }

  bool CheckValidity(AuxEngineServices &, const QueryArguments &) const override {
    return true;
  }

  void Execute(AuxEngineServices &, const QueryArguments &,
               AuxRowSink &) override {}

  void ExecuteFallback(AuxEngineServices &, const QueryArguments &,
                       AuxRowSink &) override {}
};

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

MutationBatch MakeInsert(TableId table) {
  MutationBatch batch;
  batch.table = table;
  batch.kind = MutationKind::Insert;
  AuxRow row;
  row.values.push_back(AuxValue::Of(AuxType::Int64, int64_t{1}));
  batch.new_rows.push_back(row);
  return batch;
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

// 3. Store: empty TryPublish is a successful no-op (no image).
void TestEmptyPublishIsNoop() {
  MysqlStateStore st;
  StagedMaintenanceOutcome staged;
  staged.action = MaintenanceAction::ApplyDelta;
  Expect(st.TryPublish(staged, 0), "Empty TryPublish must return true");
  auto snap = st.Read(VisibilityToken{});
  Expect(!snap.available && snap.partitions.empty(),
         "Empty publish must not create a readable image");
  auto vr = st.CheckValidity({"nobody", "1"}, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing,
         "Empty store CheckValidity is Missing");
  std::cout << "  [PASS] empty_publish_noop\n";
}

// 4. Store: matching expected_version overwrite replaces payload.
void TestMatchingVersionOverwrite() {
  MysqlStateStore st;
  auto p1 = std::make_shared<const ToyPayload>(ToyPayload{1, "first"});
  StagedMaintenanceOutcome a;
  a.updates.push_back(StateImage::Make("adv_ov", 4, 1, p1));
  Expect(st.TryPublish(a, 0), "First publish");
  auto v1 = st.CheckValidity({"adv_ov", "4"}, VisibilityToken{});
  Expect(v1.code == ValidityCode::Valid, "First image Valid");
  Version seen = v1.visible_version;

  auto p2 = std::make_shared<const ToyPayload>(ToyPayload{2, "second"});
  StagedMaintenanceOutcome b;
  b.updates.push_back(StateImage::Make("adv_ov", 4, 2, p2));
  Expect(st.TryPublish(b, seen), "Overwrite with matching expected_version");
  auto snap = st.Read(VisibilityToken{});
  Expect(snap.partitions.size() == 1, "Still one partition");
  auto *tp = snap.partitions[0].As<ToyPayload>();
  Expect(tp && tp->count == 2 && tp->label == "second",
         "Overwrite must publish the new payload");
  std::cout << "  [PASS] matching_version_overwrite\n";
}

// 5. Visibility: token behind publish is NotVisible; Invalidate then Invalidated
// even for a current-epoch token (layer store tests do not combine both).
void TestBehindTokenThenInvalidate() {
  auto epoch_fn = []() -> Version { return 40; };
  MysqlStateStore st(epoch_fn);
  auto payload = std::make_shared<const ToyPayload>(ToyPayload{9, "vis"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("adv_vis", 3, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish epoch 40");

  VisibilityToken behind;
  behind.mutation_epoch = 39;
  Expect(st.Read(behind).partitions.empty(),
         "Token 39 must omit epoch-40 image");
  auto vr_behind = st.CheckValidity({"adv_vis", "3"}, behind);
  Expect(vr_behind.code == ValidityCode::NotVisible,
         "Token behind publish is NotVisible");

  InvalidationPlan plan;
  plan.scope = InvalidationScope::PartitionSet;
  plan.partitions.insert(3);
  st.Invalidate(plan);

  VisibilityToken current;
  current.mutation_epoch = 40;
  auto vr_now = st.CheckValidity({"adv_vis", "3"}, current);
  Expect(vr_now.code == ValidityCode::Invalidated,
         "After invalidate, current token is Invalidated not Valid");
  Expect(st.Read(current).partitions.empty(),
         "Read after invalidate omits the slot");
  std::cout << "  [PASS] behind_token_then_invalidate\n";
}

// 6. Aux: mutation for a table the toy module does not depend on must not
// stage/publish (layer aux tests only mutate table 1).
void TestUnrelatedTableDoesNotPublish() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  auto module = std::make_shared<AdvToyModule>();
  coordinator.Register(module);
  MysqlAuxBackend backend(coordinator, store, []() {
    VisibilityToken tok;
    tok.mutation_epoch = 0;
    return tok;
  });

  backend.CaptureMutation(MakeInsert(99));
  Expect(!coordinator.HasPendingWork(),
         "Unrelated table_id must not create pending work");
  backend.OnPreCommit();
  backend.OnCommit();
  auto snap = store.Read(VisibilityToken{});
  Expect(!snap.available, "Unrelated-table commit must not publish");
  std::cout << "  [PASS] unrelated_table_no_publish\n";
}

// 7. Aux: abort drops the buffered mutation; a later capture+commit publishes
// only the new txn (no leaked abort image).
void TestAbortDoesNotLeakIntoLaterCommit() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  auto module = std::make_shared<AdvToyModule>();
  coordinator.Register(module);
  MysqlAuxBackend backend(coordinator, store, []() {
    VisibilityToken tok;
    tok.mutation_epoch = 0;
    return tok;
  });

  backend.CaptureMutation(MakeInsert(1));
  Expect(coordinator.HasPendingWork(), "First capture pending");
  backend.OnAbort();
  Expect(!coordinator.HasPendingWork(), "Abort clears pending work");
  Expect(!store.Read(VisibilityToken{}).available, "Abort did not publish");

  backend.CaptureMutation(MakeInsert(1));
  backend.OnPreCommit();
  backend.OnCommit();
  auto snap = store.Read(VisibilityToken{});
  Expect(snap.available, "Fresh txn after abort must publish");
  auto *tp = snap.partitions.empty() ? nullptr : snap.partitions[0].As<ToyPayload>();
  Expect(tp && tp->label == "adv", "Published image is the post-abort commit");
  std::cout << "  [PASS] abort_does_not_leak\n";
}

// 8. SQL: capture of table 99 then COMMIT leaves the txn toy store empty.
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

// 9. SQL: capture outside START TRANSACTION stays unpublished until COMMIT
// (audit hook keys off sql_command=commit, not autocommit SELECT).
void TestSqlCaptureNeedsExplicitCommit() {
  EnsureTxnAndRuntimeUdfs();
  Exec("SELECT mysql_accel_txn_reset();", true);
  Exec("SELECT mysql_accel_txn_init();");
  Exec("SELECT mysql_accel_txn_capture(1, 0, 1, 'autocommit_window');");
  std::string before = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(before.find("available=1") == std::string::npos,
         "Capture without COMMIT must not publish");
  Exec("COMMIT;");
  std::string after = QueryOne("SELECT mysql_accel_txn_read();");
  Expect(after.find("available=1") != std::string::npos,
         "Explicit COMMIT after autocommit capture must publish");
  std::cout << "  [PASS] sql_capture_needs_explicit_commit\n";
}

// 10. Host UDFs fail closed: execute/read without init do not crash and do
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
  TestEmptyPublishIsNoop();
  TestMatchingVersionOverwrite();
  TestBehindTokenThenInvalidate();
  TestUnrelatedTableDoesNotPublish();
  TestAbortDoesNotLeakIntoLaterCommit();
  TestSqlWrongTableCaptureDoesNotPublish();
  TestSqlCaptureNeedsExplicitCommit();
  TestRuntimeUdfsFailClosedWithoutInit();
  std::cout << "engine_adversarial[mysql] PASS\n";
  return 0;
}
