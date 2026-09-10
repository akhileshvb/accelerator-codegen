// GENERATED BACKEND - PostgreSQL transaction callbacks
#pragma once

namespace accelerator_codegen::aux_state::engine::postgres {

class PostgresAuxBackend;

// Register PostgreSQL transaction (XACT_EVENT) callbacks that bridge
// PostgresAuxBackend PreCommit/Commit/Abort into the PG transaction system.
void RegisterPostgresXactCallbacks();

// Register a backend for mutation capture.
bool RegisterPostgresBackendForMutations();

// Set the active backend pointer for callback dispatch.
void SetPostgresActiveBackend(PostgresAuxBackend *backend);

} // namespace accelerator_codegen::aux_state::engine::postgres
