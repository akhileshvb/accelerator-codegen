// SCAFFOLDING
// C1/C4/C7/C11: one accelerator's runtime: module, versioned store, applier,
// background builds, reads at an epoch. Engine-neutral.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/columnar_batch.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"
#include "substrate/aux/runtime/sequenced_applier.hpp"
#include "substrate/aux/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state::runtime {

class ThreadBudget;

// Per-accelerator counters, for run reports and the incremental-maintenance
// feasibility metrics.
struct AcceleratorStats {
  uint64_t commits_applied = 0;
  uint64_t commits_stale = 0; // commits it could not maintain: each costs a rebuild
  uint64_t rows_applied = 0; // changed rows maintained incrementally
  double apply_us = 0;       // time maintaining them
  uint64_t builds = 0;
  double build_us = 0;
  uint64_t build_failures = 0;   // builds that threw: a real fault, logged
  uint64_t builds_abandoned = 0; // builds invalidated meanwhile or whose catch-up did not apply
  std::string last_build_error;
  uint64_t catch_up_commits = 0;
  uint64_t reads_hit = 0;
  uint64_t reads_waited = 0;
  uint64_t reads_fallback = 0;
  uint64_t gaps = 0;
  std::string last_stale_reason;
  // The policy's decision for the last maintained (or stale) commit: what it chose and why.
  std::optional<core::MaintenanceDecisionLogEntry> last_decision;
};

enum class ReadResult { Served, Fallback };

struct ReadOptions {
  // The reader's epoch (S3). VersionedAuxStore::kNewestSnapshot: newest.
  Epoch epoch = VersionedAuxStore::kNewestSnapshot;
  // Dense-sequence engines (Postgres): every commit with epoch <=
  // `require_applied_through` must be applied before serving (it may be
  // committed but not yet shipped). In-process engines leave it unset: their
  // commits are marked in flight from pre-commit until published.
  std::optional<Epoch> require_applied_through;
  // The most a reader waits for an in-flight commit, a build or a catch-up
  // (C11): a ceiling; it falls back sooner when the SQL is cheaper than waiting.
  uint64_t max_wait_us = 0;
};

// The accelerator's store: VersionedAuxStore by default; an engine may supply
// another core::AuxStateStore (the Verus store) with its install entry point.
// A factory returning an empty handle keeps the default.
struct StoreHandle {
  std::unique_ptr<core::AuxStateStore> store;
  std::function<void(std::vector<core::StateImage>, Epoch)> install;
};
using StoreFactory = std::function<StoreHandle(const core::AcceleratorId &,
                                               std::function<Epoch()> epoch,
                                               std::function<Epoch()> floor)>;

// Batches carry host table ids: one per engine table, shared by every
// accelerator. A module numbers its own tables; this maps its numbers to the
// host's (a table not listed keeps its number).
using TableMap = std::unordered_map<core::TableId, core::TableId>; // module -> host

// Every read is answered from SQL (differential tests): ACCEL_AUX_FORCE_FALLBACK=1,
// or this switch for an engine whose server environment is fixed.
void SetForcedFallback(bool forced);
bool ForcedFallback();

class AcceleratorRuntime : public core::AuxEngineServices {
public:
  // Runs a build somewhere: a pool thread (in-process engines) or the host's
  // engine thread (S6). Default: ThreadBudget::Spawn.
  using BuildScheduler = std::function<void(std::function<void()>)>;

  AcceleratorRuntime(std::shared_ptr<core::AuxQueryModule> module, EngineSlots &slots,
                     ThreadBudget &threads, BuildScheduler schedule = nullptr,
                     StoreFactory make_store = nullptr, const TableMap &tables = {});
  ~AcceleratorRuntime() override;

  const core::AcceleratorId &Id() const { return id; }
  core::AuxQueryModule &Module() { return *module; }
  // Host ids of the tables it depends on, and the columns it watches on one
  // (empty: the whole row); nullptr if it does not depend on `table`.
  std::vector<core::TableId> DependencyTables() const;
  const std::vector<int32_t> *Projection(core::TableId table) const;

  // ---- reads (any thread; never take the commit or build locks) ----
  // While a servable guard lives, reads of this accelerator's store on this
  // thread resolve at options.epoch, in one version.
  class ReadGuard {
  public:
    ~ReadGuard();
    ReadGuard(ReadGuard &&) noexcept;
    bool Servable() const { return servable; }

