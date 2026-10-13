// GENERATED BACKEND - cross-backend Aux synchronisation (see aux_sync.hpp).
extern "C" {
#include <postgres.h>
#include <access/transam.h>
#include <access/xact.h>
#include <miscadmin.h>
#include <port/atomics.h>
#include <storage/dsm_registry.h>
#include <storage/lock.h>
#include <storage/lmgr.h>
#include <utils/snapshot.h>
}

#include "accelerator_codegen/stateful/postgres/aux_sync.hpp"

#include <algorithm>
#include <string>

namespace accelerator_codegen::aux_state::engine::postgres::aux_sync {
namespace {

// A transaction writing tracked tables publishes its top-level xid and the
// slots it writes here (before it counts as in flight on them), so that a
// reader can tell whether those writes are invisible to its snapshot.
constexpr std::size_t kMaxWriters = 128;
constexpr std::size_t kMaskWords = (kSlots + 63) / 64;
struct WriterEntry {
  pg_atomic_uint32 xid; // 0: free
  pg_atomic_uint64 mask[kMaskWords];
};

struct SharedCounters {
  pg_atomic_uint64 seq[kSlots];
  pg_atomic_uint64 inflight[kSlots];
  // In-flight writers on the slot that are not in `writers`.
  pg_atomic_uint64 untracked[kSlots];
  WriterEntry writers[kMaxWriters];
};

SharedCounters *g_shared = nullptr;
Oid g_shared_db = InvalidOid;

// Per-transaction (process-local) writer state.
std::array<bool, kSlots> g_registered{};
std::array<bool, kSlots> g_truncated{};
bool g_registered_any = false;
std::array<bool, kSlots> g_untracked{}; // registered without a writer entry
int g_writer = -1;                       // this transaction's writer entry
bool g_gate_held = false;
CommitInfo g_commit_info;

std::vector<Participant *> g_participants;

void InitShared(void *ptr) {
  auto *shared = static_cast<SharedCounters *>(ptr);
  for (std::size_t k = 0; k < kSlots; ++k) {
    pg_atomic_init_u64(&shared->seq[k], 0);
    pg_atomic_init_u64(&shared->inflight[k], 0);
    pg_atomic_init_u64(&shared->untracked[k], 0);
  }
  for (auto &w : shared->writers) {
    pg_atomic_init_u32(&w.xid, 0);
    for (auto &m : w.mask)
      pg_atomic_init_u64(&m, 0);
  }
}

SharedCounters *Shared() {
  if (g_shared && g_shared_db == MyDatabaseId) {
    return g_shared;
  }
  // One segment per database: writes in other databases never block hits.
  const std::string name =
      "accel_aux_sync_v2_" + std::to_string(static_cast<unsigned>(MyDatabaseId));
  bool found = false;
  g_shared = static_cast<SharedCounters *>(
      GetNamedDSMSegment(name.c_str(), sizeof(SharedCounters), InitShared,
                         &found));
  g_shared_db = MyDatabaseId;
  return g_shared;
}

bool InSortedRunning(const std::vector<TransactionId> &running,
                     TransactionId xid) {
  return std::binary_search(running.begin(), running.end(), xid);
}

bool InSnapshot(Snapshot snapshot, TransactionId xid) {
  for (uint32 i = 0; i < snapshot->xcnt; ++i) {
    if (TransactionIdEquals(snapshot->xip[i], xid)) {
      return true;
    }
  }
  for (int32 i = 0; i < snapshot->subxcnt; ++i) {
    if (TransactionIdEquals(snapshot->subxip[i], xid)) {
      return true;
    }
  }
  return false;
}

} // namespace

std::size_t SlotOf(core::TableId table) {
  return table < kSlots ? static_cast<std::size_t>(table) : 0;
}

std::vector<std::size_t> SlotsOf(const std::vector<core::TableId> &tables) {
  std::vector<std::size_t> slots;
  for (auto table : tables) {
    slots.push_back(SlotOf(table));
  }
  std::sort(slots.begin(), slots.end());
  slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
  return slots;
}

void RegisterWrite(core::TableId table) {
  const auto slot = SlotOf(table);
  if (g_registered[slot]) {
    return;
  }
  SharedCounters *shared = Shared();
  // Mark locally first: if the increment below is reached the abort/commit
  // callback releases it; the increment itself cannot fail.
  g_registered[slot] = true;
  g_registered_any = true;
  // Publish who writes before counting as in flight (atomics are full
  // barriers): a reader that sees the in-flight count also sees the entry.
  const TransactionId xid = GetTopTransactionIdIfAny();
  bool tracked = false;
  if (TransactionIdIsNormal(xid)) {
    for (std::size_t i = 0; g_writer < 0 && i < kMaxWriters; ++i) {
      uint32 expected = 0;
      if (pg_atomic_compare_exchange_u32(&shared->writers[i].xid, &expected, xid))
        g_writer = static_cast<int>(i);
    }
    if (g_writer >= 0) {
      pg_atomic_fetch_or_u64(&shared->writers[g_writer].mask[slot / 64],
                             uint64{1} << (slot % 64));
      tracked = true;
    }
  }
  if (!tracked) {
    g_untracked[slot] = true;
    pg_atomic_fetch_add_u64(&shared->untracked[slot], 1);
  }
  pg_atomic_fetch_add_u64(&shared->inflight[slot], 1);
}

void RegisterTruncate(core::TableId table) {
  RegisterWrite(table);
  g_truncated[SlotOf(table)] = true;
}

bool OwnTruncatedAnyOf(const std::vector<std::size_t> &slots) {
  for (auto slot : slots) {
    if (g_truncated[slot]) {
      return true;
    }
  }
  return false;
}

bool OwnRegistered(std::size_t slot) { return g_registered[slot]; }

bool OwnRegisteredAny() { return g_registered_any; }

bool OwnRegisteredAnyOf(const std::vector<std::size_t> &slots) {
  for (auto slot : slots) {
    if (g_registered[slot]) {
      return true;
    }
  }
  return false;
}

void CheckPrepareAllowed() {
  if (g_registered_any) {
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot PREPARE a transaction that modified "
                    "accelerator-tracked tables")));
  }
}

