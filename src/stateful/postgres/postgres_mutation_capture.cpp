// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_mutation_capture.hpp"

#include "accelerator_codegen/stateful/postgres/postgres_session_gucs.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

namespace {

std::vector<PostgresMutationCapture *> capture_targets;
std::unordered_map<core::TableId, std::vector<PostgresMutationCapture *>>
    captures_by_table;
std::unordered_map<PostgresMutationCapture *,
                   std::vector<core::TableId>>
    tables_by_capture;

void RemoveCaptureFromTableMap(PostgresMutationCapture *capture) {
  const auto it = tables_by_capture.find(capture);
  if (it == tables_by_capture.end()) {
    return;
  }
  for (const core::TableId table : it->second) {
    auto map_it = captures_by_table.find(table);
    if (map_it == captures_by_table.end()) {
      continue;
    }
    auto &captures = map_it->second;
    captures.erase(
        std::remove(captures.begin(), captures.end(), capture),
        captures.end());
    if (captures.empty()) {
      captures_by_table.erase(map_it);
    }
  }
  tables_by_capture.erase(it);
}

void UnregisterAllCapturesExcept(PostgresMutationCapture *keep) {
  const auto targets = capture_targets;
  for (auto *capture : targets) {
    if (capture != keep) {
      capture->Unregister();
    }
  }
}

} // namespace

PostgresMutationCapture::PostgresMutationCapture(PostgresAuxBackend &backend_p)
    : backend(backend_p) {}

void PostgresMutationCapture::Register(
    const std::vector<core::TableId> &dependency_tables) {
  if (SessionSingleActiveAuxEnabled()) {
    UnregisterAllCapturesExcept(this);
  }
  RemoveCaptureFromTableMap(this);
  if (!registered) {
    RegisterPostgresMutationCapture(this);
    registered = true;
  }
  tables_by_capture[this] = dependency_tables;
  for (const core::TableId table : dependency_tables) {
    captures_by_table[table].push_back(this);
  }
}

void PostgresMutationCapture::Unregister() {
  if (!registered) {
    return;
  }
  RemoveCaptureFromTableMap(this);
  UnregisterPostgresMutationCapture(this);
  registered = false;
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

bool HasRegisteredCaptureForTable(const core::TableId table) {
  const auto it = captures_by_table.find(table);
  return it != captures_by_table.end() && !it->second.empty();
}

bool DispatchPostgresMutation(const core::MutationBatch &mutation) {
  const auto it = captures_by_table.find(mutation.table);
  if (it == captures_by_table.end() || it->second.empty()) {
    return true;
  }
  for (auto *capture : it->second) {
    capture->Capture(mutation);
  }
  return true;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
