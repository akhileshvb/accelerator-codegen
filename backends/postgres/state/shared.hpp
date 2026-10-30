// GENERATED BACKEND
//
// What a database's backends and shard workers share (S2-S4, S6), in the
// DSM registry (no shared_preload_libraries needed).
//
//   Accelerator index  i in [0, kMaxAccelerators): assigned when an
//                      accelerator is first used in the database. Shard
//                      worker i hosts accelerator i and nothing else.
//   Commit epochs      per accelerator and dense: the n-th commit that wrote
//                      any of accelerator i's tables has epoch n. A commit
//                      takes gate_i (a session advisory lock, accelerator
//                      index order) at pre-commit, takes n, publishes
//                      (n, xid) in ring_i, and releases the gate once it is
//                      visible. So i's commits become visible in epoch order
//                      and every snapshot sees a prefix 1..k of them.
//   Reader epoch       k = the largest n in ring_i whose xid the snapshot
//                      sees (all of i's commits up to n are then visible).
//   Thread tokens      accel.threads tokens, one pool for all shard workers.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"
#include "substrate/aux/runtime/thread_budget.hpp"

extern "C" {
#include <postgres.h>
#include <utils/snapshot.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shared {

using runtime::Epoch;

inline constexpr int kMaxAccelerators = 64;
// Accelerators per shard.
inline constexpr uint32_t kMaxMembers = 32;

// ---- registry ----------------------------------------------------------------
// The index of `id` in this database, activating it on first use (which
// starts its shard worker, shard_worker.hpp). -1 if the registry is full.
int Activate(const core::AcceleratorId &id);
// The index of `id` if active, else -1.
int Find(const core::AcceleratorId &id);
core::AcceleratorId IdOf(int index);
// Active accelerators; Generation() changes whenever the set does.
std::vector<int> Active();
uint64_t Generation();

// ---- commit epochs (S2), writer side ----------------------------------------
// PRE_COMMIT: takes gate_i for every index in `indices` (ascending), assigns
// each its next epoch and publishes (epoch, xid) in its ring. Returns the
// epochs in `indices` order. May raise (cancel while waiting for a gate).
std::vector<Epoch> TakeCommitEpochs(const std::vector<int> &indices, TransactionId xid);
// COMMIT (after the commit is visible) and ABORT: releases every gate held.
void ReleaseGates();
// Whether this transaction wrote any of `tables` (its own reads of an
// accelerator over them answer from SQL: no state has them). Defined with the
// transaction's staged writes, postgres_xact_callbacks.cpp.
bool TransactionWrote(const std::vector<core::TableId> &tables);
// Highest epoch assigned so far for accelerator `index`.
Epoch AssignedEpoch(int index);
// The worker of a verified-store shard: every member has applied through `applied`, and is alive.
void PublishApplied(int index, Epoch applied);
void PublishCatchingUp(int index, bool catching_up);
// A reader of such a shard at `epoch`: true if it must wait for the worker (writers' next commits then
// wait for it); ReaderWaitEnded after.
bool NoteReaderWaiting(int index, Epoch epoch);
void ReaderWaitEnded(int index);

// ---- SQL fallback time (C11) ----------------------------------------------------
// What a backend's SQL fallback of member `member` of shard `index` took (moving average,
// microseconds; 0: never measured). The shard worker weighs it against waiting.
void NoteSqlFallbackUs(int index, uint32_t member, double us);
double SqlFallbackUs(int index, uint32_t member);

// ---- reader epoch (S3) and version floor (S4) -------------------------------
// The newest epoch of accelerator `index` that `snapshot` sees. nullopt if
// the snapshot is older than what the ring remembers, or does not see the
// writes of transactions that began before the accelerator was activated
// (they have no epoch, but the build contains them): answer from SQL.
std::optional<Epoch> ReaderEpoch(int index, Snapshot snapshot);
// An epoch every open snapshot in the database sees: no reader will ever ask
// for an older one. Uses Postgres APIs (worker main thread).
Epoch HorizonEpoch(int index);

// ---- thread budget (C12) ------------------------------------------------------
// accel.threads (default: the CPUs this process may run on), and the token
// pool that caps the sum over all of the database's shard workers.
std::size_t ThreadBudget();
// The pool, charged to shard worker `index`.
std::shared_ptr<runtime::TokenSource> ThreadTokens(int index);
// Returns the tokens shard `index`'s worker held: it exited, or a dead one's remain.
void ReleaseHeldTokens(int index);

// accel.commit_ship_timeout_ms: how long COMMIT waits to queue its commit
// messages for a shard worker.
std::chrono::milliseconds CommitShipTimeout();

// Defines accel.threads and accel.commit_ship_timeout_ms (call from _PG_init).
void DefineGucs();

} // namespace accelerator_codegen::aux_state::engine::postgres::shared

#endif
