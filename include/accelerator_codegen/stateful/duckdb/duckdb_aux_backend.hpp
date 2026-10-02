// GENERATED BACKEND
#pragma once

#include <atomic>
#include <functional>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// The epoch at which a connection's commits publish aux state, in DuckDB's
// snapshot order (registered on the writing connection). Before PRE_COMMIT
// the writer sets it to its transaction's start time + 1 (pending from
// there); DuckDB then reports the commit id (CommitObserver, called while
// the commit is serialized), and the epoch becomes commit id + 1: the
// publish is visible exactly to snapshots that include the commit.
class AuxCommitClock final : public ::duckdb::CommitObserver {
public:
  explicit AuxCommitClock(::duckdb::shared_ptr<::duckdb::AttachedDatabase> db);

  std::atomic<core::Version> epoch{0};

  void OnCommit(::duckdb::AttachedDatabase &db,
                ::duckdb::transaction_t commit_id) override;

  // The connection's default database (null if unavailable).
  static ::duckdb::shared_ptr<::duckdb::AttachedDatabase>
  DefaultDatabase(::duckdb::ClientContext &context);

private:
  ::duckdb::shared_ptr<::duckdb::AttachedDatabase> db;
};

class DuckDBAuxBackend final : public ::duckdb::MutationConsumer,
                               public core::AuxEngineServices {
public:
  using MutationTranslator =
      std::function<core::MutationBatch(const ::duckdb::TableMutationEvent &)>;
  using VisibilityCallback = std::function<core::VisibilityToken()>;
  using CostModelCallback =
      std::function<maintenance::CostModelParams()>;
  using WorkloadCallback = std::function<double()>;

  DuckDBAuxBackend(core::AuxTransactionCoordinator &coordinator,
                   core::AuxStateStore &state_store,
                   MutationTranslator mutation_translator,
                   VisibilityCallback visibility,
                   CostModelCallback cost_model = {},
                   WorkloadCallback workload = {});

  void OnTableMutation(::duckdb::ClientContext &,
                       const ::duckdb::TableMutationEvent &) override;
  void CaptureMutation(const core::MutationBatch &mutation);
  void TransactionPreCommit(::duckdb::MetaTransaction &,
                            ::duckdb::ClientContext &) override;
  void TransactionCommit(::duckdb::MetaTransaction &,
                         ::duckdb::ClientContext &) override;
  void TransactionRollback(::duckdb::MetaTransaction &,
                           ::duckdb::ClientContext &) override;

  core::VisibilityToken CurrentVisibility() const override;
  core::AuxStateStore &StateStore() override;
  const maintenance::CostModelParams &MaintenanceCostModel() const override;
  double ExpectedQueriesBeforeNextWrite() const override;
  std::size_t WorkerCount() const override;
  // A table function cannot run SQL on its own context, so a reader cannot
  // build the state at its snapshot: every commit publishes valid state.
  bool ReadersCanBuildState() const override { return false; }

  void SetExecutionContext(::duckdb::ClientContext *context);
  // The reading connection's context if the current thread is inside a
  // ReaderScope, else the one set by SetExecutionContext.
  ::duckdb::ClientContext *ExecutionContext() const;

  // Marks the current thread as reading for `context` (an aux table function
  // running in another connection): its transaction's snapshot is the
  // visibility (ReaderVisibility), and its context runs intra-query workers.
  class ReaderScope {
  public:
    explicit ReaderScope(::duckdb::ClientContext &context);
    ~ReaderScope();
    ReaderScope(const ReaderScope &) = delete;
    ReaderScope &operator=(const ReaderScope &) = delete;

  private:
    ::duckdb::ClientContext *previous;
  };
  static ::duckdb::ClientContext *CurrentReader();
  // The current reader's snapshot (SnapshotEpoch), or 0 (the newest state)
  // outside a ReaderScope.
  static core::VisibilityToken ReaderVisibility();
  // The snapshot epoch of `context`'s transaction on its default database:
  // its start time. A commit with id c is in that snapshot iff c < start
  // time, so state published at epoch c + 1 is visible to it exactly then.
  // 0 if there is no DuckDB transaction.
  static core::Version SnapshotEpoch(::duckdb::ClientContext &context);

private:
  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  MutationTranslator mutation_translator;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
  ::duckdb::ClientContext *execution_context = nullptr;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
