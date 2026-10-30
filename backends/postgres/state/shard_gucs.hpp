// GENERATED BACKEND
//
// The accel.* settings of the shard workers and their readers (DefineGucs is
// in shard_worker.hpp).
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

namespace accelerator_codegen::aux_state::engine::postgres::shard {

// Readers (session settings).
// accel.read_timeout_ms: how long a query waits for a shard worker before it
// answers from SQL.
int ReadTimeoutMs();
// accel.read_wait_ms: how long the worker may make a read wait for commits in
// flight, a build or a catch-up before declining it.
int ReadWaitMs();
// accel.force_fallback: every read answers from SQL (differential tests).
bool ForceFallback();

// Workers (server settings).
// accel.shard_read_threads: threads serving reads.
int ReadThreads();
// accel.read_threads / build_threads / maint_threads: caps of the module's parallel
// reads, builds and maintenance (0: the runtime's default).
int PhaseReadThreads();
int PhaseBuildThreads();
int PhaseMaintenanceThreads();
// accel.per_commit_versions: verified-store shards apply and publish one commit at a time.
bool PerCommitVersions();
// accel.shard_idle_exit_s: a worker exits after this much idleness (0: never).
int IdleExitSeconds();

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

#endif
