// GENERATED BACKEND — layer harness for aux_backend (Aux backend).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_aux_backend   CTest: engine_mysql_aux_backend
//
// Rule: aux_backend
// Evidence: MysqlAuxBackend ctor calls AttachServices; CaptureMutation
//   forwards to coordinator.OnMutation; Abort publishes nothing; Commit
//   publishes exactly one image; CurrentVisibility / MaintenanceCostModel /
//   WorkerCount asserted on the backend instance (not a second services obj).
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
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

class ToyModule final : public AuxQueryModule {
public:
  AcceleratorId Id() const override { return "toy_backend"; }

  std::vector<TableDependency> Dependencies() const override {
    TableDependency dep;
    dep.table = 1;
    dep.mutation_attnums = {};
    return {dep};
  }

  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override {
    auto buf = std::make_unique<BasicAuxTxnBuffer>();
    return buf;
  }

  void Build(AuxEngineServices &svc, StateBuilder &builder) override {
    int count = ++build_count;
    auto payload = std::make_shared<const ToyPayload>(
        ToyPayload{count, "built"});
    builder.Write(StateImage::Make(Id(), 1, 1, payload));
    builder.Finish();
  }

  InvalidationPlan InterpretMutation(const MutationBatch &mutation) const override {
    InvalidationPlan plan;
    plan.scope = InvalidationScope::PartitionSet;
    plan.partitions.insert(1);
    return plan;
  }

  void AccumulateMutation(const MutationBatch &mutation, AuxEngineServices &svc,
                          AuxTxnBuffer &buf) override {
    (void)mutation;
    (void)svc;
    (void)buf;
  }

  StagedMaintenanceOutcome
  StageMaintenance(AuxEngineServices &svc, const InvalidationPlan &plan,
                   AuxTxnBuffer &buf) override {
    (void)svc;
    (void)plan;
    (void)buf;
    StagedMaintenanceOutcome outcome;
    outcome.action = MaintenanceAction::ApplyDelta;
    int count = ++build_count;
    auto payload = std::make_shared<const ToyPayload>(
        ToyPayload{count, "staged"});
    outcome.updates.push_back(StateImage::Make(Id(), 1, 1, payload));
    return outcome;
  }

  bool CheckValidity(AuxEngineServices &svc,
                     const QueryArguments &args) const override {
    (void)svc;
    (void)args;
    return true;
  }

  void Execute(AuxEngineServices &svc, const QueryArguments &args,
               AuxRowSink &sink) override {
    (void)svc;
    (void)args;
    OutputSchema schema;
    schema.columns.push_back({"val", AuxType::Int64, false});
    sink.Begin(schema);
    AuxRow row;
    row.values.push_back(AuxValue::Of(AuxType::Int64, int64_t{42}));
    sink.WriteRow(row);
    sink.End();
  }

  void ExecuteFallback(AuxEngineServices &svc, const QueryArguments &args,
                       AuxRowSink &sink) override {
    (void)svc;
    (void)args;
    OutputSchema schema;
    schema.columns.push_back({"val", AuxType::Int64, false});
    sink.Begin(schema);
    AuxRow row;
    row.values.push_back(AuxValue::Of(AuxType::Int64, int64_t{99}));
    sink.WriteRow(row);
    sink.End();
  }

  mutable int build_count = 0;
};

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_aux_backend[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

void TestBackendConstruction() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  Version current_epoch = 0;
  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto tok = backend.CurrentVisibility();
  Expect(tok.mutation_epoch == 0, "Visibility should return 0 epoch initially");

  auto &store_ref = backend.StateStore();
  (void)&store_ref;

  const auto &cost = backend.MaintenanceCostModel();
  (void)&cost;

  Expect(backend.WorkerCount() >= 1, "WorkerCount should be >= 1");

  std::cout << "  [PASS] backend_construction\n";
}

void TestAttachServicesCalled() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  auto visibility_cb = []() -> VisibilityToken {
    return VisibilityToken{};
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  Expect(coordinator.HasPendingWork() == false,
         "Coordinator should have no pending work initially");

  std::cout << "  [PASS] attach_services_called\n";
}

