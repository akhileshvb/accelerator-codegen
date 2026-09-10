// GENERATED BACKEND - C bridge for PostgreSQL extension.
// Provides C-linkage functions that the PG extension (.so) calls.
// Compiled into MODULE accelerator_codegen_pg (not the host shared lib).

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"

using namespace accelerator_codegen::aux_state;

namespace {

struct BridgeState {
  engine::postgres::PostgresStateStore state_store;
  core::AuxTransactionCoordinator coordinator;
  engine::postgres::PostgresAuxBackend backend;

  struct Observer {
    std::string last_old_json;
    std::string last_new_json;
    std::string last_upd_cols_json;
    std::vector<core::MutationBatch> captured_mutations;
    int pending_mutations = 0;
    bool pre_committed = false;
    bool committed = false;
    bool aborted = false;

    struct StagedToyBuild {
      std::string key;
      int64_t value;
    };
    std::vector<StagedToyBuild> staged_toy_builds;

    struct CapturedRow {
      std::vector<std::string> cells;
    };
    std::vector<CapturedRow> scan_rows;
    std::size_t scan_column_count = 0;
    int scan_callback_count = 0;
  };
  Observer observer;

  explicit BridgeState(std::string backing_dir)
      : state_store(std::move(backing_dir)),
        backend(coordinator, state_store,
               [this]() {
                 state_store.EnsureLoaded();
                 core::VisibilityToken tok;
                 tok.mutation_epoch = state_store.GetCurrentMutationEpoch();
                 tok.backend_token = "pg_backend";
                 return tok;
               },
               nullptr, nullptr) {
    coordinator.AttachServices(backend);
  }
};

static std::mutex g_mutex;
static BridgeState *g_bridge = nullptr;

BridgeState &State() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_bridge) g_bridge = new BridgeState("/tmp/accel_pg_aux_state");
  return *g_bridge;
}

} // anonymous namespace

