// GENERATED BACKEND
#pragma once

#include <vector>

#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Transition-table SQL functions call this bridge after converting OLD TABLE
// and NEW TABLE rows. UPDATE batches must contain both sets; row ordering is
// not part of the contract.
class PostgresMutationCapture {
public:
  explicit PostgresMutationCapture(PostgresAuxBackend &backend);
  ~PostgresMutationCapture();

  void Register(const std::vector<core::TableId> &dependency_tables = {});
  void Unregister();

  void Capture(const core::MutationBatch &mutation);
  void CaptureInsert(core::TableId table, std::vector<core::AuxRow> rows);
  void CaptureDelete(core::TableId table, std::vector<core::AuxRow> rows);
  void CaptureUpdate(core::TableId table, std::vector<core::AuxRow> old_rows,
                     std::vector<core::AuxRow> new_rows,
                     std::vector<core::ColumnId> updated_columns);

private:
  PostgresAuxBackend &backend;
  bool registered = false;
};

// The trigger function runs in the same backend process as the coordinator.
// This registration is process-local; no pointer is placed in shared memory.
void RegisterPostgresMutationCapture(PostgresMutationCapture *capture);
void UnregisterPostgresMutationCapture(PostgresMutationCapture *capture);
bool DispatchPostgresMutation(const core::MutationBatch &mutation);
bool HasRegisteredCaptureForTable(core::TableId table);

} // namespace accelerator_codegen::aux_state::engine::postgres
