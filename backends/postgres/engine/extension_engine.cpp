// GENERATED BACKEND — Postgres MODULE entry point (production).
// PG_MODULE_MAGIC and _PG_init for accelerator_codegen_pg: defines the GUCs.
// Mutation capture is the statement-level transition trigger
// (transition_capture.cpp); the commit lifecycle is in
// postgres_xact_callbacks.cpp, the registry and epochs in state/shared.cpp, the
// shard workers in state/shard_worker.cpp; query-facing SQL entry points are in
// backends/postgres/api/extension_api.cpp.

extern "C" {
#include <postgres.h>
#include <fmgr.h>
PG_MODULE_MAGIC;
}

#include <execinfo.h>
#include <csignal>
#include <unistd.h>
#include "backends/postgres/state/shard_worker.hpp"
#include "backends/postgres/state/shared.hpp"

extern "C" {

static void DbgSegv(int) { void *b[50]; int n = backtrace(b, 50); backtrace_symbols_fd(b, n, 2); _exit(139); }
void _PG_init(void) {
  signal(SIGSEGV, DbgSegv);
  namespace postgres = accelerator_codegen::aux_state::engine::postgres;
  postgres::shared::DefineGucs();
  postgres::shard::DefineGucs();
}

} // extern "C"