  private:
    friend class AcceleratorRuntime;
    struct Scope;
    ReadGuard(bool servable, std::unique_ptr<Scope> scope);
    bool servable;
    std::unique_ptr<Scope> scope;
  };
  // Waits until `options` can be served: for an in-flight commit up to
  // options.max_wait_us, otherwise only while that is cheaper than the SQL
  // fallback (C11).
  ReadGuard BeginRead(const ReadOptions &options);
  // BeginRead, then the module's CheckValidity and Execute.
  ReadResult Read(const ReadOptions &options, const core::QueryArguments &arguments,
                  core::AuxRowSink &sink);

  // ---- in-process commits (writer's thread), driven by RuntimeHost ----
  // Pre-commit: takes this accelerator's place in commit order (blocks while
  // another of its commits is between BeginCommit and Publish/Abort) and
  // marks readers that may see the commit as waiting.
  Seq BeginCommit(uint64_t txn, std::vector<ColumnarBatchRef> batches);
  // The engine committed at `epoch`: a store shared by several accelerators
  // enrolls this one (AuxStateStore::JoinCommit). The host enrolls every
  // staged accelerator before any publishes, so the store installs the engine
  // commit once.
  void OpenStoreCommit(Epoch epoch);
  // Maintain (lookups see the database as the commit left it) and publish;
  // queued for catch-up if not synced. Closes the store's commit either way.
  void PublishCommit(Epoch epoch);
  void AbortCommit(); // the engine rolled back

  // ---- shipped commits (Postgres worker main thread) ----
  void OfferCommit(CommittedChange change);
  void AnnounceCommit(Seq seq);
  // Applies whatever is ready; requests a build when stale, or when a gap
  // has persisted for `gap_timeout_us`.
  void Pump(uint64_t gap_timeout_us = 5'000'000);

  // ---- builds ----
  // Ensures a build is running or scheduled (no-op if synced or building).
  void RequestBuild();
  // Runs one build on the calling thread (waiting for one already running).
  // Returns true if installed and synced.
  bool BuildNow();
  // Drops the state: readers wait or fall back until the next build.
  void Invalidate();
  // A commit at `epoch` that no capture saw (a writer that began before the
  // accelerator was registered): a state built before it lacks it, so
  // readers from `epoch` on wait for a rebuild.
  void InvalidateFrom(Epoch epoch);
  // The services maintenance passes to the module (default: this runtime).
  // An engine whose modules need engine context supplies its own, delegating
  // to this runtime.
  void UseModuleServices(core::AuxEngineServices &services);
  bool IsSynced() const;

  // ---- core::AuxEngineServices (what modules see) ----
  core::VisibilityToken CurrentVisibility() const override;
  core::AuxStateStore &StateStore() override;
  const maintenance::CostModelParams &MaintenanceCostModel() const override;
  double ExpectedQueriesBeforeNextWrite() const override;
  uint64_t DataScaleRows() const override;
  std::size_t WorkerCount() const override;
  void ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const override;

  // How long the engine's SQL took to answer a read of this accelerator; with
  // the cost model's estimate it prices the fallback a waiting reader weighs.
  void NoteSqlFallbackUs(double us);
  // The same, when the measurement is kept elsewhere (Postgres: in shared
  // memory, written by the backends that ran the SQL).
  void SetSqlFallbackUs(double us);

  AcceleratorStats Stats() const;

private:
  struct Impl;

  core::StagedMaintenanceOutcome Stage(const std::vector<ColumnarBatchRef> &batches);
  bool Publish(const core::StagedMaintenanceOutcome &outcome, Epoch epoch);
  bool ApplyChange(const CommittedChange &change);
  void RecordStale(const std::string &reason);
  void ApplyReady();
  void Collapse(Epoch from, Epoch to);
  void AbandonBuild();
  void BuildEnded(uint64_t AcceleratorStats::*counter, const std::string &error);
  void Applied(const CommittedChange &change, double us);
  void MarkStale(Epoch from);
  bool Servable(Epoch epoch, const std::optional<Epoch> &applied_through) const;
  double UntilServableUs() const;
  bool WaitWorthIt(Epoch epoch) const;
  bool Collapsed(Epoch epoch) const;

  std::shared_ptr<core::AuxQueryModule> module;
  core::AcceleratorId id;
  EngineSlots &slots;
  ThreadBudget &threads;
  std::unique_ptr<Impl> impl;
};

} // namespace accelerator_codegen::aux_state::runtime