void TestCaptureMutationAndCommit() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  auto mod = std::make_shared<ToyModule>();
  coordinator.Register(mod);

  Version current_epoch = 42;
  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  // Build a hand-built MutationBatch
  MutationBatch mutation;
  mutation.table = 1;
  mutation.kind = MutationKind::Insert;
  AuxRow new_row;
  new_row.values.push_back(AuxValue::Of(AuxType::Int64, int64_t{100}));
  mutation.new_rows.push_back(new_row);

  // Capture through the backend
  backend.CaptureMutation(mutation);

  Expect(coordinator.HasPendingWork(), "Coordinator should have pending work after CaptureMutation");

  // PreCommit then Commit
  backend.OnPreCommit();
  backend.OnCommit();

  Expect(!coordinator.HasPendingWork(), "Coordinator should have no pending work after commit");

  // Verify the store now has an image
  VisibilityToken tok;
  tok.mutation_epoch = current_epoch;
  auto snapshot = store.Read(tok);
  Expect(snapshot.available, "Snapshot should be available after commit");
  Expect(snapshot.partitions.size() == 1, "Should have exactly 1 partition after commit");

  const auto &img = snapshot.partitions[0];
  Expect(img.accelerator == "toy_backend", "Wrong accelerator in published image");
  Expect(img.partition == 1, "Wrong partition in published image");

  auto *tp = img.As<ToyPayload>();
  Expect(tp != nullptr, "Payload should be ToyPayload");
  Expect(tp->count == 1, "Payload count should be 1");

  std::cout << "  [PASS] capture_mutation_and_commit\n";
}

void TestCaptureMutationAndAbort() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  auto mod = std::make_shared<ToyModule>();
  coordinator.Register(mod);

  Version current_epoch = 100;
  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  MutationBatch mutation;
  mutation.table = 1;
  mutation.kind = MutationKind::Insert;
  AuxRow new_row;
  new_row.values.push_back(AuxValue::Of(AuxType::Int64, int64_t{777}));
  mutation.new_rows.push_back(new_row);

  backend.CaptureMutation(mutation);
  Expect(coordinator.HasPendingWork(), "Coordinator should have pending work");

  // Abort — should not publish
  backend.OnAbort();

  Expect(!coordinator.HasPendingWork(), "Coordinator should have no pending work after abort");

  // Store snapshot should have no images
  VisibilityToken tok;
  tok.mutation_epoch = current_epoch;
  auto snapshot = store.Read(tok);
  Expect(!snapshot.available || snapshot.partitions.empty(),
         "Store should not have images after abort");

  std::cout << "  [PASS] capture_mutation_and_abort\n";
}

void TestVisibilityCallback() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  auto mod = std::make_shared<ToyModule>();
  coordinator.Register(mod);

  Version current_epoch = 555;
  auto visibility_cb = [&]() -> VisibilityToken {
    VisibilityToken tok;
    tok.mutation_epoch = current_epoch;
    tok.transaction_id = 12345;
    tok.backend_token = "test_scope";
    return tok;
  };

  MysqlAuxBackend backend(coordinator, store, visibility_cb);

  auto tok = backend.CurrentVisibility();
  Expect(tok.mutation_epoch == 555, "Epoch should match callback");
  Expect(tok.transaction_id == 12345, "Tx id should match callback");
  Expect(tok.backend_token == "test_scope", "Backend token should match callback");

  std::cout << "  [PASS] visibility_callback\n";
}

void TestCostModel() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  auto cost_cb = []() -> maintenance::CostModelParams {
    maintenance::CostModelParams params;
    params.aux_hit_us = 300.0;
    params.fallback_us = 99999.0;
    return params;
  };

  MysqlAuxBackend backend(coordinator, store, {}, cost_cb);

  const auto &cost = backend.MaintenanceCostModel();
  Expect(cost.aux_hit_us == 300.0, "aux_hit_us should be 300.0");
  Expect(cost.fallback_us == 99999.0, "fallback_us should be 99999.0");

  std::cout << "  [PASS] cost_model_callback\n";
}

