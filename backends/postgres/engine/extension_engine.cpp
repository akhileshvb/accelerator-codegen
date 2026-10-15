// GENERATED BACKEND — Postgres MODULE entry point (production).
// PG_MODULE_MAGIC and _PG_init for accelerator_codegen_pg: initializes the
// backend bridge (postgres_bridge.cpp) and defines the shard-worker GUCs.
// Mutation capture is the statement-level transition trigger
// (transition_capture.cpp); transaction callbacks are in
// postgres_xact_callbacks.cpp; query-facing SQL entry points are in
// backends/postgres/api/extension_api.cpp.
//
// The engine_backend gate-test UDFs (txn / mutations / scan / runtime /
// visibility toy bridges) are not here: they live in
// backends/postgres/tests/extension_test_engine.cpp and are built only into the
// test MODULE accelerator_codegen_pg_test.

extern "C" {
#include <postgres.h>
#include <fmgr.h>
PG_MODULE_MAGIC;
}

#include "backends/postgres/state/shard_worker.hpp"

extern "C" {

void accel_pg_init_backend(const char *backing_dir);

void _PG_init(void) {
  accel_pg_init_backend(nullptr);
  accelerator_codegen::aux_state::engine::postgres::shard::DefineGucs();
}

} // extern "C"
