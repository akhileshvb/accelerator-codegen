// Shared aux state server for PostgreSQL (workload-agnostic).
//
// Background workers own the accelerators' state in their own heaps; backends
// do not hold a copy.  Per-backend mode (the default) is unchanged; this mode
// is selected by the GUC accel.shared_state.  Each database has one worker per
// shard (accel.shared_sharding, on by default: one shard per TPC-H
// accelerator accel_tpch_q<N>_aux, shard 0 for anything else; off: shard 0
// serves everything), so reads of different accelerators and the applies for
// them never queue behind each other.
//
//   Reads   A backend's accelerator UDF sends (function oid, arguments,
//           statement snapshot) to the worker over a per-backend shm_mq pair.
//           The worker runs the same SQL function under that snapshot with the
//           same Runtime (hit / rebuild / decline logic, aux_sync counters and
//           snapshot cover/see checks), and streams the result tuples back.  A
//           decline (or an unavailable / timed-out worker, or a worker error)
//           makes the backend answer from SQL (ExecuteFallback): never a wrong
//           answer, only a slower one.
//
//   Writes  Captured statement-level transition rows are staged per backend
//           transaction (with savepoint frames).  At PRE_COMMIT the backend
//           takes the commit gate (a transaction-scoped lock) and serialises
//           what it will ship; at COMMIT, after the counters are published, it
//           sends each shard the batches of the accelerators it hosts (only
//           shards hosting an accelerator on a written table), and then waits
//           (bounded) for all their applies, which run in parallel.
//           Nothing uncommitted ever reaches the worker, so aborts and dying
//           backends need no cleanup there.  The worker applies the batches
//           through the existing coordinator / module code, then advances the
//           sync marker.  All of a transaction's captured statements for an
//           accelerator are applied in order, under a snapshot taken right
//           after the commit (the commit gate and the counters exclude any
//           other write to its tables): module lookups therefore see the
//           database as the whole transaction left it, not as each statement
//           did.  That is what every TPC-H module expects: lookups in
//           StageMaintenance run at pre-commit anyway, and those in
//           AccumulateMutation (Q3, Q21) replace each touched order's record
//           from a fresh read, so a later read of the same key wins.  A commit
//           that is too large, poisoned by an aborted savepoint, or that the
//           worker did not apply, leaves the
//           shared counters ahead of the worker's marker, so it stops serving
//           hits until it rebuilds.  Correctness rests on those counters, not
//           on message delivery.
//
// The worker needs no shared_preload_libraries: a backend starts it on demand
// (dynamic background worker, one per database and shard) and everything else lives in
// the DSM registry.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/postgres/aux_sync.hpp"

extern "C" {
#include <postgres.h>
#include <fmgr.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::aux_server {

// ---- settings --------------------------------------------------------------
// Defines the accel.shared_* GUCs (call from _PG_init).
void DefineGucs();
// accel.log_decisions: log every accelerator decision (diagnostics).
bool LogDecisions();
// True in the worker process.
bool InServer();
// accel.shared_state is on (and this is not the worker).
bool SharedEnabled();

// ---- worker side: the runtimes it hosts -------------------------------------
struct ApplyInfo {
  TransactionId xid = InvalidTransactionId;
  std::array<bool, aux_sync::kSlots> wrote{};
  std::array<uint64_t, aux_sync::kSlots> old_seq{};
};
// What one committed transaction captured for one accelerator.
struct ApplyEntry {
  // Counter slots whose writes the capture examined for this accelerator.
  std::vector<std::size_t> examined_slots;
  // In statement order.
  std::vector<core::MutationBatch> batches;
};

class ServerRuntime {
public:
  virtual ~ServerRuntime() = default;
  virtual const core::AcceleratorId &ServerKey() const = 0;
  virtual std::vector<core::TableDependency> ServerDependencies() const = 0;
  // `entry` is null when the commit shipped nothing for this accelerator.
  virtual void ServerApply(const ApplyInfo &info, const ApplyEntry *entry) = 0;
  virtual void ServerInvalidate() = 0;
};

// A Runtime constructed in the worker registers here (and publishes its
// capture projection for the writers).
void RegisterServerRuntime(ServerRuntime *runtime);

// Set by Runtime during one request: the answer path, or that it declined.
void NoteServed(core::AuxExecutePath path);
void NoteDeclined();
// The runtime executing the current request (invalidated if the request fails).
void NoteCurrentRuntime(ServerRuntime *runtime);

// Counts one apply outcome (the commit was folded into the state or not).
void NoteApplyOutcome(bool synced);

// In the worker: a writer has committed (or is committing) a transaction it
// ships, and the apply has not been handled yet (bounded by the apply timeout).
// A state that is behind only by such a commit is about to be caught up.
bool ApplyPending();

// Marks the worker busy (a rebuild) so writers do not queue behind it.
class BusyScope {
public:
  BusyScope();
  ~BusyScope();
  BusyScope(const BusyScope &) = delete;
  BusyScope &operator=(const BusyScope &) = delete;

private:
  bool active;
};

// ---- reader side ------------------------------------------------------------
enum class Remote { Served, Declined, Unavailable };
// Runs the current SQL function (fcinfo) on the worker under the active
// snapshot.  On Served the result tuples are in fcinfo's materialized result
// and `path` says how the worker answered.  Never raises for a worker
// problem; Declined / Unavailable mean "answer from SQL yourself".
Remote RemoteExecute(FunctionCallInfo fcinfo, core::AuxExecutePath &path);

// ---- writer side ------------------------------------------------------------
struct CaptureTarget {
  std::string key; // the accelerator
  // Physical 0-based attnums in the order the module expects; empty: all.
  std::vector<int32_t> attnums;
};
// Accelerators the worker hosts that depend on `table`.
std::vector<CaptureTarget> CaptureTargets(core::TableId table);
// This statement was examined for `key` on counter slot `slot` (even if it
// produced no batch).
void StageExamined(const std::string &key, std::size_t slot);
void StageBatch(const std::string &key, core::MutationBatch batch,
                std::size_t depth);
void OnSubCommit(std::size_t depth);
void OnSubAbort(std::size_t depth);
// PRE_COMMIT: decides what ships, takes the commit gate, serialises.
// May raise (cancel while waiting for the gate); then nothing ships.
void PrepareCommit();
// COMMIT (after aux_sync::PublishCommit): ships and waits for the apply.
// Never raises.
void ShipCommit(const aux_sync::CommitInfo &info);
// Transaction end (commit or abort): forgets the staged writes.
void ResetStage();

// ---- diagnostics ------------------------------------------------------------
// "requests=N hits=N rebuilds=N declined=N errors=N applies=N synced=N
//  unsynced=N generation=G pid=P" summed over this database's shard workers
// (generation: the largest; pid: the lowest running shard's), or "no worker".
std::string StatsText();
// Stops all of this database's shard workers (they restart on demand).
// Returns false if none was running.
bool StopWorker();

} // namespace accelerator_codegen::aux_state::engine::postgres::aux_server

extern "C" {
PGDLLEXPORT void accel_aux_server_main(Datum main_arg);
}

#endif
