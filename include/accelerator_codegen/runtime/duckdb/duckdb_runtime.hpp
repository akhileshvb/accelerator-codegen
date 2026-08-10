#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/core/aux_registry.hpp"
#include "accelerator_codegen/aux_state/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/engine/duckdb/duckdb_aux_backend.hpp"
#include "accelerator_codegen/aux_state/engine/duckdb/duckdb_state_store.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/engine/shared/query_catalog.hpp"
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
  std::vector<core::AuxRow> ExecuteCollect();
  std::vector<core::AuxRow> ExecuteCollect(const core::QueryArguments &arguments);
  void Apply(SharedProbe &probe);

  // Calibration helpers (no SQL mutation; exercises coordinator + state store).
  void ResetCalibrationState();
  void InvalidateAll();
  void ApplyMutationBatch(const core::MutationBatch &mutation);
  void ExecuteBaseQuery();

  uint64_t last_rows = 0;

private:
  ::duckdb::Connection &connection_;
  const SharedQueryCatalogEntry &entry_;
  accelerator_codegen::aux_state::PartitionedStateStore legacy_store_;
  DuckDBStateStore state_store_;
  core::AuxTransactionCoordinator coordinator_;
  DuckDBAuxBackend backend_;
  std::unique_ptr<Binding> binding_;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