extern "C" {

void accel_pg_init_backend(const char *backing_dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string dir =
      (backing_dir && *backing_dir) ? backing_dir : "/tmp/accel_pg_aux_state";
  if (g_bridge) {
    delete g_bridge;
    g_bridge = nullptr;
  }
  g_bridge = new BridgeState(dir);
  engine::postgres::SetPostgresActiveBackend(&g_bridge->backend);
}

int accel_pg_capture_mutation_value(int table_oid, int kind,
                                     const char *old_json, const char *new_json,
                                     const char *updated_cols_json) {
  auto &s = State();
  core::MutationBatch batch;
  batch.table = static_cast<uint64_t>(table_oid);
  batch.kind = static_cast<core::MutationKind>(kind);

  // Parse old/new rows from JSON. Format: "v1,v2,...|v3,v4,..."
  // Simple comma-separated cell values per row, pipe-separated rows.
  auto parse_rows = [](const char *json, std::vector<core::AuxRow> &rows) {
    if (!json || !*json) return;
    std::string data(json);
    // Pipe separates rows
    std::string row_str;
    for (size_t i = 0; i <= data.size(); ++i) {
      if (i == data.size() || data[i] == '|') {
        if (!row_str.empty()) {
          core::AuxRow row;
          // Comma separates cells
          std::string cell;
          for (size_t j = 0; j <= row_str.size(); ++j) {
            if (j == row_str.size() || row_str[j] == ',') {
              core::AuxValue val;
              if (cell == "NULL") {
                val = core::AuxValue::Null(core::AuxType::String);
              } else {
                val = core::AuxValue::Of(core::AuxType::String, cell);
              }
              row.values.push_back(val);
              cell.clear();
            } else {
              cell += row_str[j];
            }
          }
          rows.push_back(std::move(row));
          row_str.clear();
        }
      } else {
        row_str += data[i];
      }
    }
  };

  parse_rows(old_json, batch.old_rows);
  parse_rows(new_json, batch.new_rows);

  // Parse updated column IDs
  if (updated_cols_json && *updated_cols_json) {
    std::string cols(updated_cols_json);
    std::string num;
    for (size_t i = 0; i <= cols.size(); ++i) {
      if (i == cols.size() || cols[i] == ',') {
        if (!num.empty()) {
          batch.updated_columns.push_back(
              static_cast<core::ColumnId>(std::stoul(num)));
          num.clear();
        }
      } else {
        num += cols[i];
      }
    }
  }

  if (old_json) s.observer.last_old_json = old_json;
  if (new_json) s.observer.last_new_json = new_json;
  if (updated_cols_json) s.observer.last_upd_cols_json = updated_cols_json;
  s.observer.captured_mutations.push_back(batch);
  s.observer.pending_mutations++;
  s.backend.CaptureMutation(batch);
  return 1;
}

int accel_pg_toy_build(const char *key, int64_t value) {
  auto &s = State();
  auto encoded = engine::postgres::EncodeToyPayload(key, value);

  core::StagedMaintenanceOutcome outcome;
  outcome.action = maintenance::MaintenanceAction::RebuildAll;
  outcome.invalidations = core::AllPartitionsPlan();

  auto payload = std::make_shared<std::vector<uint8_t>>(std::move(encoded));
  core::StateImage image = core::StateImage::Make(
      "postgres", 1, 0,
      std::static_pointer_cast<const std::vector<uint8_t>>(payload));
  outcome.updates.push_back(image);
  outcome.expected_version = 0;

  return s.state_store.TryPublish(outcome, 0) ? 1 : 0;
}

// Stage a toy build for deferred txn-aware publication.
// Flushed by commit hook, cleared by abort hook.
int accel_pg_toy_stage_build(const char *key, int64_t value) {
  auto &s = State();
  BridgeState::Observer::StagedToyBuild b;
  b.key = key ? key : "";
  b.value = value;
  s.observer.staged_toy_builds.push_back(b);
  return 1;
}

int accel_pg_toy_read(int64_t *out_value) {
  auto &s = State();
  core::VisibilityToken tok = s.backend.CurrentVisibility();
  auto snapshot = s.state_store.Read(tok);

  if (snapshot.partitions.empty()) return 0;

  const auto *raw = static_cast<const std::vector<uint8_t> *>(
      snapshot.partitions[0].payload.get());
  if (!raw) return 0;

  auto [key, value] = engine::postgres::DecodeToyPayload(*raw);
  (void)key;
  *out_value = value;
  return 1;
}

// --- Staged toy build support (txn-aware visibility testing) ---
int accel_pg_toy_current_epoch(uint64_t *out_epoch) {
  auto &s = State();
  s.state_store.EnsureLoaded();
  *out_epoch = s.state_store.GetCurrentMutationEpoch();
  return 1;
}

// Check validity: QueryKey{"postgres", key} x VisibilityToken{epoch}
int accel_pg_toy_check_validity(const char *key, uint64_t visibility_epoch,
                                 int *out_code, uint64_t *out_version) {
  auto &s = State();
  core::QueryKey qk;
  qk.accelerator = "postgres";
  qk.value = key ? key : "";
  core::VisibilityToken vt;
  vt.mutation_epoch = visibility_epoch;
  vt.backend_token = "pg_toy_check";
  auto result = s.state_store.CheckValidity(qk, vt);
  *out_code = static_cast<int>(result.code);
  *out_version = result.visible_version;
  return 1;
}

// Invalidate all partitions.
void accel_pg_toy_invalidate() {
  auto &s = State();
  auto plan = core::AllPartitionsPlan();
  s.state_store.Invalidate(plan);
}

// Flush staged toy builds (called from commit hook).
void accel_pg_flush_staged_toy_builds() {
  auto &s = State();
  auto &staged = s.observer.staged_toy_builds;
  if (staged.empty()) return;
  for (const auto &b : staged) {
    auto encoded = engine::postgres::EncodeToyPayload(b.key, b.value);
    core::StagedMaintenanceOutcome outcome;
    outcome.action = maintenance::MaintenanceAction::RebuildAll;
    outcome.invalidations = core::AllPartitionsPlan();
    auto payload = std::make_shared<std::vector<uint8_t>>(std::move(encoded));
    core::StateImage image = core::StateImage::Make(
        "postgres", 1, 0,
        std::static_pointer_cast<const std::vector<uint8_t>>(payload));
    outcome.updates.push_back(image);
    outcome.expected_version = 0;
    s.state_store.TryPublish(outcome, 0);
  }
  staged.clear();
}

// Clear staged toy builds (called from abort hook).
void accel_pg_clear_staged_toy_builds() {
  State().observer.staged_toy_builds.clear();
}

int accel_pg_get_observer_mutation_count() {
  return static_cast<int>(State().observer.captured_mutations.size());
}

int accel_pg_get_observer_pre_committed() {
  return State().observer.pre_committed ? 1 : 0;
}

int accel_pg_get_observer_committed() {
  return State().observer.committed ? 1 : 0;
}

int accel_pg_get_observer_aborted() {
  return State().observer.aborted ? 1 : 0;
}

void accel_pg_reset_observer() {
  auto &s = State();
  s.observer = BridgeState::Observer{};
}

// --- Mutation detail ---
const char *accel_pg_get_observer_mutation_at(int idx) {
  auto &s = State();
  if (idx < 0 || static_cast<size_t>(idx) >= s.observer.captured_mutations.size())
    return "{}";
  static char buf[4096];
  const auto &m = s.observer.captured_mutations[idx];
  const char *kind_str = "Insert";
  if (m.kind == core::MutationKind::Delete) kind_str = "Delete";
  else if (m.kind == core::MutationKind::Update) kind_str = "Update";
  snprintf(buf, sizeof(buf),
           "{table:%lu,kind:%s,old_rows:%zu,new_rows:%zu,upd_cols:%zu}",
           (unsigned long)m.table, kind_str,
           m.old_rows.size(), m.new_rows.size(),
           m.updated_columns.size());
  return buf;
}

const char *accel_pg_get_last_new_json() {
  return State().observer.last_new_json.c_str();
}

const char *accel_pg_get_last_old_json() {
  return State().observer.last_old_json.c_str();
}

const char *accel_pg_get_observer_mutation_row(int idx, int row_num, int is_new) {
  auto &s = State();
  if (idx < 0 || static_cast<size_t>(idx) >= s.observer.captured_mutations.size())
    return "";
  const auto &rows = is_new ? s.observer.captured_mutations[idx].new_rows
                            : s.observer.captured_mutations[idx].old_rows;
  if (row_num < 0 || static_cast<size_t>(row_num) >= rows.size()) return "";
  static char buf[4096];
  std::string out;
  for (size_t ci = 0; ci < rows[row_num].values.size(); ++ci) {
    if (ci > 0) out += ",";
    const auto &v = rows[row_num].values[ci];
    if (v.is_null) {
      out += "NULL";
    } else if (auto *sptr = std::get_if<std::string>(&v.payload)) {
      out += *sptr;
    } else {
      out += "?";
    }
  }
  snprintf(buf, sizeof(buf), "%s", out.c_str());
  return buf;
}
 

// --- Native scan results ---
int accel_pg_get_scan_row_count() {
  return static_cast<int>(State().observer.scan_rows.size());
}

int accel_pg_get_scan_column_count() {
  return static_cast<int>(State().observer.scan_column_count);
}

int accel_pg_get_scan_callback_count() {
  return State().observer.scan_callback_count;
}

const char *accel_pg_get_scan_row_cell(int row_idx, int col_idx) {
  auto &s = State();
  if (row_idx < 0 || static_cast<size_t>(row_idx) >= s.observer.scan_rows.size())
    return "";
  if (col_idx < 0 || static_cast<size_t>(col_idx) >= s.observer.scan_rows[row_idx].cells.size())
    return "";
  return s.observer.scan_rows[row_idx].cells[col_idx].c_str();
}

void accel_pg_scan_clear() {
  auto &s = State();
  s.observer.scan_rows.clear();
  s.observer.scan_column_count = 0;
  s.observer.scan_callback_count = 0;
}

// Called from the extension's native scan implementation.
void accel_pg_scan_callback_push(int column_count, const char *cells_json) {
  auto &s = State();
  BridgeState::Observer::CapturedRow row;
  if (!cells_json || !*cells_json) return;
  std::string data(cells_json);
  std::string cell;
  for (size_t i = 0; i <= data.size(); ++i) {
    if (i == data.size() || data[i] == '\t') {
      row.cells.push_back(cell);
      cell.clear();
    } else {
      cell += data[i];
    }
  }
  s.observer.scan_rows.push_back(std::move(row));
  s.observer.scan_column_count = static_cast<std::size_t>(column_count);
}

void accel_pg_scan_callback_inc() {
  State().observer.scan_callback_count++;
}

// --- Txn hooks ---
void accel_pg_pre_commit_hook() {
  auto &s = State();
  if (s.observer.pending_mutations > 0) {
    s.observer.pre_committed = true;
  }
  s.backend.OnPreCommit();
}

void accel_pg_commit_hook() {
  auto &s = State();
  if (s.observer.pending_mutations > 0) {
    s.observer.committed = true;
    s.observer.pending_mutations = 0;
  }
  // Flush staged toy builds (txn-aware visibility contract).
  accel_pg_flush_staged_toy_builds();
  s.backend.OnCommit();
}

void accel_pg_abort_hook() {
  auto &s = State();
  if (s.observer.pending_mutations > 0) {
    s.observer.aborted = true;
    s.observer.pending_mutations = 0;
  }
  // Clear staged toy builds on abort (txn-aware visibility contract).
  accel_pg_clear_staged_toy_builds();
  s.backend.OnAbort();
}

} // extern "C"
