// GENERATED BACKEND
//
// Shard workers (S6): one background worker per active accelerator, hosting
// it on the substrate runtime (RuntimeHost + AcceleratorRuntime).
//
//   main thread    the only thread that calls Postgres: builds (S5/S8, SPI),
//                  maintenance of shipped commits under a snapshot whose
//                  epoch it reports (MaintenanceLookupEpoch), the version
//                  floor (HorizonEpoch), interrupts.
//   commit thread  receives commits from the shard's commit ring and offers
//                  them to the runtime (never blocks writers behind a build).
//   read threads   serve read requests at the reader's epoch: the module's
//                  Execute only, no Postgres calls (all signals blocked).
//
// Transport is accel::ShmRing (futex wake-ups, usable off the Postgres
// thread), in a per-shard DSM segment.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <chrono>
#include <string>
#include <string_view>

#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_types.hpp"

extern "C" {
#include <postgres.h>
#include <fmgr.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {

// Starts shard worker `index` if it is not running (shared::Activate).
void EnsureWorker(int index);

// Writer side, COMMIT: queues one encoded CommitMessage (messages.hpp) for
// shard `index`. Blocks while the ring is full, up to `deadline`; false if
// it could not be queued (the worker then sees a gap and rebuilds).
bool SendCommit(int index, std::string_view message,
                std::chrono::steady_clock::time_point deadline);

enum class Remote { Served, Fallback };
// Reader side: runs the read on shard `index` at `epoch` and writes the rows
// into fcinfo's materialized result. Fallback: answer from SQL (state not
// servable in time, worker unavailable or failed). Never raises for a worker
// problem.
Remote RemoteRead(int index, runtime::Epoch epoch, const core::QueryArguments &arguments,
                  FunctionCallInfo fcinfo);

// Defines the accel.shard_* GUCs (call from _PG_init).
void DefineGucs();
// "requests=N served=N fallback=N builds=N applied=N ..." over the
// database's shard workers, or "no worker".
std::string StatsText();
// Stops the database's shard workers (they restart on demand).
bool StopWorkers();

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

extern "C" {
PGDLLEXPORT void accel_shard_worker_main(Datum main_arg);
}

#endif
