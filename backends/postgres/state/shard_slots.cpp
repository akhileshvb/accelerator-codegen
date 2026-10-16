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
#include <access/xact.h>
#include <miscadmin.h>
#include <storage/procarray.h>
#include <utils/guc.h>
#include <utils/snapmgr.h>
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
  PgBuildSnapshot(PgSlots &slots_p, runtime::Epoch epoch_p) : slots(slots_p), epoch(epoch_p) {}
  ~PgBuildSnapshot() override { slots.End(std::uncaught_exceptions() > 0); }
  runtime::Epoch SnapshotEpoch() const override { return epoch; }

private:
  PgSlots &slots;
  runtime::Epoch epoch;
};

PgSlots::PgSlots(int index_p, PostgresAccelerator &accelerator_p, CommitArrivals &arrivals_p)
    : index(index_p), accelerator(accelerator_p), arrivals(arrivals_p) {}

void PgSlots::Begin() {
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  XactIsoLevel = XACT_REPEATABLE_READ;
  PushActiveSnapshot(GetTransactionSnapshot());
  open = true;
}

void PgSlots::End(bool failed) {
  if (!open) {
    return;
  }
  open = false;
  lookup_epoch.reset();
  if (failed) {
    AbortCurrentTransaction();
    return;
  }
  PopActiveSnapshot();
  CommitTransactionCommand();
}

void PgSlots::NotePreRegistrationWriters() {
  Begin();
  const Snapshot snapshot = GetActiveSnapshot();
  pre_registration.assign(snapshot->xip, snapshot->xip + snapshot->xcnt);
  End(false);
}

std::optional<runtime::Epoch> PgSlots::BeginMaintenance() {
  End(false);
  Begin();
  lookup_epoch = shared::ReaderEpoch(index, GetActiveSnapshot());
  return lookup_epoch;
}

void PgSlots::EndMaintenance() { End(false); }

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
  End(false); // maintenance a previous build left open
  Begin();
  const auto epoch = shared::ReaderEpoch(index, GetActiveSnapshot());
  if (!epoch) {
    End(true);
    throw std::runtime_error("the build snapshot is older than the epoch ring or the accelerator's activation");
  }
  // The build contains every commit through `epoch`, including those no one
  // shipped (before this worker started): they have arrived, and a commit
  // standing for them keeps the sequence the runtime catches up from dense.
  arrivals.Covered(*epoch);
  if (*epoch > 0) {
    runtime.OfferCommit(runtime::CommittedChange{*epoch, *epoch, {}, false, false});
  }
  return std::make_unique<PgBuildSnapshot>(*this, *epoch);
}

// Postgres applies a commit after it is visible, so the lookups of a
// maintenance pass see everything visible to the snapshot it runs under.
runtime::Epoch PgSlots::MaintenanceLookupEpoch(runtime::Epoch) {
  if (!open) { // the catch-up at the end of a build
    BeginMaintenance();
  }
  return lookup_epoch.value_or(0);
}

void PgSlots::RunBuild(runtime::AcceleratorRuntime &runtime, runtime::BuildSnapshot &,
                       core::StateBuilder &builder) {
  BuildGucScope tuning(accelerator.TuneBuild());
  // The build's scans decode in parallel: this thread plus the budget's other
  // threads as Postgres parallel workers.
  SetBuildScanParticipants(static_cast<int>(shared::ThreadBudget()));
  try {
    runtime.Module().Build(runtime, builder);
  } catch (const std::exception &e) {
    // The runtime retries a failed build; this is the only place its reason is seen.
    ereport(LOG, (errmsg("accelerator %s: build failed: %s", runtime.Id().c_str(), e.what())));
    throw;
  }
}

const maintenance::CostModelParams &PgSlots::CostModel(const core::AcceleratorId &) const {
  return accelerator.CostModel();
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

#endif