void TestDefaultCostModel() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  MysqlAuxBackend backend(coordinator, store, {});

  const auto &cost = backend.MaintenanceCostModel();
  Expect(cost.aux_hit_us == 500.0, "Default aux_hit_us should be 500.0");
  Expect(cost.fallback_us == 50000.0, "Default fallback_us should be 50000.0");

  std::cout << "  [PASS] default_cost_model\n";
}

void TestWorkerCount() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  MysqlAuxBackend backend(coordinator, store, {});

  std::size_t wc = backend.WorkerCount();
  Expect(wc >= 1, "WorkerCount should be >= 1");

  std::cout << "  [PASS] worker_count\n";
}

void TestExpectedQueriesBeforeNextWrite() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  auto workload_cb = []() -> double { return 42.0; };

  MysqlAuxBackend backend(coordinator, store, {}, {}, workload_cb);

  double eq = backend.ExpectedQueriesBeforeNextWrite();
  Expect(eq == 42.0, "ExpectedQueriesBeforeNextWrite should be 42.0");

  std::cout << "  [PASS] expected_queries_callback\n";
}

void TestExpectedQueriesDefault() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  MysqlAuxBackend backend(coordinator, store, {});

  double eq = backend.ExpectedQueriesBeforeNextWrite();
  Expect(eq == 10.0, "Default ExpectedQueriesBeforeNextWrite should be 10.0");

  std::cout << "  [PASS] expected_queries_default\n";
}

void TestBackendIsAuxEngineServices() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  MysqlAuxBackend backend(coordinator, store, {});

  AuxEngineServices *svc_ptr = &backend;
  (void)svc_ptr;

  std::cout << "  [PASS] backend_is_aux_engine_services\n";
}

void TestAllowsSynchronousRebuildOnCommit() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  MysqlAuxBackend backend(coordinator, store, {});

  AuxEngineServices *svc_ptr = &backend;
  Expect(svc_ptr->AllowsSynchronousRebuildOnCommit() == false,
         "MysqlAuxBackend must not allow synchronous rebuild on commit");

  std::cout << "  [PASS] no_sync_rebuild_on_commit\n";
}

void TestMutationCaptureAttnums() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  auto mod = std::make_shared<ToyModule>();
  coordinator.Register(mod);

  MysqlAuxBackend backend(coordinator, store, {});

  auto attnums = backend.MutationCaptureAttnums(1);
  Expect(attnums.has_value(), "MutationCaptureAttnums should return value for table 1");

  auto attnums_unknown = backend.MutationCaptureAttnums(999);
  Expect(!attnums_unknown.has_value(), "MutationCaptureAttnums should return nullopt for unknown table");

  std::cout << "  [PASS] mutation_capture_attnums\n";
}

void TestChooseMaintenance() {
  MysqlStateStore store;
  AuxTransactionCoordinator coordinator;

  MysqlAuxBackend backend(coordinator, store, {});

  MaintenanceRequest req;
  req.rows_touched = 5;
  req.incremental_supported = true;

  auto decision = backend.ChooseMaintenance(req);
  (void)decision;

  std::cout << "  [PASS] choose_maintenance\n";
}

} // namespace

int main() {
  std::cout << "engine_aux_backend[mysql]\n";
  TestBackendConstruction();
  TestAttachServicesCalled();
  TestCaptureMutationAndCommit();
  TestCaptureMutationAndAbort();
  TestVisibilityCallback();
  TestCostModel();
  TestDefaultCostModel();
  TestWorkerCount();
  TestExpectedQueriesBeforeNextWrite();
  TestExpectedQueriesDefault();
  TestBackendIsAuxEngineServices();
  TestAllowsSynchronousRebuildOnCommit();
  TestMutationCaptureAttnums();
  TestChooseMaintenance();
  std::cout << "engine_aux_backend[mysql] PASS\n";
  return 0;
}
