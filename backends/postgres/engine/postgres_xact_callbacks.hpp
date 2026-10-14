// GENERATED BACKEND - PostgreSQL transaction callbacks
#pragma once

#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

class PostgresAuxBackend;

// Register PostgreSQL transaction (XACT_EVENT) callbacks that bridge
// PostgresAuxBackend PreCommit/Commit/Abort into the PG transaction system.
void RegisterPostgresXactCallbacks();

// Register a backend for mutation capture.
bool RegisterPostgresBackendForMutations();

// Fan-out registry: Runtime instances register so trigger captures reach the
// coordinator that owns their AuxQueryModule (not the empty BridgeState one).
void RegisterPostgresMutationBackend(PostgresAuxBackend *backend);

// Snapshot of backends currently registered for mutation fan-out.
std::vector<PostgresAuxBackend *> RegisteredPostgresMutationBackends();

} // namespace accelerator_codegen::aux_state::engine::postgres
