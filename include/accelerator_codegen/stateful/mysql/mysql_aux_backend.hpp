// GENERATED BACKEND
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

class MysqlAuxBackend final : public core::AuxEngineServices {
public:
  using VisibilityCallback = std::function<core::VisibilityToken()>;
  using CostModelCallback = std::function<maintenance::CostModelParams()>;
  using WorkloadCallback = std::function<double()>;

  MysqlAuxBackend(core::AuxTransactionCoordinator &coordinator,
                  core::AuxStateStore &state_store,
                  VisibilityCallback visibility,
                  CostModelCallback cost_model = {},
                  WorkloadCallback workload = {});

  void CaptureMutation(const core::MutationBatch &mutation);
  void OnPreCommit();
  void OnCommit();
  void OnAbort();

  core::VisibilityToken CurrentVisibility() const override;
  core::AuxStateStore &StateStore() override;
  const maintenance::CostModelParams &MaintenanceCostModel() const override;
  double ExpectedQueriesBeforeNextWrite() const override;
  bool AllowsSynchronousRebuildOnCommit() const override;
  std::size_t WorkerCount() const override;

  std::optional<std::vector<int32_t>>
  MutationCaptureAttnums(core::TableId table) const;

  // ---- Consistent-snapshot builds -------------------------------------
  // A build that scans through one read view (START TRANSACTION WITH
  // CONSISTENT SNAPSHOT) is installed without being restarted when writes
  // land meanwhile: the writes committed after the view are recorded here and
  // replayed on the installed state.  Protocol (Runtime::RebuildIfNeeded):
  //   1. no captured write pending, `staged = StagedMutationCount()`;
  //   2. open the read view;
  //   3. BeginBuildLog(staged): atomically checks that nothing was captured
  //      since step 1 (so every write in the view is already in the aux
  //      state's past, and every later write is after the view) and starts
  //      recording the committed writes;
  //   4. build from the view; writes keep committing and are recorded;
  //   5. under CommitMutex(): EndBuildLog(), install, replay, publish.
  // CommitMutex() serializes the commit/abort of captured writes with step 5.
  std::mutex &CommitMutex() { return commit_mutex; }
  // Number of mutations captured so far (monotonic).
  uint64_t StagedMutationCount() const { return staged_count.load(); }
  // Captured work is between capture and its commit/abort (or a capture
  // failed and its transaction has not ended yet).
  bool HasPendingWork() const { return coordinator.HasPendingWork() || failure_open.load(); }
  // See above. False (not recording) if a mutation was captured since
  // `expected_staged` was read or captured work is pending.
  bool BeginBuildLog(uint64_t expected_staged);
  struct BuildLog {
    std::vector<core::MutationBatch> batches; // committed since BeginBuildLog, in order
    // False if the record is unusable: too many rows were logged, or a write
    // was lost (NoteCaptureFailure) while recording. Discard the build.
    bool complete = true;
  };
  BuildLog EndBuildLog();
  // A captured write could not be decoded or applied: the aux state may have
  // missed it. Invalidates the state (the next read rebuilds) and poisons any
  // build in progress (its EndBuildLog() is incomplete).
  // `transaction_open`: the failed write's transaction is still running (true
  // for a capture failure; false when commit/abort itself failed): until its
  // commit/abort event, HasPendingWork() stays true.
  void NoteCaptureFailure(bool transaction_open = true);
  uint64_t CaptureFailureCount() const { return capture_failures.load(); }

private:
  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
  std::size_t worker_count;

  std::mutex commit_mutex;
  std::atomic<uint64_t> staged_count{0};
  std::atomic<uint64_t> capture_failures{0};
  std::atomic<bool> failure_open{false};
  // Build log: mutations staged in the coordinator's open transaction
  // (pending_log) move to committed_log at commit, are dropped at abort.
  std::mutex log_mutex;
  bool log_active = false;
  bool log_overflow = false;
  uint64_t log_failures_at_start = 0;
  std::size_t log_rows = 0;
  std::vector<core::MutationBatch> pending_log;
  std::vector<core::MutationBatch> committed_log;
};

} // namespace accelerator_codegen::aux_state::engine::mysql
