// GENERATED BACKEND
//
// Verus kits (substrate/verus/kit/verus_kit.hpp) on Postgres: the kit's engine
// services over SPI, and one accelerator, SQL function and executor per
// registered kit glue.  Names no query.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <stdexcept>

#include "substrate/verus/kit/verus_kit.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Thrown by a host call made off the main thread: a kit that declines a read in a worker read thread
// cannot answer it from SQL there, so the read goes back to its backend (the kit's reason is its cause).
struct HostOffMainThread : std::runtime_error {
  HostOffMainThread() : std::runtime_error("Postgres is called from the main thread only") {}
};

// SQL as the current transaction's active snapshot sees it.  Main thread only:
// the shard worker's read threads never call Postgres.
verus_kit::Host &SpiVerusHost();

// CREATE FUNCTION accel_<glue id>(<parameters>) RETURNS TABLE(<result of the query>) for every
// registered kit whose tables exist; returns the number created.  Run by accel_register_queries().
int RegisterVerusKitFunctions();

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
