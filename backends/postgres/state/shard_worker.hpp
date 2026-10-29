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
// thread), in a per-shard DSM segment (shard_segment.hpp).
//
// A shard worker is a dynamic background worker (no shared_preload_libraries):
// the first backend to use an accelerator starts it, it exits on SIGTERM or
// (accel.shard_idle_exit_s) when idle, and the next use starts it again. A
// Postgres error inside the worker ends it the same way.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <chrono>
#include <string>
#include <string_view>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/state/shard_segment.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"

extern "C" {
#include <postgres.h>
#include <fmgr.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {

// Shard `index`'s shared memory in this database (attached on first use).
ShardSegment *Segment(int index);

// Starts shard worker `index` if it is not running (shared::Activate), and
// returns once it accepts requests.
void EnsureWorker(int index);

// Writer side, COMMIT: queues one encoded CommitMessage (messages.hpp) for
// shard `index`. Blocks while the ring is full, up to `deadline`; false if
// it could not be queued (the worker then sees a gap and rebuilds). A shard
// whose worker is not running is owed nothing (its next build includes the
// commit): true.
bool SendCommit(int index, std::string_view message,
                std::chrono::steady_clock::time_point deadline);

enum class Remote { Served, Fallback };
// Reader side: runs the read of member `member` of shard `index` at `epoch` and
// writes the rows into fcinfo's materialized result. Fallback: answer from SQL
// (state not servable in time, worker unavailable or failed). Never raises for
// a worker problem.
Remote RemoteRead(int index, uint32_t member, runtime::Epoch epoch, const core::QueryArguments &arguments,
                  FunctionCallInfo fcinfo);

// The shard and member position of `accelerator`, activating its shard (which starts its worker).
// False: no shard (registry full).
bool ShardPosition(const PostgresAccelerator &accelerator, int &index, uint32_t &member);
// Reader side for any accelerator: its shard's worker answers at the statement snapshot's epoch.
// False: answer from SQL (this transaction wrote the shard's tables, the snapshot is older than
// the epoch ring remembers, or the worker could not serve it).
bool ServeFromShard(const PostgresAccelerator &accelerator, const core::QueryArguments &arguments,
                    FunctionCallInfo fcinfo);

// Defines the accel.* GUCs of the readers and workers (call from _PG_init).
void DefineGucs();
// accel.build_tuning: planner settings for state builds (the worker's).
bool BuildTuning();

// "requests=N served=N fallback=N errors=N commits=N applied=N builds=N gaps=N
// synced=N workers=N generation=G pid=P" summed over the database's running
// shard workers, or "no worker".
std::string StatsText();
// Stops the database's shard workers and waits for them (they restart on
// demand). False if none was running.
bool StopWorkers();

// Maintenance diagnostics, bridged to the workers (maintenance runs there): a
// forced action for their next decisions ("" clears), and the last decision
// any of them made, as core::FormatLastMaintenanceDecision / its action name
// ("" if none).
void SetForcedMaintenance(const std::string &action_name);
std::string LastMaintenanceActionName();
std::string LastMaintenanceDecisionText();

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

extern "C" {
PGDLLEXPORT void accel_shard_worker_main(Datum main_arg);
}

#endif
