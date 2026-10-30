// GENERATED BACKEND
//
// The shard worker's engine slots (S2-S8, runtime/engine_slots.hpp) for
// Postgres. Everything here runs on the worker's main thread (EngineSlots
// OldestReaderEpoch, called from any thread, reads a cached value).
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <atomic>
#include <deque>
#include <optional>
#include <vector>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/state/shard_server.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"

extern "C" {
#include <postgres.h>
#include <utils/memutils.h>
#include <utils/resowner.h>
#include <utils/snapshot.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {

class PgSlots final : public runtime::EngineSlots {
public:
  PgSlots(int index, std::vector<PostgresAccelerator *> members, CommitArrivals &arrivals);

  // Remembers the writers running now: any of them may have written the
  // accelerator's tables before its capture existed (S5).
  void NotePreRegistrationWriters();

  // Maintenance under a snapshot taken now: module lookups see the database as of that snapshot, whose
  // epoch is returned (nullopt: older than the epoch ring remembers). With per-commit lookups they see it
  // as of the commit being applied instead (MaintenanceLookupEpoch).
  // EndMaintenance(applied) closes the pass (also one a build left open), given the epoch through which
  // the state is applied: older snapshots than the one still needed to look up the commits after it are
  // released.
  std::optional<runtime::Epoch> BeginMaintenance();
  void EndMaintenance(runtime::Epoch applied);
  // Looks up each commit as of its own epoch (a verified store: one version per commit).
  void SetPerCommit(bool on) { per_commit = on; }
  // Re-reads the version floor (S4) from the database's open snapshots.
  void RefreshHorizon();

  // ---- EngineSlots ----
  // Shipped commits only: nothing is in flight in this process.
  runtime::Epoch PendingEpochLowerBound(uint64_t) const override { return 0; }
  runtime::Epoch OldestReaderEpoch() const override { return horizon; }
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &tables,
                                     uint64_t timeout_us) override;
  std::unique_ptr<runtime::BuildSnapshot>
  OpenBuildSnapshot(runtime::AcceleratorRuntime &accelerator) override;
  runtime::Epoch MaintenanceLookupEpoch(runtime::Epoch through) override;
  bool PerCommitLookups() const override { return per_commit; }
  bool EngineWorkOnHostThread() const override { return true; }
  void RunBuild(runtime::AcceleratorRuntime &accelerator, runtime::BuildSnapshot &snapshot,
                core::StateBuilder &builder) override;
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &) const override;
  uint64_t TableRows(core::TableId table) const override;

private:
  friend class PgBuildSnapshot;
  // The maintenance transaction stays open, holding the snapshots (anchors) that keep the versions of
  // the commits not yet applied from being vacuumed.  A pass runs in a subtransaction that is rolled
  // back at its end, which releases the locks the pass's reads took.  Its snapshot is the active one.
  void OpenPass();
  void ClosePass();
  // Makes the base snapshot of the pass the active one.
  void DropDerived();
  // The pass snapshot with the commits after `epoch` hidden, active; false if it cannot be derived.
  bool Derive(runtime::Epoch epoch);

  const int index;
  PostgresAccelerator &Member(const core::AcceleratorId &id) const;
  std::vector<PostgresAccelerator *> members;
  CommitArrivals &arrivals;
  std::vector<TransactionId> pre_registration;
  std::atomic<runtime::Epoch> horizon{kNoFloor};
  struct Anchor {
    Snapshot snapshot; // registered with the transaction
    runtime::Epoch epoch;
  };
  bool per_commit = false;
  bool transaction_open = false;
  bool pass_open = false;
  bool derived = false;
  runtime::Epoch derived_epoch = 0;
  Snapshot pass_snapshot = nullptr;
  std::optional<runtime::Epoch> lookup_epoch; // of the pass snapshot
  std::deque<Anchor> anchors;                  // in epoch order
  MemoryContext pass_context = nullptr;
  ResourceOwner pass_owner = nullptr;
};

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

#endif
