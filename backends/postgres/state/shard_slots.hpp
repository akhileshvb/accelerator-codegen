// GENERATED BACKEND
//
// The shard worker's engine slots (S2-S8, runtime/engine_slots.hpp) for
// Postgres. Everything here runs on the worker's main thread (EngineSlots
// OldestReaderEpoch, called from any thread, reads a cached value).
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <atomic>
#include <optional>
#include <vector>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/state/shard_server.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"

extern "C" {
#include <postgres.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {

class PgSlots final : public runtime::EngineSlots {
public:
  PgSlots(int index, PostgresAccelerator &accelerator, CommitArrivals &arrivals);

  // Remembers the writers running now: any of them may have written the
  // accelerator's tables before its capture existed (S5).
  void NotePreRegistrationWriters();

  // Maintenance under a snapshot taken now: module lookups see the database
  // as of that snapshot, whose epoch is returned (nullopt: older than the
  // epoch ring remembers). End closes it (also one a build left open).
  std::optional<runtime::Epoch> BeginMaintenance();
  void EndMaintenance();
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
  bool EngineWorkOnHostThread() const override { return true; }
  void RunBuild(runtime::AcceleratorRuntime &accelerator, runtime::BuildSnapshot &snapshot,
                core::StateBuilder &builder) override;
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &) const override;

private:
  friend class PgBuildSnapshot;
  // A transaction with a snapshot that stays the same throughout (repeatable
  // read), pushed as the active one: SPI reads see exactly it.
  void Begin();
  void End(bool failed);

  const int index;
  PostgresAccelerator &accelerator;
  CommitArrivals &arrivals;
  std::vector<TransactionId> pre_registration;
  std::atomic<runtime::Epoch> horizon{kNoFloor};
  bool open = false;
  std::optional<runtime::Epoch> lookup_epoch; // of the open maintenance snapshot
};

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

#endif
