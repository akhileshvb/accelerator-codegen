// SCAFFOLDING
// C1/C4/C7/C11: one accelerator's runtime: module, versioned store, applier,
// background builds, reads at an epoch. Engine-neutral.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
  uint64_t rows_applied = 0;        // changed rows maintained incrementally
  double apply_us = 0;              // time maintaining them
  uint64_t bytes_published = 0;     // new partition payload bytes published (estimate)
  uint64_t builds = 0;
  double build_us = 0;
  uint64_t catch_up_commits = 0;
  uint64_t reads_hit = 0;
  uint64_t reads_waited = 0;
  uint64_t reads_fallback = 0;
  uint64_t gaps = 0;
};

enum class ReadResult { Served, Fallback };

struct ReadOptions {
  // The reader's epoch (S3). VersionedAuxStore::kNewestSnapshot: newest.
  Epoch epoch = VersionedAuxStore::kNewestSnapshot;
  // Dense-sequence engines (Postgres, MySQL): every commit with epoch <=
  // `require_applied_through` must be applied before serving (it may be
  // committed but not yet applied). nullopt: rely on the store's pending
  // marks (in-process engines mark a commit pending before it is visible).
  std::optional<Epoch> require_applied_through;
  // How long the reader may wait for a pending commit, build or catch-up
  // before falling back (C11). Backends derive it from the fallback cost.
  uint64_t max_wait_us = 0;
};

class AcceleratorRuntime : public core::AuxEngineServices {
public:
  AcceleratorRuntime(std::shared_ptr<core::AuxQueryModule> module, EngineSlots &slots,
                     ThreadBudget &threads, std::string store_name = {});
  ~AcceleratorRuntime() override;

  const core::AcceleratorId &Id() const { return id; }
  core::AuxQueryModule &Module() { return *module; }
  std::vector<core::TableId> DependencyTables() const;

  // ---- reads (any thread; never take the commit or build locks) ----
  ReadResult Read(const ReadOptions &options, const core::QueryArguments &arguments,
                  core::AuxRowSink &sink);

  // ---- in-process commits (writer's thread), driven by RuntimeHost ----
  // Takes this accelerator's place in commit order (blocks while another
  // in-process commit of it is between StageCommit and Publish/Abort),
  // assigns its sequence number, and stages maintenance from `batches` when
  // the state is synced; otherwise only records them.
  Seq StageCommit(uint64_t txn, const std::vector<ColumnarBatchRef> &batches);
  void PublishCommit(Epoch epoch);  // the engine committed at `epoch`
  void AbortCommit();               // the engine rolled back

  // ---- shipped commits (Postgres worker main thread) ----
  void OfferCommit(CommittedChange change);
  void AnnounceCommit(Seq seq);
  // Applies whatever is ready; may start a build (on a gap or when stale).
  void Pump();

  // ---- builds ----
  // Ensures a build is running or scheduled (no-op if synced or building).
  void RequestBuild();
  // Runs one build synchronously on the calling thread (used by the host's
  // build scheduler and by tests). Returns true if installed and synced.
  bool BuildNow();
  bool IsSynced() const;

  // ---- core::AuxEngineServices (what modules see) ----
  core::VisibilityToken CurrentVisibility() const override;
  core::AuxStateStore &StateStore() override;
  const maintenance::CostModelParams &MaintenanceCostModel() const override;
  double ExpectedQueriesBeforeNextWrite() const override;
  bool AllowsSynchronousRebuildOnCommit() const override { return false; }
  bool ReadersCanBuildState() const override { return false; }
  std::size_t WorkerCount() const override;

  AcceleratorStats Stats() const;
  VersionedAuxStore &Store() { return *store; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
  std::shared_ptr<core::AuxQueryModule> module;
  core::AcceleratorId id;
  EngineSlots &slots;
  ThreadBudget &threads;
  std::unique_ptr<VersionedAuxStore> store;
};

} // namespace accelerator_codegen::aux_state::runtime
