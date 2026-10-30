// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/state/shard_slots.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>

#include "backends/postgres/api/engine_helpers.hpp"
#include "backends/postgres/state/shard_worker.hpp"
#include "backends/postgres/state/shared.hpp"

extern "C" {
#include <access/htup_details.h>
#include <access/xact.h>
#include <catalog/pg_class.h>
#include <miscadmin.h>
#include <storage/procarray.h>
#include <utils/guc.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {
namespace {

// Planner settings for the one-off state build (restored afterwards, also on
// error: the GUC nest level unwinds with the transaction).  The build queries
// join and aggregate whole tables (tens of millions of rows) whose join
// cardinalities PostgreSQL badly underestimates; with the session defaults it
// picks nested loops over millions of outer rows and spilling hash
// aggregates.  Hash joins/aggregates in memory are several times faster, and
// JIT compilation buys nothing for a single scan.  accel.build_tuning = off
// keeps the session settings.
class BuildGucScope {
public:
  explicit BuildGucScope(bool wanted) {
    if (!wanted || !BuildTuning()) {
      return;
    }
    nest = NewGUCNestLevel();
    Set("work_mem", "1GB");
    Set("enable_nestloop", "off");
    Set("enable_mergejoin", "off");
    Set("jit", "off");
  }
  ~BuildGucScope() { Restore(); }
  void Restore() {
    if (nest > 0) {
      AtEOXact_GUC(true, nest);
      nest = 0;
    }
  }

private:
  static void Set(const char *name, const char *value) {
    (void)set_config_option(name, value, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
  }
  int nest = 0;
};

} // namespace

// A build's read: the snapshot's transaction, closed when the build ends (or
// fails).
class PgBuildSnapshot final : public runtime::BuildSnapshot {
public:
  // `shared`: the maintenance pass's snapshot, which the pass closes.
  PgBuildSnapshot(PgSlots &slots_p, runtime::Epoch epoch_p, bool shared_p)
      : slots(slots_p), epoch(epoch_p), shared(shared_p) {}
  ~PgBuildSnapshot() override {
    if (!shared) {
      slots.ClosePass();
    }
  }
  runtime::Epoch SnapshotEpoch() const override { return epoch; }

private:
  PgSlots &slots;
  runtime::Epoch epoch;
  bool shared;
};

PgSlots::PgSlots(int index_p, std::vector<PostgresAccelerator *> members_p, CommitArrivals &arrivals_p)
    : index(index_p), members(std::move(members_p)), arrivals(arrivals_p) {}

PostgresAccelerator &PgSlots::Member(const core::AcceleratorId &id) const {
  for (auto *member : members) {
    if (member->Id() == id) {
      return *member;
    }
  }
  throw std::logic_error("accelerator " + id + " is not a member of the shard");
}

void PgSlots::OpenPass() {
  if (!transaction_open) {
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    XactIsoLevel = XACT_READ_COMMITTED; // each pass takes its own snapshot; the anchors hold the old ones
    transaction_open = true;
  }
  pass_context = CurrentMemoryContext;
  pass_owner = CurrentResourceOwner;
  BeginInternalSubTransaction(nullptr);
  MemoryContextSwitchTo(pass_context);
  pass_snapshot = RegisterSnapshotOnOwner(GetTransactionSnapshot(), TopTransactionResourceOwner);
  PushActiveSnapshot(pass_snapshot);
  pass_open = true;
  lookup_epoch = shared::ReaderEpoch(index, pass_snapshot);
  if (lookup_epoch) {
    anchors.push_back({pass_snapshot, *lookup_epoch});
  } else {
    UnregisterSnapshotFromOwner(pass_snapshot, TopTransactionResourceOwner);
  }
}

void PgSlots::ClosePass() {
  if (!pass_open) {
    return;
  }
  DropDerived();
  PopActiveSnapshot();
  RollbackAndReleaseCurrentSubTransaction();
  MemoryContextSwitchTo(pass_context);
  CurrentResourceOwner = pass_owner;
  pass_open = false;
  pass_snapshot = nullptr;
  lookup_epoch.reset();
}

void PgSlots::DropDerived() {
  if (derived) {
    PopActiveSnapshot();
    derived = false;
  }
}

bool PgSlots::Derive(runtime::Epoch epoch) {
  if (derived && derived_epoch == epoch) {
    return true;
  }
  const auto xids = shared::CommitXids(index, epoch, *lookup_epoch);
  if (!xids) {
    return false;
  }
  // Every commit that wrote the accelerator's tables is in the ring, so hiding those after `epoch`
  // (as in progress: their subtransactions resolve to them through pg_subtrans) leaves exactly the
  // database at `epoch` for those tables.  The anchors keep what it needs from being vacuumed.
  SnapshotData hidden = *pass_snapshot;
  std::vector<TransactionId> in_progress(pass_snapshot->xip, pass_snapshot->xip + pass_snapshot->xcnt);
  for (const TransactionId xid : *xids) {
    in_progress.push_back(xid);
    if (TransactionIdPrecedes(xid, hidden.xmin)) {
      hidden.xmin = xid;
    }
  }
  hidden.xip = in_progress.data();
  hidden.xcnt = static_cast<uint32>(in_progress.size());
  hidden.subxip = nullptr;
  hidden.subxcnt = 0;
  hidden.suboverflowed = true;
  hidden.copied = false;
  hidden.active_count = 0;
  hidden.regd_count = 0;
  DropDerived();
  PushActiveSnapshot(&hidden);
  derived = true;
  derived_epoch = epoch;
  return true;
}

void PgSlots::NotePreRegistrationWriters() {
  OpenPass();
  pre_registration.assign(pass_snapshot->xip, pass_snapshot->xip + pass_snapshot->xcnt);
  ClosePass();
}

std::optional<runtime::Epoch> PgSlots::BeginMaintenance() {
  ClosePass();
  OpenPass();
  return lookup_epoch;
}

void PgSlots::EndMaintenance(runtime::Epoch applied) {
  ClosePass();
  // Keep the newest snapshot taken at or before the applied epoch and all later ones: the commits not
  // applied yet are all after it.
  std::size_t keep = 0;
  for (std::size_t i = 0; i < anchors.size(); i++) {
    if (anchors[i].epoch <= applied) {
      keep = i;
    }
  }
  for (; keep > 0; keep--) {
    UnregisterSnapshotFromOwner(anchors.front().snapshot, TopTransactionResourceOwner);
    anchors.pop_front();
  }
}

void PgSlots::RefreshHorizon() { horizon = shared::HorizonEpoch(index); }

bool PgSlots::WaitForPreRegistrationWriters(const std::vector<core::TableId> &, uint64_t timeout_us) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
  for (const TransactionId xid : pre_registration) {
    while (TransactionIdIsInProgress(xid)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      CHECK_FOR_INTERRUPTS();
      pg_usleep(1000);
    }
  }
  return true;
}

std::unique_ptr<runtime::BuildSnapshot>
PgSlots::OpenBuildSnapshot(runtime::AcceleratorRuntime &runtime) {
  // Inside a maintenance pass every build reads the pass's snapshot: the members that were not stale
  // have applied everything through its epoch, so the builds and the applies are one version at b.
  const bool in_pass = pass_open && lookup_epoch;
  if (!in_pass) {
    ClosePass(); // maintenance a previous build left open
    OpenPass();
  }
  DropDerived();
  if (!lookup_epoch) {
    ClosePass();
    throw std::runtime_error("the build snapshot is older than the epoch ring or the accelerator's activation");
  }
  const auto epoch = *lookup_epoch;
  // The build contains every commit through `epoch`, including those no one
  // shipped (before this worker started): they have arrived, and a commit
  // standing for them keeps the sequence the runtime catches up from dense.
  arrivals.Covered(epoch);
  if (epoch > 0) {
    runtime.OfferCommit(runtime::CommittedChange{epoch, epoch, {}, false, false});
  }
  return std::make_unique<PgBuildSnapshot>(*this, epoch, in_pass);
}

// Postgres applies a commit after it is visible: the lookups of a pass see everything visible to its
// snapshot, unless the runtime applies commit by commit and the commit is one the anchors can still
// look up (the database as of it is derived from the snapshot).
runtime::Epoch PgSlots::MaintenanceLookupEpoch(runtime::Epoch through) {
  if (!pass_open) { // the catch-up at the end of a build
    OpenPass();
  }
  const runtime::Epoch seen = lookup_epoch.value_or(0);
  if (per_commit && through < seen && !anchors.empty() && through >= anchors.front().epoch && Derive(through)) {
    return through;
  }
  DropDerived();
  return seen;
}

void PgSlots::RunBuild(runtime::AcceleratorRuntime &runtime, runtime::BuildSnapshot &,
                       core::StateBuilder &builder) {
  BuildGucScope tuning(Member(runtime.Id()).TuneBuild());
  try {
    runtime.Module().Build(runtime, builder);
  } catch (const std::exception &e) {
    // The runtime retries a failed build; this is the only place its reason is seen.
    ereport(LOG, (errmsg("accelerator %s: build failed: %s", runtime.Id().c_str(), e.what())));
    throw;
  }
}

const maintenance::CostModelParams &PgSlots::CostModel(const core::AcceleratorId &id) const {
  return Member(id).CostModel();
}

// The planner's row estimate for the relation (-1 before its first VACUUM or
// ANALYZE: unknown).
uint64_t PgSlots::TableRows(core::TableId table) const {
  HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(static_cast<Oid>(table)));
  if (!HeapTupleIsValid(tuple)) {
    return 0;
  }
  const float4 rows = reinterpret_cast<Form_pg_class>(GETSTRUCT(tuple))->reltuples;
  ReleaseSysCache(tuple);
  return rows > 0 ? static_cast<uint64_t>(rows) : 0;
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

#endif
