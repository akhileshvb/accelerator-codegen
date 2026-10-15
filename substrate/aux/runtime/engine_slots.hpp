// SCAFFOLDING
// C2: what an engine backend supplies to the runtime skeleton (slots S1-S8).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/maintenance/cost_model.hpp"

namespace accelerator_codegen::aux_state::runtime {

using Epoch = core::Version;
using Seq = uint64_t;

class AcceleratorRuntime;

// S5: a consistent read the build scans through. Valid until destroyed.
class BuildSnapshot {
public:
  virtual ~BuildSnapshot() = default;
  // Contract: a commit is in this snapshot iff its epoch <= Epoch().
  virtual Epoch SnapshotEpoch() const = 0;
};

// The slots. One instance per runtime host. Push-style slots (S1 capture,
// S3 reader epochs, S7 entry points) are backend code calling into
// RuntimeHost / AcceleratorRuntime and need no virtual here.
class EngineSlots {
public:
  virtual ~EngineSlots() = default;

  // ---- S2 commit epoch ----
  // Epoch from which an in-process commit that is about to happen may be
  // visible (the store's pending mark): no reader whose epoch is below it can
  // see the commit. DuckDB: the writer's start time + 1.
  virtual Epoch PendingEpochLowerBound(uint64_t txn) const = 0;

  // ---- S4 version floor ----
  // The lowest reader epoch any open reader may still use; versions below it
  // that a newer version supersedes can be dropped. kNoFloor: keep only the
  // newest version.
  static constexpr Epoch kNoFloor = 0;
  virtual Epoch OldestReaderEpoch() const = 0;

  // ---- S5 build snapshot ----
  // First build of an accelerator only: wait until every writer that started
  // writing `tables` before this accelerator's capture was registered has
  // finished (its changes would not be captured). Returns false on timeout.
  virtual bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &tables,
                                             uint64_t timeout_us) = 0;
  // Opens a build snapshot for `accelerator` on the calling thread.
  virtual std::unique_ptr<BuildSnapshot> OpenBuildSnapshot(AcceleratorRuntime &accelerator) = 0;

  // ---- S5 maintenance lookups ----
  // Module lookups during maintenance must see the database exactly as the
  // changes being applied left it. Called on the engine-work thread before
  // applying changes through epoch `through`; returns the epoch the engine's
  // lookups see now. In-process engines maintain in commit order: `through`.
  // Postgres applies after visibility, so it may return more: the runtime
  // then applies everything through that epoch as one change (or waits for
  // the changes that have not arrived).
  virtual Epoch MaintenanceLookupEpoch(Epoch through) { return through; }

  // ---- S6 placement ----
  // Where engine-facing work runs. In-process engines run builds and
  // maintenance on pool threads / the writer's thread: return false.
  // Postgres's shard worker must run them on its main (SPI) thread: return
  // true, and drain RuntimeHost::RunEngineWork() from that thread.
  virtual bool EngineWorkOnHostThread() const = 0;

  // ---- S8 engine scans ----
  // Runs the accelerator's Build through the backend's per-query scan
  // binding, inside `snapshot`. Must call builder.Finish().
  virtual void RunBuild(AcceleratorRuntime &accelerator, BuildSnapshot &snapshot,
                        core::StateBuilder &builder) = 0;

  // ---- engine services for modules ----
  // Cost model for `accelerator` (resolved by engine + scale), and the
  // workload hint.
  virtual const maintenance::CostModelParams &CostModel(const core::AcceleratorId &accelerator) const = 0;
  virtual double ExpectedQueriesBeforeNextWrite(const core::AcceleratorId &) const { return 10.0; }
};

} // namespace accelerator_codegen::aux_state::runtime
