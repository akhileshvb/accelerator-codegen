// Client programs (libpq tests) link libaccelerator_codegen, whose Postgres
// sources (stateful/postgres/snapshot_token.cpp) reference symbols only the
// Postgres server provides. These definitions satisfy the loader; a client
// never runs that code (it runs inside the backend, via the extension).
#include <cstdlib>

extern "C" {
void *ExecutorStart_hook = nullptr;
void standard_ExecutorStart(void *, int) { std::abort(); }
void *GetActiveSnapshot(void) { std::abort(); }
bool ActiveSnapshotSet(void) { std::abort(); }
}
