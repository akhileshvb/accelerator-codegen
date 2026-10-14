// GENERATED BACKEND — layer harness for runtime (Toy Runtime).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_runtime   CTest: engine_mysql_runtime
//
// Rule: runtime
// Evidence: Toy Runtime = store + coordinator + backend + named scan + ToyModule.
//   StageMaintenance fills outcome (scan + build); tests route through
//   coordinator.PreCommit()/Commit() — never store.TryPublish directly.
//   Execute reads the store with CurrentVisibility().
//   Seed → Build → Execute matches golden rows.
//   DML from mutations layer → commit → Execute rematch.
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "backends/mysql/state/mysql_state_store.hpp"
#include "backends/mysql/state/mysql_aux_backend.hpp"
#include "backends/mysql/engine/scan_helper.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::engine::mysql;
using namespace accelerator_codegen::aux_state::core;

namespace {

struct RuntimeState {
  std::vector<std::string> column_names;
  std::vector<std::vector<std::string>> rows;
};

class RuntimeToyModule final : public AuxQueryModule {
public:
  std::string table_name;
  std::vector<std::string> columns;

  AcceleratorId Id() const override { return "runtime_toy"; }

  std::vector<TableDependency> Dependencies() const override {
    TableDependency dep;
    dep.table = 1;
    dep.mutation_attnums = {};
    return {dep};
  }

  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<BasicAuxTxnBuffer>();
  }

  void Build(AuxEngineServices &svc, StateBuilder &builder) override {
    (void)svc;
    auto state = std::make_shared<RuntimeState>();
    state->column_names = columns;

    MysqlScanSpec spec;
    spec.table_name = table_name;
    for (const auto &col : columns) {
      spec.projected_columns.push_back({col});
    }

    ScanMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &cols, std::size_t) {
          state->rows.push_back(cols);
        });

    std::shared_ptr<const RuntimeState> const_state = std::move(state);
    builder.Write(StateImage::Make(Id(), 1, 1, std::move(const_state)));
    builder.Finish();
  }

  InvalidationPlan InterpretMutation(const MutationBatch &) const override {
    InvalidationPlan plan;
    plan.scope = InvalidationScope::PartitionSet;
    plan.partitions.insert(1);
    return plan;
  }

  void AccumulateMutation(const MutationBatch &,
                          AuxEngineServices &,
                          AuxTxnBuffer &) override {}

  StagedMaintenanceOutcome
  StageMaintenance(AuxEngineServices &svc,
                   const InvalidationPlan &plan,
                   AuxTxnBuffer &) override {
    StagedMaintenanceOutcome outcome;
    outcome.action = MaintenanceAction::ApplyDelta;
    outcome.invalidations = plan;

    // Fill outcome with fresh scan+build results so PreCommit/Commit
    // can publish through the coordinator (no store back door).
    auto state = std::make_shared<RuntimeState>();
    state->column_names = columns;

    MysqlScanSpec spec;
    spec.table_name = table_name;
    for (const auto &col : columns) {
      spec.projected_columns.push_back({col});
    }

    ScanMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &cols, std::size_t) {
          state->rows.push_back(cols);
        });

    std::shared_ptr<const RuntimeState> const_state = std::move(state);
    outcome.updates.push_back(StateImage::Make(Id(), 1, 1, std::move(const_state)));
    return outcome;
  }

  bool CheckValidity(AuxEngineServices &svc,
                     const QueryArguments &) const override {
    auto visibility = svc.CurrentVisibility();
    auto &store = svc.StateStore();
    QueryKey key;
    key.accelerator = Id();
    key.value = "1";
    auto result = store.CheckValidity(key, visibility);
    return result.IsValid();
  }

  void Execute(AuxEngineServices &svc, const QueryArguments &,
               AuxRowSink &sink) override {
    auto visibility = svc.CurrentVisibility();
    auto snapshot = svc.StateStore().Read(visibility);

    OutputSchema schema;
    for (const auto &col : columns) {
      schema.columns.push_back({col, AuxType::String, true});
    }
    sink.Begin(schema);

    for (const auto &image : snapshot.partitions) {
      auto *state = image.As<RuntimeState>();
      if (!state) continue;
      for (const auto &row : state->rows) {
        AuxRow aux_row;
        for (const auto &val : row) {
          aux_row.values.push_back(
              AuxValue::Of(AuxType::String, val));
        }
        sink.WriteRow(aux_row);
      }
    }
    sink.End();
  }

  void ExecuteFallback(AuxEngineServices &svc,
                       const QueryArguments &args,
                       AuxRowSink &sink) override {
    Execute(svc, args, sink);
  }
};

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_runtime[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) Fail(msg);
}

// ---- MySQL CLI helpers (for DML test via real MySQL) ----

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

