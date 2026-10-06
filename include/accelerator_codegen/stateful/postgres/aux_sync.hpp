// GENERATED BACKEND - cross-backend Aux synchronisation for PostgreSQL.
//
// Every PostgreSQL backend keeps a private (process-local) Aux store, so a
// store may only serve a hit when no other backend has changed a dependency
// table since the store was synchronised.  (With accel.shared_state the one
// store lives in the per-database worker, aux_server.hpp, and the same
// counters and snapshot checks decide whether it may answer.)  This module keeps, per database
// and per table slot, two shared counters in a named DSM segment:
//
//   inflight[k]  transactions that have written slot k and not yet finished;
//   seq[k]       committed transactions that wrote slot k.
//
// Writer protocol (capture trigger + transaction callbacks):
//   first write of slot k in a transaction  -> inflight[k]++   (before commit)
//   COMMIT (after ProcArrayEndTransaction)  -> seq[k]++, then inflight[k]--
//   ABORT                                   -> inflight[k]--
//
// Reader protocol (Runtime): a store records the seq values and a mark of the
// MVCC snapshot it was built from.  A statement snapshot S may hit only if
// S covers the build snapshot, S sees every transaction whose writes the
// store maintained locally, and -- read after S was taken -- inflight[k] == 0
// and seq[k] still equals the recorded value for every dependency slot.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

extern "C" {
#include <postgres.h>
#include <access/transam.h>
#include <utils/snapshot.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::aux_sync {

inline constexpr std::size_t kSlots = 256;

// Table id -> counter slot (ids >= kSlots share slot 0: collisions are only
// conservative).
std::size_t SlotOf(core::TableId table);
std::vector<std::size_t> SlotsOf(const std::vector<core::TableId> &tables);

// ---- writer side --------------------------------------------------------
// Called by the capture trigger for every statement on a tracked table.
void RegisterWrite(core::TableId table);
// TRUNCATE (no transition tables): registered like a write; a Runtime whose
// dependency slot was truncated by its own transaction resyncs (rebuilds).
void RegisterTruncate(core::TableId table);
bool OwnTruncatedAnyOf(const std::vector<std::size_t> &slots);
bool OwnRegistered(std::size_t slot);
bool OwnRegisteredAny();
bool OwnRegisteredAnyOf(const std::vector<std::size_t> &slots);

struct CommitInfo {
  // Top-level xid of the committed transaction.  InvalidTransactionId if it
  // never got an xid (changed nothing): then it is released like an abort
  // and `wrote` is all false.
  TransactionId xid = InvalidTransactionId;
  std::array<bool, kSlots> wrote{};
  std::array<uint64_t, kSlots> old_seq{};
  bool maintenance_failed = false;
};

// PRE_PREPARE: prepared transactions are not supported on tracked tables.
void CheckPrepareAllowed();
// PRE_COMMIT: remember the top-level xid.
void NoteTopXid();
// COMMIT: seq++ then inflight-- for every registered slot; returns the
// counter values observed before the increment.  Never throws.
const CommitInfo &PublishCommit();
// ABORT: inflight-- for every registered slot.  Never throws.
void ReleaseAbort();
void ResetTransaction();

// ---- reader side --------------------------------------------------------
// Reads inflight for every slot, then (after a barrier) seq for every slot.
// Returns false if any inflight counter differs from `expected_inflight`
// (0, or 1 for slots registered by the current transaction when
// `allow_own` is true).
bool ReadCounters(const std::vector<std::size_t> &slots, bool allow_own,
                  std::vector<uint64_t> &seq_out);

// Like ReadCounters but only reads seq (no in-flight check): for the shared
// state server applying one commit while holding the commit gate, when other
// in-flight writers are harmless (they cannot become visible meanwhile).
void ReadSeq(const std::vector<std::size_t> &slots,
             std::vector<uint64_t> &seq_out);

// Commit gate (shared state server): a transaction that will ship its
// captured writes to the server holds this transaction-scoped lock from
// PRE_COMMIT to the end of the transaction, so such commits become visible
// and are applied one at a time, in commit order.  Correctness never depends
// on it (the counters catch any interleaving); it only keeps the server in
// sync under concurrent writers.
void AcquireCommitGate();

struct SnapMark {
  bool ok = false;
  TransactionId xmax = InvalidTransactionId;
  std::vector<TransactionId> running; // xip ∪ subxip, sorted
};

// A snapshot is usable iff it is a non-overflowed MVCC snapshot not taken
// during recovery.
bool UsableSnapshot(Snapshot snapshot);
SnapMark MarkOf(Snapshot snapshot);
// Every transaction committed-visible in `mark` is committed-visible in S.
bool Covers(Snapshot snapshot, const SnapMark &mark);
// The (completed) transaction `xid` is visible in S.
bool Sees(Snapshot snapshot, TransactionId xid);

// ---- participants ---------------------------------------------------------
// Runtimes register to observe PRE_COMMIT (after the maintenance snapshot is
// pushed, before StageMaintenance) and COMMIT (after the store maintenance).
class Participant {
public:
  virtual ~Participant() = default;
  virtual void BeforePreCommit() = 0;
  virtual void AfterCommit(const CommitInfo &info) = 0;
};
void RegisterParticipant(Participant *participant);
const std::vector<Participant *> &Participants();

} // namespace accelerator_codegen::aux_state::engine::postgres::aux_sync
