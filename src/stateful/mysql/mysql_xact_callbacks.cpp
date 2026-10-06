#include "accelerator_codegen/stateful/mysql/mysql_xact_callbacks.hpp"

#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_mutation_capture.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::engine::mysql {
namespace core = accelerator_codegen::aux_state::core;
namespace maintenance = accelerator_codegen::aux_state::maintenance;

namespace {

MysqlAuxBackend *g_active_backend = nullptr;
std::mutex g_backends_mu;
std::vector<MysqlAuxBackend *> g_mutation_backends;
int g_observer_mutations = 0;
bool g_schemas_registered = false;
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

} // namespace

void RegisterTpchCaptureSchemas() {
  if (g_schemas_registered) {
    return;
  }
  using T = core::AuxType;
  auto Cols = [](std::initializer_list<T> types) {
    std::vector<CaptureColumn> cols;
    for (auto type : types) {
      cols.push_back(CaptureColumn{type, true});
    }
    return cols;
  };
  RegisterCaptureTableSchema(
      tpch_tables::kLineitem,
      Cols({T::Int64, T::Int64, T::Int64, T::Int64, T::ScaledDecimal,
            T::ScaledDecimal, T::ScaledDecimal, T::ScaledDecimal, T::String,
            T::String, T::Date, T::Date, T::Date, T::String, T::String,
            T::String}));
  RegisterCaptureTableSchema(
      tpch_tables::kOrders,
      Cols({T::Int64, T::Int64, T::String, T::ScaledDecimal, T::Date, T::String,
            T::String, T::Int64, T::String}));
  RegisterCaptureTableSchema(
      tpch_tables::kCustomer,
      Cols({T::Int64, T::String, T::String, T::Int64, T::String,
            T::ScaledDecimal, T::String, T::String}));
  RegisterCaptureTableSchema(
      tpch_tables::kPart,
      Cols({T::Int64, T::String, T::String, T::String, T::String, T::Int64,
            T::String, T::ScaledDecimal, T::String}));
  RegisterCaptureTableSchema(
      tpch_tables::kPartsupp,
      Cols({T::Int64, T::Int64, T::Int64, T::ScaledDecimal, T::String}));
  RegisterCaptureTableSchema(
      tpch_tables::kSupplier,
      Cols({T::Int64, T::String, T::String, T::Int64, T::String,
            T::ScaledDecimal, T::String}));
  RegisterCaptureTableSchema(
      tpch_tables::kNation, Cols({T::Int64, T::String, T::Int64, T::String}));
  RegisterCaptureTableSchema(tpch_tables::kRegion,
                             Cols({T::Int64, T::String, T::String}));
  g_schemas_registered = true;
}

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
  RegisterTpchCaptureSchemas();
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

void DispatchMysqlMutation(const char *relname,
                           const core::MutationBatch &mutation) {
  core::MutationBatch batch = mutation;
  if (relname && *relname) {
    const auto id = tpch_tables::IdFromRelname(relname);
    if (id != 0) {
      batch.table = id;
    }
  }
  auto backends = RegisteredMysqlMutationBackends();
  if (backends.empty()) {
    return;
  }
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
      backend->CaptureMutation(ProjectBatch(batch, *attnums));
    } catch (...) {
      // One accelerator must not skip capture for the others. This one may
      // have missed the write, so its state can no longer be trusted.
      try { backend->NoteCaptureFailure(); } catch (...) {}
    }
  }
}

void InvalidateAllMysqlAccelerators(bool transaction_open) {
  for (auto *backend : RegisteredMysqlMutationBackends()) {
    if (!backend) continue;
    try { backend->NoteCaptureFailure(transaction_open); } catch (...) {}
  }
}

void DispatchMysqlTxnEvent(bool is_commit, bool is_rollback) {
  auto backends = RegisteredMysqlMutationBackends();
  for (auto *backend : backends) {
    if (!backend) {
      continue;
    }
    // Commit/abort of captured writes are serialized with the install of a
    // snapshot build (MysqlAuxBackend::BeginBuildLog).
    std::lock_guard<std::mutex> commit_lock(backend->CommitMutex());
    try {
      if (is_commit) {
        backend->OnPreCommit();
        backend->OnCommit();
        const auto entry = core::LastMaintenanceDecision();
        if (entry && entry->valid) {
          std::lock_guard<std::mutex> lock(g_backends_mu);
          g_backend_last_action[backend] =
              maintenance::MaintenanceActionName(entry->staged_action);
        }
      } else if (is_rollback) {
        backend->OnAbort();
      }
    } catch (...) {
      // Keep remaining accelerators' commit/abort paths running; this one's
      // state may be missing the write.
      try { backend->NoteCaptureFailure(/*transaction_open=*/false); } catch (...) {}
    }
  }
}

int MysqlObserverMutationCount() { return g_observer_mutations; }

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
