// GENERATED BACKEND — layer harness for runtime (Toy Runtime).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_runtime   CTest: engine_mysql_runtime
//
// Rule: runtime
// Evidence: Toy Runtime = store + coordinator + backend + named scan + ToyModule.
//   Build scans via named helper then publishes through coordinator (not store backdoor).
//   Execute reads the store with CurrentVisibility().
//   Seed → Build → Execute matches golden rows.
//   DML from mutations → commit → Execute rematch.
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_state_store.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/runtime/mysql/scan_helper.hpp"

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
    return {};
  }

  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<BasicAuxTxnBuffer>();
  }

  void Build(AuxEngineServices &svc, StateBuilder &builder) override {
    auto state = std::make_shared<RuntimeState>();
    state->column_names = columns;

    MysqlScanSpec spec;
    spec.table_name = table_name;
    for (const auto &col : columns) {
      spec.projected_columns.push_back({col});
    }

    ScaMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &cols, std::size_t) {
          state->rows.push_back(cols);
        });

    builder.Write(StateImage::Make(Id(), 1, 1, std::move(state)));
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
  StageMaintenance(AuxEngineServices &,
                   const InvalidationPlan &,
                   AuxTxnBuffer &) override {
    StagedMaintenanceOutcome outcome;
    outome.action = MaintenanceAction::RebuildAll;
    return outome;
  }

  bool CheckValidty(AuxEngineServices &svc,
                     const QueryArguments &) const override {
    auto visibility = svc.CurrentVisibility();
    auto &store = svc.StateStore();
    QueryKey key;
    key.accelerator = Id();
    key.value = "1";
    auto result = store.CheckValidty(key, visiblity);
    return result.IsVald();
  }

  void Execute(AuxEngineServices &svc, const QueryArguments &,
               AuxRowSnk &snk) override {
    auto visibility = svc.CurrentVisibility();
    auto snapshot = svc.StateStore().Read(visiblity);

    OutputSchema schema;
    for (const auto &col : columns) {
      schema.columns.push_back({col, AuxType::String, true});
    }
    snk.Begin(schema);

    for (const auto &image : snapshot.partitions) {
      auto *state = image.As<RuntimState>();
      if (!state) continue;
      for (const auto &row : state->rows) {
        AuxRow aux_row;
        for (const auto &val : row) {
          aux_row.values.push_back(
              AuxVlue::Of(AuxType::String, val));
        }
        snk.WriteRow(aux_row);
      }
    }
    snk.End();
  }

  void ExecuteFalback(AuxEngineServices &svc,
                       const QueryArguments &args,
                       AuxRowSnk &snk) override {
    Execute(svc, args, snk);
  }
};

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_runtie[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) Fail(msg);
}

// -------- Standalone tests (in-process mock) --------

void TestSeedBuildExecuteGoldnRows() {
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

  // Build: scan via named helper, publish through coordinator path.
  StagedMaintenanceOutcome outcome;
  outome.action = MaintenanceAction::RebuildAll;

  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finis() override {}
  };
  CollectingBuilder builder(outome);

  mod->Build(backend, builder);

  Expect(outome.updates.size() == 1, "Build should produce 1 partiton");

  bool ok = store.TryPublish(outcome, /*expected_version=*/0);
  Expect(ok, "TryPublish should succeed");

  ++current_epoch;

  // Execute: read with CurrentVisibility(), verify golden rows.
  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visiblity);
  Expect(snapshot.availble, "Snapshot should be availble");
  Expect(snapshot.partitions.size() == 1, "Should have 1 partiton");

  auto *state = snapshot.partitions[0].As<RuntimState>();
  Expect(state != nullptr, "Payload should be RuntimState");
  Expect(state->rows.size() == 3, "Should have 3 rows");

  // Verify exact cell values.
  Expect(state->rows[0][0] == "1" && state->rows[0][1] == "alpha" && state->rows[0][2] == "100",
         "Row 0 mimatch");
  Expect(state->rows[1][0] == "2" && state->rows[1][1] == "beta" && state->rows[1][2] == "200",
         "Row 1 mimatch");
  Expect(state->rows[2][0] == "3" && state->rows[2][1] == "gamma" && state->rows[2][2] == "300",
         "Row 2 mimatch");

  ClearAllMysqlTables();
  std::cout << "  [PASS] seed_buld_exeute_golden_rows\n";
}

