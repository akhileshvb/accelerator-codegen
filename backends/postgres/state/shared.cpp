// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/state/shared.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "backends/postgres/state/shard_worker.hpp"

extern "C" {
#include <access/transam.h>
#include <miscadmin.h>
#include <port/atomics.h>
#include <storage/dsm_registry.h>
#include <storage/lock.h>
#include <storage/procarray.h>
#include <storage/spin.h>
#include <utils/guc.h>
#include <utils/snapmgr.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shared {
namespace {

// Commits of one accelerator that readers can still place a snapshot among.
// A snapshot older than this many commits answers from SQL.
constexpr int kRingSize = 1024;
constexpr std::size_t kIdBytes = 96;

// The epoch's slot is ring[epoch % kRingSize]. `epoch` doubles as the
// seqlock: the (single, gate-holding) writer zeroes it, writes the xid and
// then stores the epoch, so a reader that sees the epoch it asked for before
// and after reading the xid has a whole pair.
struct Entry {
  pg_atomic_uint64 epoch;
  pg_atomic_uint32 xid;
};

struct Slot {
  // Highest epoch assigned; its entry is published before this is raised.
  pg_atomic_uint64 assigned;
  Entry ring[kRingSize];
};

struct Registry {
  slock_t activation; // serializes appends to ids
  pg_atomic_uint32 count;
  pg_atomic_uint64 generation;
  pg_atomic_uint32 tokens_in_use;
  // The next xid when ids[i] was added (invalid until set): every transaction
  // that wrote without an epoch for i began before it.
  pg_atomic_uint32 activated_xid[kMaxAccelerators];
  char ids[kMaxAccelerators][kIdBytes];
  Slot slots[kMaxAccelerators];
};

void InitRegistry(void *ptr) {
  auto *registry = static_cast<Registry *>(ptr);
  std::memset(registry, 0, sizeof(Registry));
  SpinLockInit(&registry->activation);
  pg_atomic_init_u32(&registry->count, 0);
  pg_atomic_init_u64(&registry->generation, 0);
  pg_atomic_init_u32(&registry->tokens_in_use, 0);
  for (auto &xid : registry->activated_xid) {
    pg_atomic_init_u32(&xid, InvalidTransactionId);
  }
  for (auto &slot : registry->slots) {
    pg_atomic_init_u64(&slot.assigned, 0);
    for (auto &entry : slot.ring) {
      pg_atomic_init_u64(&entry.epoch, 0);
      pg_atomic_init_u32(&entry.xid, InvalidTransactionId);
    }
  }
}

// One registry per database: its accelerators, epochs and thread tokens.
Registry &Reg() {
  static Registry *registry = nullptr;
  static Oid database = InvalidOid;
  if (registry == nullptr || database != MyDatabaseId) {
    const std::string name = "accel_pg_registry_" + std::to_string(static_cast<unsigned>(MyDatabaseId));
    bool found = false;
    registry = static_cast<Registry *>(GetNamedDSMSegment(name.c_str(), sizeof(Registry), InitRegistry, &found));
    database = MyDatabaseId;
  }
  return *registry;
}

// ---- gates ----------------------------------------------------------------------
// Session-level advisory-class locks (field 4 = 3: no pg_advisory_* call uses
// it): released explicitly at COMMIT, which runs after the commit is visible,
// and at ABORT. An aborting transaction releases before it is out of the
// ProcArray, which is harmless: the reader's k only counts committed epochs.
constexpr uint32 kGateClass = 0x41434745; // "ACGE"

std::vector<int> g_held_gates;

LOCKTAG GateTag(int index) {
  LOCKTAG tag;
  SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, kGateClass, static_cast<uint32>(index), 3);
  return tag;
}

// ---- epoch ring -----------------------------------------------------------------
void Publish(Slot &slot, Epoch epoch, TransactionId xid) {
  Entry &entry = slot.ring[epoch % kRingSize];
  pg_atomic_write_u64(&entry.epoch, 0);
  pg_write_barrier();
  pg_atomic_write_u32(&entry.xid, xid);
  pg_write_barrier();
  pg_atomic_write_u64(&entry.epoch, epoch);
  pg_write_barrier();
  pg_atomic_write_u64(&slot.assigned, epoch);
}

// The xid of `epoch`, unless its slot has been reused for a newer epoch.
bool ReadEntry(Slot &slot, Epoch epoch, TransactionId &xid) {
  Entry &entry = slot.ring[epoch % kRingSize];
  if (pg_atomic_read_u64(&entry.epoch) != epoch) {
    return false;
  }
  pg_read_barrier();
  xid = pg_atomic_read_u32(&entry.xid);
  pg_read_barrier();
  return pg_atomic_read_u64(&entry.epoch) == epoch;
}

// Scans the ring newest to oldest for the first epoch whose xid `accept`s and
// that committed (an epoch that aborted has no effect and never counts).
// 0 if there is none; nullopt if the ring no longer reaches back that far.
template <typename Accept> std::optional<Epoch> NewestCommitted(int index, Accept accept) {
  Slot &slot = Reg().slots[index];
  const Epoch hi = pg_atomic_read_u64(&slot.assigned);
  pg_read_barrier();
  const Epoch lo = hi >= kRingSize ? hi - kRingSize + 1 : 1;
  for (Epoch epoch = hi; epoch >= lo && epoch > 0; epoch--) {
    TransactionId xid;
    if (!ReadEntry(slot, epoch, xid)) {
      return std::nullopt;
    }
    if (accept(xid) && TransactionIdDidCommit(xid)) {
      return epoch;
    }
  }
  return lo > 1 ? std::nullopt : std::optional<Epoch>(0);
}

// Whether `snapshot` sees every transaction that began before `index` was
// activated. Those took no epoch, yet the build contains what they committed:
// a snapshot that does not see them must not be served from it.
bool SeesPreActivationWrites(int index, Snapshot snapshot) {
  const TransactionId activated = pg_atomic_read_u32(&Reg().activated_xid[index]);
  if (activated == InvalidTransactionId || TransactionIdPrecedes(snapshot->xmax, activated)) {
    return false;
  }
  for (uint32 i = 0; i < snapshot->xcnt; i++) {
    if (TransactionIdPrecedes(snapshot->xip[i], activated)) {
      return false;
    }
  }
  return true;
}

// ---- thread tokens --------------------------------------------------------------
int g_threads = 1;
int g_ship_timeout_ms = 2000;

class SharedTokens final : public runtime::TokenSource {
public:
  explicit SharedTokens(pg_atomic_uint32 &in_use) : in_use_(in_use) {}

  std::size_t TryAcquire(std::size_t wanted) override {
    const auto budget = static_cast<uint32>(ThreadBudget());
    uint32 used = pg_atomic_read_u32(&in_use_);
    for (;;) {
      const uint32 take = std::min<uint32>(static_cast<uint32>(wanted), budget > used ? budget - used : 0);
      if (take == 0) {
        return 0;
      }
      if (pg_atomic_compare_exchange_u32(&in_use_, &used, used + take)) {
        return take;
      }
    }
  }

  void Release(std::size_t count) override { pg_atomic_fetch_sub_u32(&in_use_, static_cast<uint32>(count)); }

private:
  pg_atomic_uint32 &in_use_;
};

} // namespace

