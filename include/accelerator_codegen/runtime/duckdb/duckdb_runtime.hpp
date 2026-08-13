// GENERATED BACKEND
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_store_diagnostics.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

class DuckdbRuntime {
public:
  struct Binding {
    virtual ~Binding() = default;
    virtual core::AuxQueryModule &Module() = 0;
    virtual void Build(DuckDBAuxBackend &backend,
                       core::StateBuilder &builder) = 0;
    virtual void Execute(DuckDBAuxBackend &backend,
                         const core::QueryArguments &arguments,
                         core::AuxRowSink &sink) = 0;
  };

  DuckdbRuntime(::duckdb::Connection &connection,
                const SharedQueryCatalogEntry &entry,
                double expected_queries_before_write);
  ~DuckdbRuntime();

  void Build();
  void Execute();
  void Execute(const core::QueryArguments &arguments);
  std::vector<core::AuxRow> ExecuteCollect();
  std::vector<core::AuxRow> ExecuteCollect(const core::QueryArguments &arguments);
  void Apply(SharedProbe &probe);

  // Mini-chunk TPC-H RF1/RF2 (orders+lineitem insert/delete). Alternating
  // chunks keep DBs from unbounded growth while exercising I/D maintenance.
  // Point-update Apply(SharedProbe) remains the default sweep write path.
  void ApplyRfChunk(int write_seq, int order_count = 1);

  void SetExpectedQueriesBeforeNextWrite(double expected_queries_before_write);
  double ExpectedQueriesBeforeNextWrite() const;

  // Calibration helpers (no SQL mutation; exercises coordinator + state store).
  void ResetCalibrationState();
  void InvalidateAll();
  void ApplyMutationBatch(const core::MutationBatch &mutation);
  void ExecuteBaseQuery();

  core::AuxStoreDiagnosticsSnapshot StoreDiagnosticsSnapshot() const;
  void ResetStoreDiagnosticsProfiling();
  uint64_t AuxStateFootprintBytes() const;

  uint64_t last_rows = 0;

private:
  ::duckdb::Connection &connection_;
  const SharedQueryCatalogEntry &entry_;
  accelerator_codegen::aux_state::PartitionedStateStore legacy_store_;
  DuckDBStateStore state_store_;
  core::AuxTransactionCoordinator coordinator_;
  std::shared_ptr<double> expected_queries_before_write_;
  DuckDBAuxBackend backend_;
  std::unique_ptr<Binding> binding_;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