void NoteTopXid() {
  g_commit_info.xid = GetTopTransactionIdIfAny();
}

// After the in-flight counts are released: forget the untracked marks and the
// writer entry (in that order, so a reader never sees fewer known writers
// than in-flight ones without falling back to "unknown").
void ReleaseWriter(SharedCounters *shared) {
  for (std::size_t k = 0; k < kSlots; ++k) {
    if (g_untracked[k]) {
      pg_atomic_fetch_sub_u64(&shared->untracked[k], 1);
    }
  }
  if (g_writer >= 0) {
    auto &w = shared->writers[g_writer];
    for (auto &m : w.mask)
      pg_atomic_write_u64(&m, 0);
    pg_memory_barrier();
    pg_atomic_write_u32(&w.xid, 0);
  }
  g_untracked.fill(false);
  g_writer = -1;
}

const CommitInfo &PublishCommit() {
  const TransactionId xid = g_commit_info.xid;
  g_commit_info = CommitInfo{};
  g_commit_info.xid = xid;
  if (!g_registered_any) {
    return g_commit_info;
  }
  SharedCounters *shared = g_shared; // mapped by RegisterWrite
  if (!TransactionIdIsValid(xid)) {
    // Registered only by statements that changed no rows (no xid was ever
    // assigned): the transaction is invisible, exactly like an abort.
    for (std::size_t k = 0; k < kSlots; ++k) {
      if (g_registered[k]) {
        pg_atomic_fetch_sub_u64(&shared->inflight[k], 1);
      }
    }
    ReleaseWriter(shared);
    return g_commit_info;
  }
  // The commit is already visible (XACT_EVENT_COMMIT runs after
  // ProcArrayEndTransaction).  Count it before leaving the in-flight set.
  for (std::size_t k = 0; k < kSlots; ++k) {
    if (!g_registered[k]) {
      continue;
    }
    g_commit_info.wrote[k] = true;
    g_commit_info.old_seq[k] = pg_atomic_fetch_add_u64(&shared->seq[k], 1);
  }
  for (std::size_t k = 0; k < kSlots; ++k) {
    if (g_registered[k]) {
      pg_atomic_fetch_sub_u64(&shared->inflight[k], 1);
    }
  }
  ReleaseWriter(shared);
  return g_commit_info;
}

void ReleaseAbort() {
  if (g_registered_any) {
    SharedCounters *shared = g_shared;
    for (std::size_t k = 0; k < kSlots; ++k) {
      if (g_registered[k]) {
        pg_atomic_fetch_sub_u64(&shared->inflight[k], 1);
      }
    }
    ReleaseWriter(shared);
  }
  ResetTransaction();
}

void ResetTransaction() {
  if (g_writer >= 0 || std::find(g_untracked.begin(), g_untracked.end(), true) != g_untracked.end()) {
    ReleaseWriter(Shared());
  }
  g_registered.fill(false);
  g_truncated.fill(false);
  g_registered_any = false;
  g_gate_held = false;
  g_commit_info = CommitInfo{};
}

bool ReadCounters(const std::vector<std::size_t> &slots, bool allow_own,
                  std::vector<uint64_t> &seq_out) {
  SharedCounters *shared = Shared();
  pg_memory_barrier();
  for (auto slot : slots) {
    const uint64_t expected = (allow_own && g_registered[slot]) ? 1 : 0;
    if (pg_atomic_read_u64(&shared->inflight[slot]) != expected) {
      return false;
    }
  }
  pg_memory_barrier();
  seq_out.clear();
  for (auto slot : slots) {
    seq_out.push_back(pg_atomic_read_u64(&shared->seq[slot]));
  }
  return true;
}