// ---- registry -------------------------------------------------------------------
int Find(const core::AcceleratorId &id) {
  Registry &registry = Reg();
  const uint32 count = pg_atomic_read_u32(&registry.count);
  pg_read_barrier();
  for (uint32 i = 0; i < count; i++) {
    if (id == registry.ids[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int Activate(const core::AcceleratorId &id) {
  int index = Find(id);
  if (index < 0) {
    if (id.size() >= kIdBytes) {
      return -1;
    }
    Registry &registry = Reg();
    SpinLockAcquire(&registry.activation);
    index = Find(id); // another backend may have just added it
    const uint32 count = pg_atomic_read_u32(&registry.count);
    if (index < 0 && count < kMaxAccelerators) {
      std::memcpy(registry.ids[count], id.c_str(), id.size() + 1);
      pg_write_barrier();
      pg_atomic_write_u32(&registry.count, count + 1);
      pg_atomic_fetch_add_u64(&registry.generation, 1);
      pg_memory_barrier(); // writers see the accelerator before the xid is read
      pg_atomic_write_u32(&registry.activated_xid[count], ReadNextTransactionId());
      index = static_cast<int>(count);
    }
    SpinLockRelease(&registry.activation);
    if (index < 0) {
      return -1;
    }
  }
  shard::EnsureWorker(index);
  return index;
}

core::AcceleratorId IdOf(int index) {
  Registry &registry = Reg();
  return index >= 0 && static_cast<uint32>(index) < pg_atomic_read_u32(&registry.count) ? registry.ids[index]
                                                                                       : "";
}

std::vector<int> Active() {
  std::vector<int> indices(pg_atomic_read_u32(&Reg().count));
  for (std::size_t i = 0; i < indices.size(); i++) {
    indices[i] = static_cast<int>(i);
  }
  return indices;
}

uint64_t Generation() { return pg_atomic_read_u64(&Reg().generation); }

// ---- commit epochs --------------------------------------------------------------
std::vector<Epoch> TakeCommitEpochs(const std::vector<int> &indices, TransactionId xid) {
  Registry &registry = Reg();
  g_held_gates.reserve(indices.size());
  for (const int index : indices) {
    const LOCKTAG tag = GateTag(index);
    (void)LockAcquire(&tag, ExclusiveLock, true, false); // waits; cancellable
    g_held_gates.push_back(index);
  }
  std::vector<Epoch> epochs;
  epochs.reserve(indices.size());
  for (const int index : indices) {
    Slot &slot = registry.slots[index];
    // The gate makes this the only writer of the slot.
    epochs.push_back(pg_atomic_read_u64(&slot.assigned) + 1);
    Publish(slot, epochs.back(), xid);
  }
  return epochs;
}

void ReleaseGates() {
  for (auto it = g_held_gates.rbegin(); it != g_held_gates.rend(); ++it) {
    const LOCKTAG tag = GateTag(*it);
    LockRelease(&tag, ExclusiveLock, true);
  }
  g_held_gates.clear();
}

Epoch AssignedEpoch(int index) { return pg_atomic_read_u64(&Reg().slots[index].assigned); }

// ---- reader epoch and version floor ---------------------------------------------
std::optional<Epoch> ReaderEpoch(int index, Snapshot snapshot) {
  if (snapshot == nullptr || snapshot->snapshot_type != SNAPSHOT_MVCC || !SeesPreActivationWrites(index, snapshot)) {
    return std::nullopt;
  }
  // Epochs are published before their commit is visible, and the snapshot
  // was taken before the ring is read here: every commit it sees is in the
  // ring. Gates make the committed epochs it sees a prefix, so the newest one
  // is k.
  return NewestCommitted(index, [&](TransactionId xid) { return !XidInMVCCSnapshot(xid, snapshot); });
}

Epoch HorizonEpoch(int index) {
  // Every open snapshot (and every later one) sees a commit whose xid
  // precedes the oldest xmin, hence its whole prefix.
  const TransactionId oldest = GetOldestNonRemovableTransactionId(nullptr);
  const auto horizon = NewestCommitted(index, [&](TransactionId xid) { return TransactionIdPrecedes(xid, oldest); });
  if (horizon) {
    return *horizon;
  }
  // Older than the ring: no reader can be served below its oldest entry.
  Slot &slot = Reg().slots[index];
  const Epoch hi = pg_atomic_read_u64(&slot.assigned);
  return hi >= kRingSize ? hi - kRingSize + 1 : 0;
}

// ---- thread budget --------------------------------------------------------------
std::size_t ThreadBudget() { return runtime::ResolveThreadBudget(static_cast<std::size_t>(g_threads)); }

std::shared_ptr<runtime::TokenSource> ThreadTokens() {
  static std::shared_ptr<runtime::TokenSource> tokens;
  static Oid database = InvalidOid;
  if (!tokens || database != MyDatabaseId) {
    tokens = std::make_shared<SharedTokens>(Reg().tokens_in_use);
    database = MyDatabaseId;
  }
  return tokens;
}

std::chrono::milliseconds CommitShipTimeout() { return std::chrono::milliseconds(g_ship_timeout_ms); }

void DefineGucs() {
  g_threads = static_cast<int>(runtime::AffinityCpuCount());
  DefineCustomIntVariable("accel.threads",
                          "Threads the accelerators of a database may use in total (default: the CPUs this "
                          "process may run on).",
                          nullptr, &g_threads, g_threads, 1, 4096, PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
  DefineCustomIntVariable("accel.commit_ship_timeout_ms",
                          "How long a committing writer waits to queue its changes for a shard worker before it "
                          "gives up (the worker then rebuilds).",
                          nullptr, &g_ship_timeout_ms, g_ship_timeout_ms, 1, 600000, PGC_USERSET, 0, nullptr,
                          nullptr, nullptr);
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shared

#endif
