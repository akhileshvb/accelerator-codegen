// GENERATED BACKEND - PostgreSQL transaction callbacks
#include "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"

extern "C" {
#include <postgres.h>
#include <access/xact.h>
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

static PostgresAuxBackend *g_active_backend = nullptr;
static bool g_callbacks_registered = false;

void XactCallbackBridge(XactEvent event, void *arg) {
  (void)arg;
  if (!g_active_backend) return;
  switch (event) {
    case XACT_EVENT_PRE_COMMIT:
      g_active_backend->OnPreCommit();
      break;
    case XACT_EVENT_COMMIT:
      g_active_backend->OnCommit();
      break;
    case XACT_EVENT_ABORT:
      g_active_backend->OnAbort();
      break;
    default:
      break;
  }
}

} // anonymous namespace

void RegisterPostgresXactCallbacks() {
  if (g_callbacks_registered) return;
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

} // namespace accelerator_codegen::aux_state::engine::postgres