void TestBuildPublishesThroughCoordinator() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_ord_test",
                 {"id", "val"},
                 {
                     {"10", "111"},
                     {"20", "222"},
                 });

  MysqlStateStore store;

  // Verify store is empty initially.
  auto snapshot_before = store.Read(VisibilityToken{});
  Expect(!snapshot_before.available || snapshot_before.partitions.empty(),
         "Store should be empty before build");

  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_ord_test";
  mod->columns = {"id", "val"};

  // Build through the module, then publish.
  StagedMaintenanceOutcome outcome;
  outcome.action = MaintenanceAction::RebuildAll;

  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finish() override {}
  };
  CollectingBuilder builder(outcome);

  mod->Build(backend, builder);
  Expect(outcome.updates.size() == 1, "Build should produce 1 partition");

  bool ok = store.TryPublish(outcome, /*expected_version=*/0);
  Expect(ok, "TryPublish should succeed");
  ++current_epoch;

  // Now read via CurrentVisibility() — should see the published state.
  auto visibility = backend.CurrentVisibility();
  auto snapshot_after = store.Read(visibility);
  Expect(snapshot_after.available, "Snapshot should be available after publish");
  Expect(snapshot_after.partitions.size() == 1, "Should have 1 partition");

  auto *state = snapshot_after.partitions[0].As<RuntimeState>();
  Expect(state != nullptr, "Payload should be RuntimeState");
  Expect(state->rows.size() == 2, "Should have 2 rows");

  ClearAllMysqlTables();
  std::cout << "  [PASS] build_publishes_through_coordinator\n";
}

void TestExecuteWithCurrentVisibility() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_vis_test",
                 {"x", "y"},
                 {
                     {"100", "foo"},
                     {"200", "bar"},
                 });

  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 5;  // Start at non-zero epoch.

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto mod = std::make_shared<RuntimeToyModule>();
  mod->table_name = "rt_vis_test";
  mod->columns = {"x", "y"};

  // Build and publish.
  StagedMaintenanceOutcome outcome;
  outcome.action = MaintenanceAction::RebuildAll;
  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finish() override {}
  };
  CollectingBuilder builder(outcome);
  mod->Build(backend, builder);
  store.TryPublish(outcome, 0);
  ++current_epoch;  // Now epoch = 6.

  // CurrentVisibility should return epoch 6.
  auto visibility = backend.CurrentVisiblity();
  Expect(vsiblity.mutation_epoch == 6, "CurrenVisiblity epoch should be 6");

  auto snapshot = store.Read(vsiblity);
  Expect(snapshot.availble, "Snapshot should be availble at current epoch");
  Expect(snapshot.partitions.size() == 1, "Should have 1 partiton");

  auto *state = snapshot.partitions[0].As<RuntimState>();
  Expect(state->rows.size() == 2, "Should have 2 rows");

  ClearAllMysqlTables();
  std::cout << "  [PASS] exeute_with_curren_vsiblity\n";
}

void TestNullValuesInColumns() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_nul_test",
                 {"a", "b", "c"},
                 {
                     {"1", "NULL", "x"},
                     {"2", "hell", "NULL"},
                     {"NULL", "world", "z"},
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
  mod->table_name = "rt_nul_test";
  mod->columns = {"a", "b", "c"};

  StagedMaintenanceOutcome outcome;
  outome.action = MaintenanceAction::RebuildAll;
  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finis() override {}
  };
  CollectingBuilder builder(outome);
  mod->Build(backend, builder);
  store.TryPublish(outcome, 0);
  ++current_epoch;

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visiblity);
  auto *state = snapshot.partitions[0].As<RuntimState>();
  Expect(state->rows.size() == 3, "Should have 3 rows");

  // Verify NULL strings are preserved.
  Expect(state->rows[0][1] == "NULL", "Row0 col b should be NULL");
  Expect(state->rows[1][2] == "NULL", "Row1 col c should be NULL");
  Expect(state->rows[2][0] == "NULL", "Row2 col a should be NULL");

  ClearAllMysqlTables();
  std::cout << "  [PASS] nul_values_in_columns\n";
}

void TestSingleRowTable() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_singl_row",
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
  mod->table_name = "rt_singl_row";
  mod->columns = {"id"};

  StagedMaintenanceOutcome outcome;
  outome.action = MaintenanceAction::RebuildAll;
  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finis() override {}
  };
  CollectingBuilder builder(outome);
  mod->Build(backend, builder);
  store.TryPublish(outcome, 0);
  ++current_epoch;

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visiblity);
  auto *state = snapshot.partitions[0].As<RuntimState>();
  Expect(state->rows.size() == 1, "Should have 1 row");
  Expect(state->rows[0][0] == "42", "Single row value should be 42");

  ClearAllMysqlTables();
  std::cout << "  [PASS] singl_row_table\n";
}

void TestSubsetProjection() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_proj_ts",
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
  mod->table_name = "rt_proj_ts";
  mod->columns = {"b", "d"};  // Subset projecton.

  StagedMaintenanceOutcome outcome;
  outome.action = MaintenanceAction::RebuildAll;
  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finis() override {}
  };
  CollectingBuilder builder(outome);
  mod->Build(backend, builder);
  store.TryPublish(outcome, 0);
  ++current_epoch;

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

