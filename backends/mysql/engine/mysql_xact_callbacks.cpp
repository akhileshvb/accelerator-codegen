#include "backends/mysql/engine/mysql_xact_callbacks.hpp"

#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"
#include "backends/mysql/state/mysql_aux_backend.hpp"
#include "backends/mysql/engine/mysql_mutation_capture.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace accelerator_codegen::aux_state::engine::mysql {
namespace core = accelerator_codegen::aux_state::core;
namespace maintenance = accelerator_codegen::aux_state::maintenance;

namespace {

MysqlAuxBackend *g_active_backend = nullptr;
std::mutex g_backends_mu;
std::vector<MysqlAuxBackend *> g_mutation_backends;
std::atomic<int> g_observer_mutations{0};
std::unordered_map<MysqlAuxBackend *, std::string> g_backend_last_action;

core::AuxRow ProjectRow(const core::AuxRow &src,
                        const std::vector<int32_t> &attnums) {
  core::AuxRow out;
  out.values.reserve(attnums.size());
  for (int32_t attribute : attnums) {
    if (attribute >= 0 &&
        static_cast<std::size_t>(attribute) < src.values.size()) {
      out.values.push_back(src.values[static_cast<std::size_t>(attribute)]);
    } else {
      out.values.push_back(core::AuxValue::Null(core::AuxType::String));
    }
  }
  return out;
}

core::MutationBatch ProjectBatch(const core::MutationBatch &src,
                                 const std::vector<int32_t> &attnums) {
  core::MutationBatch out;
  out.table = src.table;
  out.kind = src.kind;
  for (const auto &row : src.old_rows) {
    out.old_rows.push_back(ProjectRow(row, attnums));
  }
  for (const auto &row : src.new_rows) {
    out.new_rows.push_back(ProjectRow(row, attnums));
  }
  for (auto physical : src.updated_columns) {
    const auto it =
        std::find(attnums.begin(), attnums.end(), static_cast<int32_t>(physical));
    if (it != attnums.end()) {
      out.updated_columns.push_back(
          static_cast<core::ColumnId>(it - attnums.begin()));
    }
  }
  return out;
}

// ---- Sessions -------------------------------------------------------------
// A session is one server connection (one THD). The server exports
// thd_get_current_thd() (the connection the calling thread serves) and
// thd_in_active_multi_stmt_transaction(); both are looked up at run time so
// the plugin still loads (and falls back to one session per OS thread, i.e.
// per connection under the default thread-per-connection model, and to
// autocommit semantics) on a server that lacks them.
struct ServerHooks {
  void *(*current_thd)() = nullptr;
  bool (*in_transaction)(const void *) = nullptr;
};

const ServerHooks &Hooks() {
  static const ServerHooks hooks = [] {
    ServerHooks h;
    h.current_thd = reinterpret_cast<void *(*)()>(
        dlsym(RTLD_DEFAULT, "_Z19thd_get_current_thdv"));
    h.in_transaction = reinterpret_cast<bool (*)(const void *)>(
        dlsym(RTLD_DEFAULT, "_Z36thd_in_active_multi_stmt_transactionPK3THD"));
    return h;
  }();
  return hooks;
}

thread_local char t_thread_marker;

// Sessions that captured something not yet committed or rolled back: the
// statement-end fast path (most statements never touch an accelerator).
std::mutex g_active_mu;
std::unordered_set<std::uintptr_t> g_active_sessions;

void MarkSessionActive(std::uintptr_t session) {
  std::lock_guard<std::mutex> lock(g_active_mu);
  g_active_sessions.insert(session);
}

bool IsSessionActive(std::uintptr_t session) {
  std::lock_guard<std::mutex> lock(g_active_mu);
  return g_active_sessions.count(session) != 0;
}

void ClearSessionActive(std::uintptr_t session) {
  std::lock_guard<std::mutex> lock(g_active_mu);
  g_active_sessions.erase(session);
}

} // namespace

void RegisterMysqlXactCallbacks() {}

bool RegisterMysqlBackendForMutations() { return true; }

void SetMysqlActiveBackend(MysqlAuxBackend *backend) {
  g_active_backend = backend;
}

MysqlAuxBackend *GetMysqlActiveBackend() { return g_active_backend; }

