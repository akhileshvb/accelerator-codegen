#pragma once

#include "accelerator_codegen/aux_state/engine/postgres/postgres_aux_backend.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

enum class TransactionEvent : uint8_t {
  PreCommit,
  Commit,
  Abort,
  SubtransactionStart,
  SubtransactionCommit,
  SubtransactionAbort,
};

// The PostgreSQL ABI bridge registers the backend-local coordinator once during
// backend initialization, then forwards xact.c notifications here. A backend
// process owns this pointer; it is never stored in shared memory.
void RegisterTransactionBackend(PostgresAuxBackend *backend);
void UnregisterTransactionBackend(PostgresAuxBackend *backend);
void HandleTransactionEvent(TransactionEvent event);

} // namespace accelerator_codegen::aux_state::engine::postgres