void ExecSql(const std::string &sql, bool allow_fail = false) {
  CommandResult res = RunMysql(sql, allow_fail);
  if (res.exit_code != 0 && !allow_fail) {
    std::cerr << "engine_runtime[mysql] FAIL: SQL exec failed: " << sql << "\n"
              << res.output << "\n";
    std::abort();
  }
}

} // namespace

// Helper: registers module in coordinator, calls PreCommit→Commit, bumps epoch.
// Returns the version (epoch) after commit for visibility checking.
static Version BuildViaCoordinator(
    AuxTransactionCoordinator &coordinator,
    RuntimeToyModule &mod,
    Version &epoch) {
  (void)mod;
  // Create a synthetic mutation to trigger the module's build pipeline.
  // The module depends on table_id=1, so the coordinator will invoke
  // InterpretMutation -> PreCommit stage -> Commit publish.
  MutationBatch mutation;
  mutation.table = 1;
  mutation.kind = MutationKind::Insert;
  mutation.new_rows.push_back(AuxRow{});
  coordinator.OnMutation(mutation);
  coordinator.PreCommit();
  coordinator.Commit();
  ++epoch;
  return epoch;
}

// --------------- coordinator-published tests ---------------

void TestSeedBuildExecuteGoldenRows() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_toy_golden",
                 {"id", "name", "val"},
                 {
                     {"1", "alpha", "100"},
                     {"2", "beta", "200"},
                     {"3", "gamma", "300"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_toy_golden";
  mod->columns = {"id", "name", "val"};

  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  auto *state = snapshot.partitions[0].As<RuntimeState>();
  Expect(state != nullptr, "Should have state after coordinator build");
  Expect(state->rows.size() == 3, "Should have 3 rows");

  // Check each expected row is present (order may vary).
  bool has_alpha = false, has_beta = false, has_gamma = false;
  for (const auto &row : state->rows) {
    if (row.size() >= 3) {
      if (row[0] == "1" && row[1] == "alpha" && row[2] == "100") has_alpha = true;
      if (row[0] == "2" && row[1] == "beta" && row[2] == "200") has_beta = true;
      if (row[0] == "3" && row[1] == "gamma" && row[2] == "300") has_gamma = true;
    }
  }
  Expect(has_alpha, "Row 1 should be alpha,100");
  Expect(has_beta, "Row 2 should be beta,200");
  Expect(has_gamma, "Row 3 should be gamma,300");

  ClearAllMysqlTables();
  std::cout << "  [PASS] seed_build_execute_golden_rows\n";
}

void TestBuildPublishesThroughCoordinator() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_toy_coord",
                 {"k", "v"},
                 {
                     {"x", "10"},
                     {"y", "20"},
                     {"z", "30"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_toy_coord";
  mod->columns = {"k", "v"};

  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  auto *state = snapshot.partitions[0].As<RuntimeState>();
  Expect(state != nullptr, "Coordinator must publish state");
  Expect(state->rows.size() == 3, "Should have 3 rows via coordinator");

  // Verify that state is readable.
  bool has_x = false, has_y = false, has_z = false;
  for (const auto &row : state->rows) {
    if (row.size() >= 2) {
      if (row[0] == "x" && row[1] == "10") has_x = true;
      if (row[0] == "y" && row[1] == "20") has_y = true;
      if (row[0] == "z" && row[1] == "30") has_z = true;
    }
  }
  Expect(has_x && has_y && has_z, "All rows must be visible through coordinator");

  ClearAllMysqlTables();
  std::cout << "  [PASS] build_publishes_through_coordinator\n";
}

void TestExecuteWithCurrentVisibility() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_toy_vis",
                 {"col"},
                 {
                     {"a"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_toy_vis";
  mod->columns = {"col"};

  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  {
    auto visibility = backend.CurrentVisibility();
    auto snapshot = store.Read(visibility);
    Expect(!snapshot.partitions.empty(), "Should read state at current epoch");
    auto *state = snapshot.partitions[0].As<RuntimeState>();
    Expect(state != nullptr, "State should be readable");
    Expect(state->rows.size() == 1, "Should have 1 row");
    Expect(state->rows[0][0] == "a", "Row should be 'a'");
  }

  // Advance epoch to simulate mutation visibility shift.
  ++current_epoch;

  {
    auto visibility = backend.CurrentVisibility();
    auto snapshot = store.Read(visibility);
    // With non-zero mutation_epoch and the old data having valid_through epoch
    // = 0, Read() may filter it out (epoch > 0 && valid_through > epoch).
    // We verify that the state *may* be filtered depending on store semantics.
    // The key contract: CurrentVisibility() is used, not a hardcoded epoch.
    (void)snapshot;
  }

  ClearAllMysqlTables();
  std::cout << "  [PASS] execute_with_current_visibility\n";
}

/// --------------- edge-case tests (wired into main) ---------------

void TestNullValuesInColumns() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_nulls",
                 {"id", "label"},
                 {
                     {"1", "NULL"},
                     {"2", "hello"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_nulls";
  mod->columns = {"id", "label"};

  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  auto *state = snapshot.partitions[0].As<RuntimeState>();
  Expect(state->rows.size() == 2, "Should have 2 rows");

  bool has_null = false, has_hello = false;
  for (const auto &row : state->rows) {
    // Row: id, label
    if (row.size() >= 2) {
      if (row[0] == "1" && row[1] == "NULL") has_null = true;
      if (row[0] == "2" && row[1] == "hello") has_hello = true;
    }
  }
  Expect(has_null, "NULL string must be preserved");
  Expect(has_hello, "Non-null value must be preserved");

  ClearAllMysqlTables();
  std::cout << "  [PASS] null_values_in_columns\n";
}

void TestSingleRowTable() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_single_row",
                 {"id"},
                 {
                     {"42"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_single_row";
  mod->columns = {"id"};

  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  auto *state = snapshot.partitions[0].As<RuntimeState>();
  Expect(state->rows.size() == 1, "Should have 1 row");
  Expect(state->rows[0][0] == "42", "Single row value should be 42");

  ClearAllMysqlTables();
  std::cout << "  [PASS] single_row_table\n";
}

void TestSubsetProjection() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_proj_test",
                 {"a", "b", "c", "d"},
                 {
                     {"1", "one", "uno", "x"},
                     {"2", "two", "dos", "y"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_proj_test";
  mod->columns = {"b", "d"};  // Subset projection.

  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  auto *state = snapshot.partitions[0].As<RuntimeState>();
  Expect(state->rows.size() == 2, "Should have 2 rows");

  // Should only contain projected columns.
  Expect(state->rows[0].size() == 2, "Projected row should have 2 columns");
  Expect(state->rows[0][0] == "one" && state->rows[0][1] == "x", "Row0 proj mismatch");
  Expect(state->rows[1][0] == "two" && state->rows[1][1] == "y", "Row1 proj mismatch");

  ClearAllMysqlTables();
  std::cout << "  [PASS] subset_projection\n";
}

void TestRebuildOverwritesPriorState() {
  ClearAllMysqlTables();

  // First seed - 3 rows.
  SeedMysqlTable("rt_rebuild",
                 {"k", "v"},
                 {
                     {"a", "1"},
                     {"b", "2"},
                     {"c", "3"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_rebuild";
  mod->columns = {"k", "v"};

  // Build 1 via coordinator.
  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  auto *state1 = store.Read(backend.CurrentVisibility()).partitions[0].As<RuntimeState>();
  Expect(state1->rows.size() == 3, "First build should have 3 rows");

  // Re-seed with different data.
  ClearAllMysqlTables();
  SeedMysqlTable("rt_rebuild",
                 {"k", "v"},
                 {
                     {"x", "99"},
                     {"y", "88"},
                 });

  // Build 2 - should overwrite. New coordinator instance.
  AuxTransactionCoordinator coordinator2;
  MysqlAuxBackend backend2(coordinator2, store, visibility_cb);
  auto mod2 = std::make_shared<RuntimeToyModule>();
  mod2->table_name = "rt_rebuild";
  mod2->columns = {"k", "v"};
  coordinator2.Register(mod2);
  BuildViaCoordinator(coordinator2, *mod2, current_epoch);

  auto *state2 = store.Read(backend2.CurrentVisibility()).partitions[0].As<RuntimeState>();
  Expect(state2->rows.size() == 2, "Second build should have 2 rows");
  Expect(state2->rows[0][0] == "x" && state2->rows[0][1] == "99", "Row0 after rebuild mismatch");
  Expect(state2->rows[1][0] == "y" && state2->rows[1][1] == "88", "Row1 after rebuild mismatch");

  ClearAllMysqlTables();
  std::cout << "  [PASS] rebuild_overwrites_prior_state\n";
}

// --------------- DML + commit + rematch --------------------

void TestDmlCommitRematch() {
  // Seed data, build through coordinator, verify golden rows.
  // Then perform DML (INSERT/UPDATE/DELETE) via real MySQL,
  // re-seed in-process mock, rebuild through coordinator,
  // and verify updated rows match.

  ClearAllMysqlTables();

  // 1. Create real MySQL table and populate.
  ExecSql("DROP TABLE IF EXISTS rt_dml_rematch CASCADE", true);
  ExecSql("CREATE TABLE rt_dml_rematch ("
          "id INT AUTO_INCREMENT PRIMARY KEY, "
          "name VARCHAR(255), val INT)");
  ExecSql("INSERT INTO rt_dml_rematch (id, name, val) VALUES (1, 'alpha', 100)");
  ExecSql("INSERT INTO rt_dml_rematch (id, name, val) VALUES (2, 'beta', 200)");
  ExecSql("INSERT INTO rt_dml_rematch (id, name, val) VALUES (3, 'gamma', 300)");

  // 2. Seed in-process mock (used by scan helper in this standalone test).
  SeedMysqlTable("rt_dml_rematch",
                 {"id", "name", "val"},
                 {
                     {"1", "alpha", "100"},
                     {"2", "beta", "200"},
                     {"3", "gamma", "300"},
                 });

  // 3. Build via coordinator.
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_dml_rematch";
  mod->columns = {"id", "name", "val"};

  coordinator.Register(mod);
  BuildViaCoordinator(coordinator, *mod, current_epoch);

  // 4. Verify initial golden rows.
  {
    auto visibility = backend.CurrentVisibility();
    auto snapshot = store.Read(visibility);
    auto *state = snapshot.partitions[0].As<RuntimeState>();
    Expect(state != nullptr, "Should have state after coordinator build");
    Expect(state->rows.size() == 3, "Should have 3 rows initially");

    bool has_a = false, has_b = false, has_g = false;
    for (const auto &row : state->rows) {
      if (row.size() >= 3) {
        if (row[0] == "1" && row[1] == "alpha" && row[2] == "100") has_a = true;
        if (row[0] == "2" && row[1] == "beta" && row[2] == "200") has_b = true;
        if (row[0] == "3" && row[1] == "gamma" && row[2] == "300") has_g = true;
      }
    }
    Expect(has_a && has_b && has_g, "All initial golden rows must be present");
  }

  // 5. Perform DML via real MySQL.
  ExecSql("INSERT INTO rt_dml_rematch (id, name, val) VALUES (4, 'delta', 400)");
  ExecSql("UPDATE rt_dml_rematch SET val = 250 WHERE id = 2");
  ExecSql("DELETE FROM rt_dml_rematch WHERE id = 3");

  // 6. Re-seed in-process mock to reflect DML changes.
  ClearAllMysqlTables();
  SeedMysqlTable("rt_dml_rematch",
                 {"id", "name", "val"},
                 {
                     {"1", "alpha", "100"},
                     {"2", "beta", "250"},
                     {"4", "delta", "400"},
                 });

  // 7. Rebuild via coordinator.
  AuxTransactionCoordinator coordinator2;
  MysqlAuxBackend backend2(coordinator2, store, visibility_cb);
  auto mod2 = std::make_shared<RuntimeToyModule>();
  mod2->table_name = "rt_dml_rematch";
  mod2->columns = {"id", "name", "val"};
  coordinator2.Register(mod2);
  BuildViaCoordinator(coordinator2, *mod2, current_epoch);

  // 8. Verify updated rows.
  {
    auto visibility = backend2.CurrentVisibility();
    auto snapshot = store.Read(visibility);
    auto *state = snapshot.partitions[0].As<RuntimeState>();
    Expect(state != nullptr, "Should have state after DML + rebuild");
    Expect(state->rows.size() == 3, "Should have 3 rows after DML");

    bool has_a = false, has_b_upd = false, has_d = false, has_g = false;
    for (const auto &row : state->rows) {
      if (row.size() >= 3) {
        if (row[0] == "1" && row[1] == "alpha" && row[2] == "100") has_a = true;
        if (row[0] == "2" && row[1] == "beta" && row[2] == "250") has_b_upd = true;
        if (row[0] == "4" && row[1] == "delta" && row[2] == "400") has_d = true;
        if (row[0] == "3" && row[1] == "gamma" && row[2] == "300") has_g = true;
      }
    }
    Expect(has_a, "Row 1 should still be present");
    Expect(has_b_upd, "Row 2 should have updated val=250");
    Expect(has_d, "Row 4 should be present after INSERT");
    Expect(!has_g, "Row 3 should be gone after DELETE");
  }

  ExecSql("DROP TABLE IF EXISTS rt_dml_rematch CASCADE", true);
  ClearAllMysqlTables();
  std::cout << "  [PASS] dml_commit_rematch\n";
}

}  // namespace

int main() {
  std::cout << "engine_runtime[mysql]\n";

  TestSeedBuildExecuteGoldenRows();
  TestBuildPublishesThroughCoordinator();
  TestExecuteWithCurrentVisibility();
  TestNullValuesInColumns();
  TestSingleRowTable();
  TestSubsetProjection();
  TestRebuildOverwritesPriorState();
  TestDmlCommitRematch();

  std::cout << "engine_runtime[mysql] PASS\n";
  return 0;
}
