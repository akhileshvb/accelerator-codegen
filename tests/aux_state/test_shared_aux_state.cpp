#include <cassert>
#include <memory>
#include <string>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

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

  void Build(AuxEngineServices &, StateBuilder &) override {}

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

  // The DuckDB compatibility store must not re-enter PartitionedStateStore's
  // mutex from a ForEachSlot callback while serializing or checking versions.
  ::accelerator_codegen::aux_state::PartitionedStateStore legacy_store;
  auto payload = std::make_shared<int>(42);
  legacy_store.PublishPartition(3, std::shared_ptr<const void>(payload), 0);
  ::accelerator_codegen::aux_state::engine::duckdb::DuckDBStateStore
      duckdb_store(
          "test", legacy_store);
  const auto snapshot = duckdb_store.Read({0, 0, {}});
  assert(snapshot.available);
  assert(snapshot.partitions.size() == 1);
  assert(*static_cast<const int *>(snapshot.partitions[0].payload.get()) == 42);
  assert(duckdb_store.CheckValidity({"test", ""}, {0, 0, {}}).IsValid());

  // Exercise the shared coordinator through DuckDB's custom mutation and
  // transaction-hook adapter, rather than calling the coordinator directly.
  duckdb::DuckDB db(nullptr);
  duckdb::Connection connection(db);
  AuxTransactionCoordinator duckdb_coordinator;
  TestStore duckdb_aux_store;
  auto duckdb_module = std::make_shared<TestModule>();
  duckdb_coordinator.Register(duckdb_module);
  ::accelerator_codegen::aux_state::engine::duckdb::DuckDBAuxBackend
      duckdb_backend(
      duckdb_coordinator, duckdb_aux_store,
          [](const duckdb::TableMutationEvent &event) {
            MutationBatch mutation;
            mutation.table = event.table_oid;
            mutation.kind = event.kind == duckdb::TableMutationKind::DELETE
                                ? MutationKind::Delete
                                : event.kind == duckdb::TableMutationKind::UPDATE
                                      ? MutationKind::Update
                                      : MutationKind::Insert;
            return mutation;
          }, []() { return VisibilityToken{}; });
  duckdb::TableMutationEvent duckdb_event;
  duckdb_event.table_oid = 7;
  duckdb_backend.OnTableMutation(*connection.context, duckdb_event);
  duckdb::MetaTransaction duckdb_meta_txn(
      *connection.context, duckdb::Timestamp::GetCurrentTimestamp(), 1);
  duckdb_backend.TransactionPreCommit(duckdb_meta_txn, *connection.context);
  assert(duckdb_aux_store.publishes == 0);
  duckdb_backend.TransactionCommit(duckdb_meta_txn, *connection.context);
  assert(duckdb_aux_store.publishes == 1);

  return 0;
}
