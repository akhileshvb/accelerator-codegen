// GENERATED BACKEND
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

AuxCommitClock::AuxCommitClock(
    ::duckdb::shared_ptr<::duckdb::AttachedDatabase> db_p)
    : db(std::move(db_p)) {}

void AuxCommitClock::OnCommit(::duckdb::AttachedDatabase &committed,
                              ::duckdb::transaction_t commit_id) {
  if (db && &committed == db.get()) {
    // Visible to snapshots with start time > commit id.
    epoch = commit_id + 1;
  }
}

::duckdb::shared_ptr<::duckdb::AttachedDatabase>
AuxCommitClock::DefaultDatabase(::duckdb::ClientContext &context) {
  try {
    return ::duckdb::DatabaseManager::Get(context).GetDatabase(
        ::duckdb::DatabaseManager::GetDefaultDatabase(context));
  } catch (...) {
    return nullptr;
  }
}

DuckDBAuxBackend::DuckDBAuxBackend(
    core::AuxTransactionCoordinator &coordinator_p,
    core::AuxStateStore &state_store_p,
    MutationTranslator mutation_translator_p, VisibilityCallback visibility_p,
    CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      mutation_translator(std::move(mutation_translator_p)),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p
                     ? cost_model_p()
                     : core::LoadAuxCostModelJson(
                           std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL")
                               ? std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL")
                               : "")),
      workload(std::move(workload_p)) {
  if (!mutation_translator || !visibility) {
    throw std::invalid_argument(
        "DuckDBAuxBackend requires mutation and visibility callbacks");
  }
  coordinator.AttachServices(*this);
}

void DuckDBAuxBackend::OnTableMutation(
    ::duckdb::ClientContext &, const ::duckdb::TableMutationEvent &event) {
  coordinator.OnMutation(mutation_translator(event));
}

void DuckDBAuxBackend::CaptureMutation(const core::MutationBatch &mutation) {
  coordinator.OnMutation(mutation);
}

void DuckDBAuxBackend::TransactionPreCommit(::duckdb::MetaTransaction &,
                                            ::duckdb::ClientContext &) {
  coordinator.PreCommit();
}

void DuckDBAuxBackend::TransactionCommit(::duckdb::MetaTransaction &,
                                         ::duckdb::ClientContext &) {
  coordinator.Commit();
}

void DuckDBAuxBackend::TransactionRollback(::duckdb::MetaTransaction &,
                                           ::duckdb::ClientContext &) {
  coordinator.Abort();
}

core::VisibilityToken DuckDBAuxBackend::CurrentVisibility() const {
  return visibility();
}

core::AuxStateStore &DuckDBAuxBackend::StateStore() { return state_store; }

const maintenance::CostModelParams &
DuckDBAuxBackend::MaintenanceCostModel() const {
  return cost_model;
}

double DuckDBAuxBackend::ExpectedQueriesBeforeNextWrite() const {
  if (workload) {
    return std::max(0.0, workload());
  }
  const char *value =
      std::getenv("ACCELERATOR_CODEGEN_DUCKDB_EXPECTED_QUERIES_BEFORE_WRITE");
  return value && *value ? std::max(0.0, std::stod(value)) : 10.0;
}

std::size_t DuckDBAuxBackend::WorkerCount() const {
  const char *value = std::getenv("ACCEL_AUX_THREADS");
  if (value && *value) {
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed > 0) {
      return static_cast<std::size_t>(parsed);
    }
  }
  if (execution_context) {
    return static_cast<std::size_t>(
        ::accelerator_codegen::ParallelWorkerCount(*execution_context));
  }
  return 1;
}

void DuckDBAuxBackend::SetExecutionContext(::duckdb::ClientContext *context) {
  execution_context = context;
}

namespace {
thread_local ::duckdb::ClientContext *t_reader = nullptr;
} // namespace

::duckdb::ClientContext *DuckDBAuxBackend::ExecutionContext() const {
  return t_reader ? t_reader : execution_context;
}

DuckDBAuxBackend::ReaderScope::ReaderScope(::duckdb::ClientContext &context)
    : previous(t_reader) {
  t_reader = &context;
}

DuckDBAuxBackend::ReaderScope::~ReaderScope() { t_reader = previous; }

::duckdb::ClientContext *DuckDBAuxBackend::CurrentReader() { return t_reader; }

core::VisibilityToken DuckDBAuxBackend::ReaderVisibility() {
  core::VisibilityToken token;
  if (t_reader) {
    token.mutation_epoch = SnapshotEpoch(*t_reader);
  }
  return token;
}

core::Version DuckDBAuxBackend::SnapshotEpoch(::duckdb::ClientContext &context) {
  try {
    auto &meta = ::duckdb::MetaTransaction::Get(context);
    auto db = ::duckdb::DatabaseManager::Get(context).GetDatabase(
        context, ::duckdb::DatabaseManager::GetDefaultDatabase(context));
    if (!db) {
      return 0;
    }
    auto &transaction = meta.GetTransaction(*db);
    if (!transaction.IsDuckTransaction()) {
      return 0;
    }
    return transaction.Cast<::duckdb::DuckTransaction>().start_time;
  } catch (...) {
    return 0; // no active transaction: the newest state
  }
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
