#include "accelerator_codegen/aux_state/engine/postgres/postgres_mutation_capture.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

namespace {
std::vector<PostgresMutationCapture *> capture_targets;
}

PostgresMutationCapture::PostgresMutationCapture(PostgresAuxBackend &backend_p)
    : backend(backend_p) {}

void PostgresMutationCapture::Register() {
  if (!registered) {
    RegisterPostgresMutationCapture(this);
    registered = true;
  }
}

void PostgresMutationCapture::Unregister() {
  if (registered) {
    UnregisterPostgresMutationCapture(this);
    registered = false;
  }
}

PostgresMutationCapture::~PostgresMutationCapture() { Unregister(); }

void PostgresMutationCapture::Capture(const core::MutationBatch &mutation) {
  backend.CaptureStatement(mutation);
}

void PostgresMutationCapture::CaptureInsert(core::TableId table,
                                            std::vector<core::AuxRow> rows) {
  core::MutationBatch mutation;
  mutation.table = table;
  mutation.kind = core::MutationKind::Insert;
  mutation.new_rows = std::move(rows);
  backend.CaptureStatement(mutation);
}

void PostgresMutationCapture::CaptureDelete(core::TableId table,
                                            std::vector<core::AuxRow> rows) {
  core::MutationBatch mutation;
  mutation.table = table;
  mutation.kind = core::MutationKind::Delete;
  mutation.old_rows = std::move(rows);
  backend.CaptureStatement(mutation);
}

void PostgresMutationCapture::CaptureUpdate(
    core::TableId table, std::vector<core::AuxRow> old_rows,
    std::vector<core::AuxRow> new_rows,
    std::vector<core::ColumnId> updated_columns) {
  core::MutationBatch mutation;
  mutation.table = table;
  mutation.kind = core::MutationKind::Update;
  mutation.old_rows = std::move(old_rows);
  mutation.new_rows = std::move(new_rows);
  mutation.updated_columns = std::move(updated_columns);
  backend.CaptureStatement(mutation);
}

void RegisterPostgresMutationCapture(PostgresMutationCapture *capture) {
  if (std::find(capture_targets.begin(), capture_targets.end(), capture) ==
      capture_targets.end()) {
    capture_targets.push_back(capture);
  }
}

void UnregisterPostgresMutationCapture(PostgresMutationCapture *capture) {
  capture_targets.erase(
      std::remove(capture_targets.begin(), capture_targets.end(), capture),
      capture_targets.end());
}

bool DispatchPostgresMutation(const core::MutationBatch &mutation) {
  if (capture_targets.empty()) {
    // A trigger may fire before a query runtime has been used in this backend.
    // The base-table write is authoritative; absence of a local auxiliary
    // runtime is therefore a safe no-op rather than a write failure.
    return true;
  }
  for (auto *capture : capture_targets) {
    capture->Capture(mutation);
  }
  return true;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