bool ReadCountersFor(Snapshot snapshot, const std::vector<std::size_t> &slots,
                     std::vector<uint64_t> &seq_out) {
  SharedCounters *shared = Shared();
  pg_memory_barrier();
  bool any_inflight = false;
  std::vector<uint64_t> inflight;
  for (auto slot : slots) {
    inflight.push_back(pg_atomic_read_u64(&shared->inflight[slot]));
    if (inflight.back() == 0)
      continue;
    any_inflight = true;
    if (pg_atomic_read_u64(&shared->untracked[slot]) != 0)
      return false;
  }
  if (any_inflight) {
    if (!UsableSnapshot(snapshot))
      return false;
    pg_memory_barrier();
    // Every in-flight writer of every slot must be a known one invisible to S.
    std::vector<uint64_t> known(slots.size(), 0);
    for (auto &w : shared->writers) {
      const TransactionId xid = pg_atomic_read_u32(&w.xid);
      if (xid == 0)
        continue;
      bool writes_slot = false;
      for (std::size_t i = 0; i < slots.size(); ++i) {
        if (inflight[i] != 0 &&
            (pg_atomic_read_u64(&w.mask[slots[i] / 64]) >> (slots[i] % 64)) & 1) {
          ++known[i];
          writes_slot = true;
        }
      }
      if (writes_slot && !InSnapshot(snapshot, xid) &&
          !TransactionIdFollowsOrEquals(xid, snapshot->xmax))
        return false; // committed and visible in S, not counted yet
    }
    for (std::size_t i = 0; i < slots.size(); ++i)
      if (known[i] < inflight[i])
        return false; // a writer we cannot see (it is just leaving or arriving)
  }
  pg_memory_barrier();
  seq_out.clear();
  for (auto slot : slots) {
    seq_out.push_back(pg_atomic_read_u64(&shared->seq[slot]));
  }
  return true;
}

void ReadSeq(const std::vector<std::size_t> &slots,
             std::vector<uint64_t> &seq_out) {
  SharedCounters *shared = Shared();
  pg_memory_barrier();
  seq_out.clear();
  for (auto slot : slots) {
    seq_out.push_back(pg_atomic_read_u64(&shared->seq[slot]));
  }
}

void AcquireCommitGate() {
  if (g_gate_held) {
    return; // held for the rest of this transaction
  }
  LOCKTAG tag;
  // An advisory-class lock in a field-4 namespace no pg_advisory_* call uses.
  SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x41435841u, 0u, 3);
  (void)LockAcquire(&tag, ExclusiveLock, false, false);
  g_gate_held = true;
}

bool UsableSnapshot(Snapshot snapshot) {
  return snapshot != nullptr && snapshot->snapshot_type == SNAPSHOT_MVCC &&
         !snapshot->suboverflowed && !snapshot->takenDuringRecovery &&
         TransactionIdIsNormal(snapshot->xmax);
}

SnapMark MarkOf(Snapshot snapshot) {
  SnapMark mark;
  if (!UsableSnapshot(snapshot)) {
    return mark;
  }
  mark.ok = true;
  mark.xmax = snapshot->xmax;
  for (uint32 i = 0; i < snapshot->xcnt; ++i) {
    mark.running.push_back(snapshot->xip[i]);
  }
  for (int32 i = 0; i < snapshot->subxcnt; ++i) {
    mark.running.push_back(snapshot->subxip[i]);
  }
  std::sort(mark.running.begin(), mark.running.end());
  return mark;
}

bool Covers(Snapshot snapshot, const SnapMark &mark) {
  if (!mark.ok || !UsableSnapshot(snapshot)) {
    return false;
  }
  if (TransactionIdPrecedes(snapshot->xmax, mark.xmax)) {
    return false;
  }
  // x visible in mark  <=>  x < mark.xmax and x not running in mark.
  // S must not report such an x as running.
  for (uint32 i = 0; i < snapshot->xcnt; ++i) {
    const TransactionId y = snapshot->xip[i];
    if (TransactionIdPrecedes(y, mark.xmax) &&
        !InSortedRunning(mark.running, y)) {
      return false;
    }
  }
  for (int32 i = 0; i < snapshot->subxcnt; ++i) {
    const TransactionId y = snapshot->subxip[i];
    if (TransactionIdPrecedes(y, mark.xmax) &&
        !InSortedRunning(mark.running, y)) {
      return false;
    }
  }
  return true;
}

bool Sees(Snapshot snapshot, TransactionId xid) {
  if (!UsableSnapshot(snapshot)) {
    return false;
  }
  if (!TransactionIdIsValid(xid)) {
    return false;
  }
  return TransactionIdPrecedes(xid, snapshot->xmax) &&
         !InSnapshot(snapshot, xid);
}

void RegisterParticipant(Participant *participant) {
  if (std::find(g_participants.begin(), g_participants.end(), participant) ==
      g_participants.end()) {
    g_participants.push_back(participant);
  }
}

const std::vector<Participant *> &Participants() { return g_participants; }

} // namespace accelerator_codegen::aux_state::engine::postgres::aux_sync
