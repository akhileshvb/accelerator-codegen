#include "accelerator_codegen/aux_state/engine/postgres/postgres_xact_callbacks.hpp"

#include <algorithm>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

namespace {
std::vector<PostgresAuxBackend *> backends;
}

void RegisterTransactionBackend(PostgresAuxBackend *backend_p) {
  if (std::find(backends.begin(), backends.end(), backend_p) == backends.end()) {
    backends.push_back(backend_p);
  }
}

void UnregisterTransactionBackend(PostgresAuxBackend *backend_p) {
  backends.erase(std::remove(backends.begin(), backends.end(), backend_p),
                 backends.end());
}

void HandleTransactionEvent(TransactionEvent event) {
  if (backends.empty()) return;
  for (auto *backend : backends) {
    switch (event) {
    case TransactionEvent::PreCommit:
      backend->OnPreCommit();
      break;
    case TransactionEvent::Commit:
      backend->OnCommit();
      break;
    case TransactionEvent::Abort:
      backend->OnAbort();
      break;
    case TransactionEvent::SubtransactionStart:
      backend->OnSubtransactionStart();
      break;
    case TransactionEvent::SubtransactionCommit:
      backend->OnSubtransactionCommit();
      break;
    case TransactionEvent::SubtransactionAbort:
      backend->OnSubtransactionAbort();
      break;
    }
  }
}

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)
extern "C" void accelerator_codegen_postgres_xact_callbacks_link_anchor() {}
#endif

} // namespace accelerator_codegen::aux_state::engine::postgres
