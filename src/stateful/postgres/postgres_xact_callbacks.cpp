// GENERATED BACKEND - PostgreSQL transaction callbacks + mutation fan-out
#include "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"

#include <algorithm>
#include <exception>
#include <mutex>
#include <vector>

extern "C" {
#include <postgres.h>
#include <access/xact.h>
#include <utils/snapmgr.h>
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

static PostgresAuxBackend *g_active_backend = nullptr;
static bool g_callbacks_registered = false;
static std::mutex g_backends_mu;
static std::vector<PostgresAuxBackend *> g_mutation_backends;

void XactCallbackBridge(XactEvent event, void *arg) {
  (void)arg;
  std::vector<PostgresAuxBackend *> backends;
  {
    std::lock_guard<std::mutex> lock(g_backends_mu);
    backends = g_mutation_backends;
  }
  if (backends.empty() && g_active_backend) {
    backends.push_back(g_active_backend);
  }
  for (auto *backend : backends) {
    if (!backend)
      continue;
    switch (event) {
    case XACT_EVENT_PRE_COMMIT: {
      // Statement portals have already popped their snapshot; SPI scans in
      // StageMaintenance / PreCommit rebuild need an active one (interface-v1).
      PushActiveSnapshot(GetTransactionSnapshot());
      PG_TRY();
      {
        try {
          backend->OnPreCommit();
        } catch (const std::exception &ex) {
          ereport(ERROR,
                  (errmsg("accelerator pre-commit failed: %s", ex.what())));
        }
      }
      PG_CATCH();
      {
        PopActiveSnapshot();
        PG_RE_THROW();
      }
      PG_END_TRY();
      PopActiveSnapshot();
      break;
    }
    case XACT_EVENT_COMMIT:
      backend->OnCommit();
      break;
    case XACT_EVENT_ABORT:
      backend->OnAbort();
      break;
    default:
      break;
    }
  }
}

} // anonymous namespace

void RegisterPostgresXactCallbacks() {
  if (g_callbacks_registered)
    return;
  RegisterXactCallback(XactCallbackBridge, nullptr);
  g_callbacks_registered = true;
}

bool RegisterPostgresBackendForMutations() {
  bool first = !g_callbacks_registered;
  if (first) {
    RegisterPostgresXactCallbacks();
  }
  return first;
}

void SetPostgresActiveBackend(PostgresAuxBackend *backend) {
  g_active_backend = backend;
}

void RegisterPostgresMutationBackend(PostgresAuxBackend *backend) {
  if (!backend)
    return;
  std::lock_guard<std::mutex> lock(g_backends_mu);
  for (auto *b : g_mutation_backends) {
    if (b == backend)
      return;
  }
  g_mutation_backends.push_back(backend);
  // Newest runtime is the default target for xact hooks (single-query benches).
  g_active_backend = backend;
}

bool HasRegisteredPostgresMutationBackend() {
  std::lock_guard<std::mutex> lock(g_backends_mu);
  return !g_mutation_backends.empty() || g_active_backend != nullptr;
}

void UnregisterPostgresMutationBackend(PostgresAuxBackend *backend) {
  std::lock_guard<std::mutex> lock(g_backends_mu);
  g_mutation_backends.erase(
      std::remove(g_mutation_backends.begin(), g_mutation_backends.end(),
                  backend),
      g_mutation_backends.end());
  if (g_active_backend == backend) {
    g_active_backend =
        g_mutation_backends.empty() ? nullptr : g_mutation_backends.back();
  }
}

void DispatchPostgresMutation(const char *relname,
                              const core::MutationBatch &mutation) {
  core::MutationBatch batch = mutation;
  if (relname && *relname) {
    const auto id = tpch_tables::IdFromRelname(relname);
    if (id != 0) {
      batch.table = id;
    }
  }

  std::vector<PostgresAuxBackend *> backends;
  {
    std::lock_guard<std::mutex> lock(g_backends_mu);
    backends = g_mutation_backends;
  }
  if (backends.empty() && g_active_backend) {
    backends.push_back(g_active_backend);
  }
  for (auto *backend : backends) {
    if (backend) {
      backend->CaptureMutation(batch);
    }
  }
}

PostgresAuxBackend *GetPostgresActiveBackend() { return g_active_backend; }

std::vector<PostgresAuxBackend *> RegisteredPostgresMutationBackends() {
  std::lock_guard<std::mutex> lock(g_backends_mu);
  if (!g_mutation_backends.empty()) {
    return g_mutation_backends;
  }
  if (g_active_backend) {
    return {g_active_backend};
  }
  return {};
}

} // namespace accelerator_codegen::aux_state::engine::postgres
