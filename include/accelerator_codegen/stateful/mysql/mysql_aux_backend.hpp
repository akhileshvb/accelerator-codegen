// GENERATED BACKEND
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
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

  // ---- Per-session transactions ---------------------------------------
  // One MySQL connection is one session; its transaction owns its own staged
  // aux writes (a coordinator of its own, over the same module) that are
  // committed or rolled back independently of every other session's, so
  // another session's ROLLBACK never discards them and a COMMIT never
  // publishes another session's uncommitted writes. Commits are published one
  // session at a time under CommitMutex() (the caller holds it, see
  // DispatchMysqlStatementEnd), in the order the commit events arrive: the
  // server delivers a session's commit event right after the engine commit,
  // and a later transaction that touches the same rows cannot proceed before
  // that commit released them.
  //
  // `SessionId` is an opaque connection key chosen by the host
  // (CurrentMysqlSession()); 0 is the legacy single shared session on the
  // constructor's coordinator. Per-session coordinators exist only once a
  // factory is set (SetSessionCoordinatorFactory); without one every session
  // maps to the shared coordinator.
  using SessionId = std::uintptr_t;
  using CoordinatorFactory =
      std::function<std::unique_ptr<core::AuxTransactionCoordinator>()>;
  // Creates a coordinator over the same module(s) as the constructor's, with
  // its own transaction buffers (services already attached by the caller or
  // attached here).
  void SetSessionCoordinatorFactory(CoordinatorFactory factory);

  // Statements of a session are staged in a statement frame, so a statement
  // that fails inside a still-open transaction (EndStatement(failed=true))
  // drops only its own writes.
  void CaptureMutation(const core::MutationBatch &mutation, SessionId session = 0);
  // The session's current statement finished. `failed`: it was rolled back.
  void EndStatement(SessionId session, bool failed);
  // The session's transaction can no longer be tracked write by write
  // (ROLLBACK TO SAVEPOINT): when it ends its writes are discarded and the
  // accelerator is invalidated (the next read rebuilds from the tables).
  void PoisonSession(SessionId session);
  // The session has captured work (staged writes or an open statement).
  bool SessionHasWork(SessionId session);
  void OnPreCommit(SessionId session = 0);
  void OnCommit(SessionId session = 0);
  void OnAbort(SessionId session = 0);
  // The connection closed: its open transaction (if any) is rolled back.
  void EndSession(SessionId session);

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
  // Serializes every call into the (shared) query module made on behalf of a
  // session's transaction: staging a write, pre-commit, commit, abort, replay.
  // Lock order: CommitMutex(), then ModuleMutex().
  std::mutex &ModuleMutex() { return module_mutex; }
  // Number of mutations captured so far (monotonic).
  uint64_t StagedMutationCount() const { return staged_count.load(); }
  // Captured work is between capture and its commit/abort (or a capture
  // failed and its transaction has not ended yet).
  bool HasPendingWork();
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
  // for a capture failure; false when commit/abort itself failed): until that
  // session's commit/abort event, HasPendingWork() stays true.
  void NoteCaptureFailure(bool transaction_open = true, SessionId session = 0);
  uint64_t CaptureFailureCount() const { return capture_failures.load(); }

private:
  struct Session {
    // Own coordinator (per-session mode) or null: the shared one.
    std::unique_ptr<core::AuxTransactionCoordinator> owned;
    core::AuxTransactionCoordinator *coordinator = nullptr;
    bool per_session = false;       // statement frames, own coordinator
    bool stmt_open = false;         // a statement frame is open on the coordinator
    std::size_t stmt_log_mark = 0;  // pending_log size when the frame opened
    bool failure_open = false;      // a capture of this session's open transaction failed
    bool poisoned = false;          // see PoisonSession
    // Build log: this session's staged writes, moved to committed_log at its
    // commit, dropped at abort.
    std::vector<core::MutationBatch> pending_log;
  };

  // The session record, created on first use. Callers hold no lock.
  Session &SessionFor(SessionId session);
  // Closes the open statement frame (commit: its writes join the transaction).
  void FinishStatement(Session &s, bool failed);
  bool HasPendingWorkLocked();

  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
  std::size_t worker_count;

  std::mutex commit_mutex;
  std::mutex module_mutex;
  std::atomic<uint64_t> staged_count{0};
  std::atomic<uint64_t> capture_failures{0};
  // Guards sessions, factory, every Session::pending_log / failure_open, and
  // the build-log fields below. Taken after (never while calling into) a
  // coordinator, except for the leaf HasPendingWork() queries.
  std::mutex mu;
  CoordinatorFactory factory;
  std::unordered_map<SessionId, std::unique_ptr<Session>> sessions;
  bool log_active = false;
  bool log_overflow = false;
  uint64_t log_failures_at_start = 0;
  std::size_t log_rows = 0;
  std::vector<core::MutationBatch> committed_log;
};

} // namespace accelerator_codegen::aux_state::engine::mysql
