#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory>
#include <string>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "backends/duckdb/state/duckdb_state_store.hpp"
#include "eval/tests/test_support/module_runtime.hpp"
#include "duckdb/main/connection.hpp"

using namespace accelerator_codegen::aux_state::core;

namespace {

class TestStore final : public AuxStateStore {
public:
  StateSnapshot Read(const VisibilityToken &visibility) const override {
    StateSnapshot snapshot;
    snapshot.accelerator = "test";
    snapshot.visibility = visibility;
    snapshot.partitions = images;
    snapshot.available = !images.empty();
    return snapshot;
  }

  ValidityResult CheckValidity(const QueryKey &,
                               const VisibilityToken &) const override {
    return {images.empty() ? ValidityCode::Missing : ValidityCode::Valid,
            images.empty() ? 0 : images[0].version};
  }

  void Invalidate(const InvalidationPlan &) override {
    ++invalidations;
    images.clear();
  }

  bool TryPublish(const StagedMaintenanceOutcome &outcome, Version) override {
    images = outcome.updates;
    ++publishes;
    return true;
  }

  std::vector<StateImage> images;
  int invalidations = 0;
  int publishes = 0;
};

class TestServices final : public AuxEngineServices {
public:
  VisibilityToken CurrentVisibility() const override { return visibility; }

  AuxStateStore &StateStore() override { return store; }

  VisibilityToken visibility;
  TestStore store;
};

class TestModule final : public AuxQueryModule {
public:
  AcceleratorId Id() const override { return "test"; }

  std::vector<TableDependency> Dependencies() const override {
    return {{7, {1}}};
  }

  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<BasicAuxTxnBuffer>();
  }

  void Build(AuxEngineServices &, StateBuilder &builder) override {
    builder.Finish();
  }

  InvalidationPlan InterpretMutation(const MutationBatch &) const override {
    return {};
  }

  void AccumulateMutation(const MutationBatch &, AuxEngineServices &,
                          AuxTxnBuffer &buffer) override {
    static_cast<BasicAuxTxnBuffer &>(buffer).MarkPending();
  }

  StagedMaintenanceOutcome StageMaintenance(AuxEngineServices &services,
                                            const InvalidationPlan &,
                                            AuxTxnBuffer &) override {
    StagedMaintenanceOutcome outcome;
    outcome.action = MaintenanceAction::ApplyDelta;
    outcome.expected_version = 0;
    StateImage image;
    image.accelerator = Id();
    image.partition = 0;
    image.version = 1;
    image.payload = std::make_shared<const std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{'o', 'k'});
    outcome.updates.push_back(std::move(image));
    ++stage_count;
    assert(services.CurrentVisibility().mutation_epoch == 0);
    return outcome;
  }

  bool CheckValidity(AuxEngineServices &,
                     const QueryArguments &) const override {
    return true;
  }

  void Execute(AuxEngineServices &, const QueryArguments &,
               AuxRowSink &) override {}

  void ExecuteFallback(AuxEngineServices &, const QueryArguments &,
                       AuxRowSink &) override {}

  int stage_count = 0;
};

} // namespace

int main() {
  TestServices services;
  AuxTransactionCoordinator coordinator;
  auto module = std::make_shared<TestModule>();
  coordinator.Register(module);
  coordinator.AttachServices(services);

  MutationBatch mutation;
  mutation.table = 7;
  mutation.kind = MutationKind::Insert;
  coordinator.OnMutation(mutation);
  coordinator.PreCommit();
  assert(module->stage_count == 1);
  assert(services.store.publishes == 0);
  coordinator.Abort();
  assert(services.store.publishes == 0);

  coordinator.OnMutation(mutation);
  coordinator.PreCommit();
  coordinator.Commit();
  assert(services.store.publishes == 1);
  assert(services.store.images.size() == 1);
  assert(*static_cast<const std::vector<uint8_t> *>(
             services.store.images[0].payload.get()) ==
         std::vector<uint8_t>({'o', 'k'}));

  // Child abort does not feed the parent transaction; child commit merges its
  // captured batches before the outer precommit stage.
  coordinator.BeginSubtransaction();
  coordinator.OnMutation(mutation);
  coordinator.AbortSubtransaction();
  coordinator.PreCommit();
  assert(module->stage_count == 2);

  coordinator.BeginSubtransaction();
  coordinator.OnMutation(mutation);
  coordinator.CommitSubtransaction();
  coordinator.PreCommit();
  assert(module->stage_count == 3);
  coordinator.Abort();

  // The DuckDB store: one accelerator per store, an installed build reads
  // back for every snapshot.
  ::accelerator_codegen::aux_state::engine::duckdb::DuckDBStateStore
      duckdb_store("test");
  auto payload = std::make_shared<int>(42);
  StateImage built;
  built.partition = 3;
  built.payload = std::shared_ptr<const void>(payload);
  duckdb_store.Install({built}, "test");
  const auto snapshot = duckdb_store.Read({0, 0, {}});
  assert(snapshot.available);
  assert(snapshot.partitions.size() == 1);
  assert(*static_cast<const int *>(snapshot.partitions[0].payload.get()) == 42);
  assert(duckdb_store.CheckValidity({"test", ""}, {0, 0, {}}).IsValid());

  // The DuckDB runtime: the module maintains after the SQL commit (never for
  // a rollback) and the published state reads back.
  duckdb::DuckDB db(nullptr);
  duckdb::Connection connection(db);
  connection.Query("CREATE TABLE t (a BIGINT)");
  auto duckdb_module = std::make_shared<TestModule>();
  accelerator_codegen::aux_state::test::ModuleRuntime runtime(connection, duckdb_module);
  runtime.Build();
  MutationBatch row_insert;
  row_insert.table = 7;
  row_insert.kind = MutationKind::Insert;
  row_insert.new_rows = {AuxRow{{AuxValue::Of(AuxType::Int64, int64_t(1))}}};

  connection.BeginTransaction();
  connection.Query("INSERT INTO t VALUES (1)");
  connection.Rollback();
  assert(duckdb_module->stage_count == 0);

  connection.BeginTransaction();
  connection.Query("INSERT INTO t VALUES (1)");
  assert(duckdb_module->stage_count == 0);
  connection.Commit();
  runtime.Commit({row_insert});
  assert(duckdb_module->stage_count == 1);
  auto &duckdb_services = runtime.Backend();
  const auto published =
      duckdb_services.StateStore().Read(duckdb_services.CurrentVisibility());
  assert(published.partitions.size() == 1);
  assert(*static_cast<const std::vector<uint8_t> *>(
             published.partitions[0].payload.get()) ==
         std::vector<uint8_t>({'o', 'k'}));

  return 0;
}
