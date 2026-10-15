// GENERATED BACKEND
#include "backends/duckdb/engine/duckdb_aux_backend.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {
thread_local ::duckdb::ClientContext *t_reader = nullptr;
} // namespace

DuckDBAuxBackend::DuckDBAuxBackend(runtime::AcceleratorRuntime &runtime_p,
                                   ::duckdb::ClientContext &context_p)
    : runtime(runtime_p), context(context_p) {}

core::VisibilityToken DuckDBAuxBackend::CurrentVisibility() const {
  return runtime.CurrentVisibility();
}

core::AuxStateStore &DuckDBAuxBackend::StateStore() { return runtime.StateStore(); }

const maintenance::CostModelParams &DuckDBAuxBackend::MaintenanceCostModel() const {
  return runtime.MaintenanceCostModel();
}

double DuckDBAuxBackend::ExpectedQueriesBeforeNextWrite() const {
  return runtime.ExpectedQueriesBeforeNextWrite();
}

std::size_t DuckDBAuxBackend::WorkerCount() const { return runtime.WorkerCount(); }

void DuckDBAuxBackend::ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const {
  runtime.ParallelFor(n, fn);
}

::duckdb::ClientContext *DuckDBAuxBackend::ExecutionContext() const {
  return t_reader ? t_reader : &context;
}

DuckDBAuxBackend::ReaderScope::ReaderScope(::duckdb::ClientContext &reader)
    : previous(t_reader) {
  t_reader = &reader;
}

DuckDBAuxBackend::ReaderScope::~ReaderScope() { t_reader = previous; }

::duckdb::ClientContext *DuckDBAuxBackend::CurrentReader() { return t_reader; }

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