void RegisterMysqlMutationBackend(MysqlAuxBackend *backend) {
  if (!backend) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_backends_mu);
  for (auto *existing : g_mutation_backends) {
    if (existing == backend) {
      g_active_backend = backend;
      return;
    }
  }
  g_mutation_backends.push_back(backend);
  g_active_backend = backend;
}

void UnregisterMysqlMutationBackend(MysqlAuxBackend *backend) {
  std::lock_guard<std::mutex> lock(g_backends_mu);
  g_mutation_backends.erase(std::remove(g_mutation_backends.begin(),
                                        g_mutation_backends.end(), backend),
                            g_mutation_backends.end());
  g_backend_last_action.erase(backend);
  if (g_active_backend == backend) {
    g_active_backend =
        g_mutation_backends.empty() ? nullptr : g_mutation_backends.back();
  }
}

bool HasRegisteredMysqlMutationBackend() {
  std::lock_guard<std::mutex> lock(g_backends_mu);
  return !g_mutation_backends.empty() || g_active_backend != nullptr;
}

std::vector<MysqlAuxBackend *> RegisteredMysqlMutationBackends() {
  std::lock_guard<std::mutex> lock(g_backends_mu);
  if (!g_mutation_backends.empty()) {
    return g_mutation_backends;
  }
  if (g_active_backend) {
    return {g_active_backend};
  }
  return {};
}

std::uintptr_t MysqlSessionFromThd(const void *thd) {
  if (thd && Hooks().current_thd) {
    return reinterpret_cast<std::uintptr_t>(thd);
  }
  return reinterpret_cast<std::uintptr_t>(&t_thread_marker);
}

std::uintptr_t CurrentMysqlSession() {
  if (Hooks().current_thd) {
    if (void *thd = Hooks().current_thd()) {
      return reinterpret_cast<std::uintptr_t>(thd);
    }
  }
  return reinterpret_cast<std::uintptr_t>(&t_thread_marker);
}

bool MysqlThdInActiveTransaction(const void *thd) {
  return thd && Hooks().in_transaction && Hooks().in_transaction(thd);
}

void DispatchMysqlMutation(const core::MutationBatch &batch) {
  auto backends = RegisteredMysqlMutationBackends();
  if (backends.empty()) {
    return;
  }
  const std::uintptr_t session = CurrentMysqlSession();
  MarkSessionActive(session);
  ++g_observer_mutations;
  for (auto *backend : backends) {
    if (!backend) {
      continue;
    }
    try {
      auto attnums = backend->MutationCaptureAttnums(batch.table);
      if (!attnums || attnums->empty()) {
        continue;
      }
      backend->CaptureMutation(ProjectBatch(batch, *attnums), session);
    } catch (...) {
      // One accelerator must not skip capture for the others. This one may
      // have missed the write, so its state can no longer be trusted.
      try { backend->NoteCaptureFailure(true, session); } catch (...) {}
    }
  }
}

void InvalidateAllMysqlAccelerators(bool transaction_open) {
  const std::uintptr_t session = CurrentMysqlSession();
  if (transaction_open) {
    MarkSessionActive(session);
  }
  for (auto *backend : RegisteredMysqlMutationBackends()) {
    if (!backend) continue;
    try { backend->NoteCaptureFailure(transaction_open, session); } catch (...) {}
  }
}

namespace {

// Publishes (or discards) one session's staged writes on one backend. Commit
// and abort of captured writes are serialized with the install of a snapshot
// build (MysqlAuxBackend::BeginBuildLog) and with each other: sessions publish
// one at a time, each staged against the state the previous one left.
void CommitSession(MysqlAuxBackend *backend, std::uintptr_t session) {
  std::lock_guard<std::mutex> commit_lock(backend->CommitMutex());
  try {
    backend->OnPreCommit(session);
    backend->OnCommit(session);
    const auto entry = core::LastMaintenanceDecision();
    if (entry && entry->valid) {
      std::lock_guard<std::mutex> lock(g_backends_mu);
      g_backend_last_action[backend] =
          maintenance::MaintenanceActionName(entry->staged_action);
    }
  } catch (...) {
    // Keep the remaining accelerators' commit paths running; this one's
    // state may be missing the write.
    try { backend->OnAbort(session); } catch (...) {}
    try { backend->NoteCaptureFailure(/*transaction_open=*/false, session); } catch (...) {}
  }
}

void AbortSession(MysqlAuxBackend *backend, std::uintptr_t session) {
  std::lock_guard<std::mutex> commit_lock(backend->CommitMutex());
  try {
    backend->OnAbort(session);
  } catch (...) {
    try { backend->NoteCaptureFailure(/*transaction_open=*/false, session); } catch (...) {}
  }
}

} // namespace

