// Hosts one aux module on a DuckDB connection for module-level tests.
//
// A test runs its SQL on `connection`, hands the described changes to
// Commit() after the SQL commit (maintenance runs after the engine commit, as
// in the real host), and reads through Read(), which compares nothing itself.
// The module sees the connection through the usual DuckDBAuxBackend services.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

#include "backends/duckdb/api/catalog/query_catalog.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "backends/duckdb/api/duckdb_aux_backend.hpp"
#include "backends/duckdb/api/scan.hpp"
#include "substrate/aux/runtime/runtime_host.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::test {

class ModuleRuntime final : public runtime::EngineSlots {
public:
  ModuleRuntime(::duckdb::Connection &connection_p, std::shared_ptr<core::AuxQueryModule> module_p,
                maintenance::CostModelParams cost_model_p = {})
      : connection(connection_p), module(std::move(module_p)), cost_model(std::move(cost_model_p)),
        host(*this, runtime::ResolveThreadBudget(0)), accelerator(host.Register(module)),
        backend(accelerator, *connection.context) {
    // Phase caps follow ACCEL_AUX_{READ,BUILD,MAINT}_THREADS, as in the real host.
    auto &threads = host.Threads();
    threads.SetPhaseLimits(runtime::ResolveReadThreads(0, threads.Budget()),
                           runtime::ResolveBuildThreads(0, threads.Budget()),
                           runtime::ResolveMaintenanceThreads(0, threads.Budget()));
    accelerator.UseModuleServices(backend);
    // Fallbacks answer from the query's SQL on a connection of their own, as in the real host.
    if (const auto *entry = engine::FindSharedQueryByAccelerator(module->Id())) {
      backend.SetSqlAnswer([this, entry](const core::QueryArguments &arguments, core::AuxRowSink &sink) {
        return engine::AnswerFromFallbackSql(fallback_connection, *entry, arguments, sink);
      });
    }
  }

  // Builds the state afresh (dropping any current one); throws if it does not
  // end up synced.
  void Build() {
    accelerator.Invalidate();
    if (!accelerator.BuildNow()) {
      throw std::runtime_error("aux state did not sync after build");
    }
  }

  // Maintains the commit of `batches` (already committed in SQL), then waits
  // out any rebuild the maintenance asked for. Rows hold the module's
  // mutation_attnums columns in that order (every column if it lists none).
  void Commit(const std::vector<core::MutationBatch> &batches) {
    std::vector<runtime::ColumnarBatchRef> captured;
    for (const auto &batch : batches) {
      captured.push_back(std::make_shared<const runtime::ColumnarBatch>(
          runtime::FromMutationBatch(batch, AttnumsOf(batch))));
    }
    accelerator.BeginCommit(++txn, std::move(captured));
    accelerator.PublishCommit(NextEpoch());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!accelerator.IsSynced()) {
      if (std::chrono::steady_clock::now() > deadline) {
        throw std::runtime_error("aux state did not resync after commit");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // The newest state's answer, or the module's fallback when the state cannot
  // serve. Returns whether the state served.
  bool Read(const core::QueryArguments &arguments, core::AuxRowSink &sink) {
    const engine::duckdb::scan::ScanServicesScope scans(backend);
    runtime::ReadOptions options;
    options.max_wait_us = 1'000'000;
    if (accelerator.Read(options, arguments, sink) == runtime::ReadResult::Served) {
      return true;
    }
    module->ExecuteFallback(backend, arguments, sink);
    return false;
  }

  engine::duckdb::DuckDBAuxBackend &Backend() { return backend; }
  runtime::AcceleratorRuntime &Runtime() { return accelerator; }
  runtime::AcceleratorStats Stats() const { return accelerator.Stats(); }

private:
  struct Snapshot final : runtime::BuildSnapshot {
    explicit Snapshot(::duckdb::Connection &connection_p) : connection(connection_p) {
      connection.BeginTransaction();
      epoch = engine::duckdb::DuckDBAuxBackend::SnapshotEpoch(*connection.context);
    }
    ~Snapshot() override {
      if (connection.HasActiveTransaction()) {
        connection.Rollback();
      }
    }
    runtime::Epoch SnapshotEpoch() const override { return epoch; }
    ::duckdb::Connection &connection;
    runtime::Epoch epoch = 0;
  };

  // A transaction started now is later than every commit so far.
  runtime::Epoch NextEpoch() {
    Snapshot now(connection);
    return now.SnapshotEpoch();
  }

  std::vector<int32_t> AttnumsOf(const core::MutationBatch &batch) const {
    for (const auto &dependency : module->Dependencies()) {
      if (dependency.table != batch.table) {
        continue;
      }
      if (!dependency.mutation_attnums.empty()) {
        return dependency.mutation_attnums;
      }
      const auto &row = batch.new_rows.empty() ? batch.old_rows.at(0) : batch.new_rows.at(0);
      std::vector<int32_t> all(row.values.size());
      std::iota(all.begin(), all.end(), 0);
      return all;
    }
    throw std::invalid_argument("mutation for a table the module does not watch");
  }

  // ---- EngineSlots ----
  runtime::Epoch PendingEpochLowerBound(uint64_t) const override { return 0; }
  runtime::Epoch OldestReaderEpoch() const override { return kNoFloor; }
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &, uint64_t) override {
    return true;
  }
  std::unique_ptr<runtime::BuildSnapshot> OpenBuildSnapshot(runtime::AcceleratorRuntime &) override {
    return std::make_unique<Snapshot>(connection);
  }
  bool EngineWorkOnHostThread() const override { return false; }
  void RunBuild(runtime::AcceleratorRuntime &, runtime::BuildSnapshot &,
                core::StateBuilder &builder) override {
    const engine::duckdb::scan::ScanServicesScope scans(backend);
    module->Build(backend, builder);
  }
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &) const override {
    return cost_model;
  }

  ::duckdb::Connection &connection;
  ::duckdb::Connection fallback_connection{*connection.context->db};
  std::shared_ptr<core::AuxQueryModule> module;
  maintenance::CostModelParams cost_model;
  uint64_t txn = 0;
  runtime::RuntimeHost host;
  runtime::AcceleratorRuntime &accelerator;
  engine::duckdb::DuckDBAuxBackend backend;
};

} // namespace accelerator_codegen::aux_state::test
