// GENERATED BACKEND
#pragma once

// One per database: the runtime skeleton's host with DuckDB's slots, the
// database-level capture of every connection's writes (S1), and per
// accelerator its module binding and its own connection (builds, maintenance
// lookups, fallback scans).

#include <atomic>
#include <memory>
#include <string>

#include "backends/duckdb/api/catalog/query_catalog.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "substrate/aux/runtime/runtime_host.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// One registered accelerator: its binding, and its own connection (builds,
// maintenance lookups, fallback scans).
struct DuckdbAccelerator {
  const SharedQueryCatalogEntry *entry = nullptr;
  std::unique_ptr<::duckdb::Connection> connection;
  std::unique_ptr<DuckdbRuntime::Binding> binding;
  runtime::AcceleratorRuntime *runtime = nullptr;
  runtime::TableMap tables; // its module's table numbers -> host table ids
  std::unique_ptr<DuckDBAuxBackend> backend;
  maintenance::CostModelParams cost_model;
  std::atomic<double> expected_queries_before_write{10.0};
  // Start time when it registered: writers that began earlier may have
  // written its tables uncaptured.
  std::atomic<uint64_t> registered_at{0};
};

class DuckdbHost {
public:
  using Accelerator = DuckdbAccelerator;

  // The database's host, created on first use; it lives while someone holds it.
  static std::shared_ptr<DuckdbHost> Get(::duckdb::DatabaseInstance &db);
  // The database's host if one exists (nullptr: nothing is captured on it).
  static std::shared_ptr<DuckdbHost> Existing(::duckdb::DatabaseInstance &db);
  ~DuckdbHost();
  DuckdbHost(const DuckdbHost &) = delete;
  DuckdbHost &operator=(const DuckdbHost &) = delete;

  // Registers the catalog entry's accelerator (once; not built yet).
  Accelerator &Ensure(const SharedQueryCatalogEntry &entry);
  Accelerator *Find(const std::string &accelerator_id);
  runtime::RuntimeHost &Runtime();
  // Whether `context`'s open transaction has written a table the accelerator
  // depends on. Those uncommitted rows are in no published state, so such a
  // reader must be answered from SQL on its own transaction.
  bool TransactionWroteDependency(::duckdb::ClientContext &context, const std::string &accelerator_id);
  // Tests that hand-build every change (DuckdbRuntime::ApplyMutationBatch)
  // while also writing it with SQL: engine capture is ignored.
  void SetManualCapture(bool manual);

  // A change of `context`'s open transaction on a table the engine does not
  // capture (an accelerator without capture tables): delivered with the
  // transaction, as if captured.
  bool Captures(core::TableId table); // a host table id
  void CaptureByHand(::duckdb::ClientContext &context, runtime::ColumnarBatchRef batch);

  // ---- engine callbacks (engine/capture.cpp) ----
  void OnMutation(::duckdb::ClientContext &context, const ::duckdb::TableMutationEvent &event);
  void OnPreCommit(::duckdb::MetaTransaction &transaction);
  void OnCommitId(::duckdb::ClientContext &context, ::duckdb::AttachedDatabase &db,
                  ::duckdb::transaction_t commit_id);
  void OnCommit(::duckdb::MetaTransaction &transaction, ::duckdb::ClientContext &context);
  void OnRollback(::duckdb::MetaTransaction &transaction);

private:
  void InvalidateStraddled(uint64_t start, uint64_t epoch);
  explicit DuckdbHost(::duckdb::DatabaseInstance &db);
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