void DispatchMysqlStatementEnd(std::uintptr_t session, MysqlStatementKind kind,
                               bool failed, bool in_active_transaction) {
  if (!IsSessionActive(session)) {
    return; // this session captured nothing
  }
  bool work_left = false;
  for (auto *backend : RegisteredMysqlMutationBackends()) {
    if (!backend || !backend->SessionHasWork(session)) {
      continue;
    }
    try {
      switch (kind) {
      case MysqlStatementKind::Commit:
        if (failed) {
          // The commit failed: the transaction's fate is unknown here.
          AbortSession(backend, session);
          backend->NoteCaptureFailure(/*transaction_open=*/false, session);
        } else {
          CommitSession(backend, session);
        }
        break;
      case MysqlStatementKind::Rollback:
        AbortSession(backend, session);
        break;
      case MysqlStatementKind::RollbackToSavepoint:
        // Writes after the savepoint were captured but are now undone, and
        // are not tracked individually: discard the transaction's writes at
        // its end and rebuild (a failed ROLLBACK TO changed nothing).
        backend->EndStatement(session, false);
        if (!failed) backend->PoisonSession(session);
        break;
      case MysqlStatementKind::Begin:
        // START TRANSACTION commits a transaction still open (implicit
        // commit); the new one has captured nothing yet.
        backend->EndStatement(session, failed);
        CommitSession(backend, session);
        break;
      case MysqlStatementKind::Other:
        backend->EndStatement(session, failed);
        if (!in_active_transaction) {
          // Autocommit statement (or one that implicitly ended the
          // transaction): its writes are committed, or all rolled back.
          if (failed) AbortSession(backend, session);
          else CommitSession(backend, session);
        }
        break;
      }
    } catch (...) {
      try { backend->OnAbort(session); } catch (...) {}
      try { backend->NoteCaptureFailure(/*transaction_open=*/false, session); } catch (...) {}
    }
    work_left = work_left || backend->SessionHasWork(session);
  }
  if (!work_left) {
    ClearSessionActive(session);
  }
}

void DispatchMysqlSessionEnd(std::uintptr_t session) {
  if (!IsSessionActive(session)) {
    return;
  }
  for (auto *backend : RegisteredMysqlMutationBackends()) {
    if (!backend) continue;
    try { backend->EndSession(session); } catch (...) {}
  }
  ClearSessionActive(session);
}

int MysqlObserverMutationCount() { return g_observer_mutations.load(); }

void MysqlResetObserver() { g_observer_mutations = 0; }

void MysqlSetForceMaintenanceAction(const char *action_name) {
  core::ClearMaintenanceDecisionLog();
  core::SetForcedMaintenanceActionName(action_name ? action_name : "");
  std::lock_guard<std::mutex> lock(g_backends_mu);
  g_backend_last_action.clear();
}

void MysqlSetExpectedQueries(double expected_queries) {
  core::SetExpectedQueriesOverride(expected_queries);
}

double MysqlExpectedQueries() { return core::ExpectedQueriesOverride(); }

void NoteMysqlBackendLastAction(MysqlAuxBackend *backend) {
  if (!backend) {
    return;
  }
  const auto entry = core::LastMaintenanceDecision();
  if (!entry || !entry->valid) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_backends_mu);
  g_backend_last_action[backend] =
      maintenance::MaintenanceActionName(entry->staged_action);
}

const char *MysqlLastMaintenanceAction() {
  static thread_local std::string cached;
  MysqlAuxBackend *active = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_backends_mu);
    active = g_active_backend;
    if (active) {
      auto it = g_backend_last_action.find(active);
      if (it != g_backend_last_action.end() && !it->second.empty()) {
        cached = it->second;
        return cached.c_str();
      }
    }
  }
  const auto entry = core::LastMaintenanceDecision();
  if (!entry || !entry->valid) {
    cached = "None";
  } else {
    cached = maintenance::MaintenanceActionName(entry->staged_action);
  }
  return cached.c_str();
}

} // namespace accelerator_codegen::aux_state::engine::mysql

extern "C" int mysql_accel_get_observer_mutation_count() {
  return accelerator_codegen::aux_state::engine::mysql::MysqlObserverMutationCount();
}
