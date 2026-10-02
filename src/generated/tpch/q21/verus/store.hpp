#pragma once

// TPC-H Q21 (Verus) on the verified aux store: the C++ face
// (core::AuxStateStore) of the Verus store in src/verus/substrate, holding
// the Q21 kernel's states.  TRUSTED GLUE: it maps the coordinator's commit
// protocol and the engine's epochs onto the store, and does nothing else.
//
//   BeginCommit   q21vs_begin at the commit epoch (readers whose snapshot
//                 may include the commit wait)
//   FinishCommit  installs the module's prepared version (or an invalid one
//                 for an invalidation) at the commit epoch; ends the commit
//   AbortCommit   q21vs_abort
//   Install       a build (the module's prepared version) from an epoch
//   Read*         the version the reader's snapshot epoch selects
//
// The module prepares versions itself (TpchQ21VerusModule, in this store's
// mode): its updates carry TpchQ21VerusPrepared payloads, never states.

#include <atomic>
#include <cstdint>

#include "accelerator_codegen/stateful/state/verus_aux_store.hpp"
#include "accelerator_codegen/verus/q21_verus.h"
#include "tpch/q21/verus/module.hpp"

namespace accelerator_codegen::aux_state::query {

// A prepared next version, as a StateImage payload from the module to the
// store (consumed by the install).
struct TpchQ21VerusPrepared {
  explicit TpchQ21VerusPrepared(q21vs_prepared *p) : prepared(p) {}
  ~TpchQ21VerusPrepared() { q21vs_prepared_free(prepared); }
  TpchQ21VerusPrepared(const TpchQ21VerusPrepared &) = delete;
  TpchQ21VerusPrepared &operator=(const TpchQ21VerusPrepared &) = delete;
  // The prepared version, now owned by the caller (null if already taken).
  q21vs_prepared *Take() const {
    auto *p = prepared;
    prepared = nullptr;
    return p;
  }
  mutable q21vs_prepared *prepared = nullptr;
};

class TpchQ21VerusStore final : public VerusAuxStore {
public:
  TpchQ21VerusStore(EpochProvider epoch, FloorProvider floor);
  ~TpchQ21VerusStore() override;
  TpchQ21VerusStore(const TpchQ21VerusStore &) = delete;
  TpchQ21VerusStore &operator=(const TpchQ21VerusStore &) = delete;

  const q21vs_store *Handle() const { return store; }

  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::StateSnapshot ReadPartitions(const core::VisibilityToken &,
                                     const std::vector<core::PartitionId> &) const override;
  core::StateSnapshot ReadHitPartitions(const core::VisibilityToken &, const core::QueryKey &,
                                        const std::vector<core::PartitionId> &) const override;
  core::ValidityResult CheckValidity(const core::QueryKey &,
                                     const core::VisibilityToken &) const override;

  void Invalidate(const core::InvalidationPlan &) override;
  bool TryPublish(const core::StagedMaintenanceOutcome &, core::Version) override;

  void BeginCommit(const core::AcceleratorId &, const core::InvalidationPlan &) override;
  void FinishCommit(const core::AcceleratorId &, const core::InvalidationPlan &invalidate,
                    const std::vector<core::StateImage> &updates,
                    core::Version expected_version = 0) override;
  void AbortCommit(const core::AcceleratorId &) override;

  void Install(std::vector<core::StateImage> images, const core::AcceleratorId &,
               Epoch from_epoch) override;
  std::size_t ValidPartitionCount() const override;
  std::size_t TotalPartitionCount() const override;
  std::size_t TotalVersionCount() const override;
  std::size_t StaleInstalls() const override { return stale_installs.load(); }

private:
  enum class Status { State, Invalid, Pending, NotVisible };
  // The state the snapshot at `epoch` sees (waits for a pending commit as
  // ACCEL_AUX_PENDING_WAIT_US says: unbounded by default).
  std::shared_ptr<const TpchQ21VerusImage> Resolve(core::Version epoch, Status &status) const;
  // Install the version prepared in `updates` (or an invalid one if they
  // hold none and `invalidate` is set); `end_commit`: end the commit too.
  void InstallUpdates(const core::InvalidationPlan &invalidate,
                      const std::vector<core::StateImage> &updates, Epoch from, bool end_commit);
  Epoch CurrentEpoch() const;
  Epoch CurrentFloor() const;

  EpochProvider epoch;
  FloorProvider floor;
  uint64_t pending_wait_micros;
  q21vs_store *store;
  std::atomic<std::size_t> stale_installs{0};
};

} // namespace accelerator_codegen::aux_state::query