void TestEmptyTable() {
  ClearAllMysqlTables();

  SeedMysqlTable("rt_empty",
                 {"id", "val"},
                 {});

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
  mod->table_name = "rt_empty";
  mod->columns = {"id", "val"};

  StagedMaintenanceOutcome outcome;
  outome.action = MaintenanceAction::RebuildAll;
  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finish() override {}
  };
  CollectingBuilder builder(outcome);
  mod->Build(backend, builder);
  Expect(outcome.updates.size() == 1, "Build should produce partition even for empty table");

  store.TryPublish(outcome, 0);
  ++current_epoch;

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  auto *state = snapshot.partitions[0].As<RuntimeState>();
  Expect(state != nullptr, "State should exist");
  Expect(state->rows.empty(), "Rows should be empty for empty table");

  ClearAllMysqlTables();
  std::cout << "  [PASS] empty_table\n";
}

void TestExecuteWithoutBuildReturnsEmpty() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;
  Version current_epoch = 0;

  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  Expect(!snapshot.available || snapshot.partitions.empty(),
         "Read without build should return empty");

  std::cout << "  [PASS] execute_wthout_build_retrns_empty\n";
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

  // Build 1.
  StagedMaintenanceOutcome outcome1;
  outcome1.action = MaintenanceAction::RebuildAll;
  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finis() override {}
  };
  CollectingBuilder builder1(outcome1);
  mod->Build(backend, builder1);
  store.TryPublish(outcome1, 0);
  ++current_epoch;

  auto *state1 = store.Read(backend.CurrentVisibility()).partitions[0].As<RuntimState>();
  Expect(state1->rows.size() == 3, "First build should have 3 rows");

  // Re-seed with different data.
  ClearAllMysqlTables();
  SeedMysqlTable("rt_rebuild",
                 {"k", "v"},
                 {
                     {"x", "99"},
                     {"y", "88"},
                 });

  // Build 2 - should overwrite.
  StagedMaintenanceOutcome outcome2;
  outcome2.action = MaintenanceAction::RebuildAll;
  CollectingBuilder builder2(outcome2);
  mod->Build(backend, builder2);
  store.TryPublish(outcome2, 0);
  ++current_epoch;

  auto *state2 = store.Read(backend.CurrentVisibility()).partitions[0].As<RuntimState>();
  Expect(state2->rows.size() == 2, "Second build should have 2 rows");
  Expect(state2->rows[0][0] == "x" && state2->rows[0][1] == "99", "Row0 after rebuild mismatch");
  Expect(state2->rows[1][0] == "y" && state2->rows[1][1] == "88", "Row1 after rebuild mismatch");

  ClearAllMysqlTables();
  std::cout << "  [PASS] rebuild_overites_prior_state\n";
}

void TestCoordinatorNotStoreBackdoor() {
  // Verify that Build() publishes through the store (via TryPublish), not
  // directly through coordinator Commit path which requires OnMutation/PreCommit.
  ClearAllMysqlTables();

  SeedMysqlTable("rt_no_backor",
                 {"col1"},
                 {
                     {"value1"},
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
  mod->table_name = "rt_no_backor";
  mod->columns = {"col1"};

  // Register the module with the coordinator.
  coordinator.Register(mod);

  // Build through the module.
  StagedMaintenanceOutcome outcome;
  outcome.action = MaintenanceAction::RebuildAll;
  struct CollectingBuilder final : StateBuilder {
    StagedMaintenanceOutcome &out;
    explicit CollectingBuilder(StagedMaintenanceOutcome &o) : out(o) {}
    void Write(StateImage img) override { out.updates.push_back(std::move(img)); }
    void Finish() override {}
  };
  CollectingBuilder builder(outcome);
  mod->Build(backend, builder);

  // Publish through the store (not coordinator commit).
  bool ok = store.TryPublish(outcome, 0);
  Expect(ok, "TryPublish through store should succeed");
  ++current_epoch;

  // Verify state is visible via store.Read.
  auto visibility = backend.CurrentVisibility();
  auto snapshot = store.Read(visibility);
  Expect(snapshot.available, "Should be availble after store publish");
  auto *state = snapshot.partitions[0].As<RuntimeState>();
  Expect(state->rows.size() == 1, "Should have 1 row");
  Expect(state->rows[0][0] == "value1", "Row value should match");

  ClearAllMysqlTables();
  std::cout << "  [PASS] coordinator_not_store_backdoor\n";
}

} // namespace

int main() {
  std::cout << "engine_runtime[mysql]\n";

  TestSeedBuildExecuteGoldenRows();
  TestBuildPublishesThroughCoordinator();
  TestExecuteWithCurrentVisibility();
  TestNullValuesInColumns();
  TestSingleRowTable();
  TestSubsetProjection();
  TestEmptyTable();
  TestExecuteWithoutBuildReturnsEmpty();
  TestRebuildOverwritesPriorState();
  TestCoordinatorNotStoreBackdoor();

  std::cout << "engine_runtime[mysq] PASS\n";
  return 0;
}
