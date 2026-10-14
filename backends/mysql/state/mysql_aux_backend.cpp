// GENERATED BACKEND
#include "backends/mysql/state/mysql_aux_backend.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::engine::mysql {

MysqlAuxBackend::MysqlAuxBackend(
    core::AuxTransactionCoordinator &coordinator_p,
    core::AuxStateStore &state_store_p,
    VisibilityCallback visibility_p,
    CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p ? cost_model_p() : maintenance::CostModelParams{}),
      workload(std::move(workload_p)),
      worker_count(1) {
  coordinator.AttachServices(*this);
  const char *threads = std::getenv("ACCEL_AUX_THREADS");
  if (threads && *threads) {
    long parsed = std::strtol(threads, nullptr, 10);
    if (parsed > 0) {
      worker_count = static_cast<std::size_t>(parsed);
    }
  }
}

namespace {
// A build that logs more rows than this while it runs is discarded (and
// retried): replaying an unbounded log would cost more than rebuilding.
constexpr std::size_t kMaxBuildLogRows = 4u << 20;

std::size_t RowCount(const core::MutationBatch &m) {
  return m.old_rows.size() + m.new_rows.size() + 1;
}
} // namespace

void MysqlAuxBackend::SetSessionCoordinatorFactory(CoordinatorFactory f) {
  std::lock_guard<std::mutex> lock(mu);
  factory = std::move(f);
}

MysqlAuxBackend::Session &MysqlAuxBackend::SessionFor(SessionId session) {
  std::lock_guard<std::mutex> lock(mu);
  const SessionId key = factory ? session : 0;
  auto it = sessions.find(key);
  if (it != sessions.end()) {
    return *it->second;
  }
  auto fresh = std::make_unique<Session>();
  if (key != 0) {
    fresh->owned = factory();
    if (!fresh->owned) {
      throw std::logic_error("mysql session coordinator factory returned null");
    }
    fresh->owned->AttachServices(*this);
    fresh->coordinator = fresh->owned.get();
    fresh->per_session = true;
  } else {
    fresh->coordinator = &coordinator;
  }
  Session &ref = *fresh;
  sessions.emplace(key, std::move(fresh));
  return ref;
}

void MysqlAuxBackend::CaptureMutation(const core::MutationBatch &mutation,
                                      SessionId session) {
  Session &s = SessionFor(session);
  bool open_frame = false;
  {
    std::lock_guard<std::mutex> lock(mu);
    staged_count.fetch_add(1);
    if (s.per_session && !s.stmt_open) {
      s.stmt_open = true;
      s.stmt_log_mark = s.pending_log.size();
      open_frame = true;
    }
    if (log_active && !log_overflow) {
      log_rows += RowCount(mutation);
      if (log_rows > kMaxBuildLogRows) {
        log_overflow = true;
        committed_log.clear();
      } else {
        s.pending_log.push_back(mutation);
      }
    }
  }
  std::lock_guard<std::mutex> module_lock(module_mutex);
  if (open_frame) {
    s.coordinator->BeginSubtransaction();
  }
  s.coordinator->OnMutation(mutation);
}

void MysqlAuxBackend::FinishStatement(Session &s, bool failed) {
  if (!s.per_session) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!s.stmt_open) {
      return;
    }
    s.stmt_open = false;
    if (failed && s.pending_log.size() > s.stmt_log_mark) {
      s.pending_log.resize(s.stmt_log_mark);
    }
  }
  std::lock_guard<std::mutex> module_lock(module_mutex);
  if (failed) {
    s.coordinator->AbortSubtransaction();
  } else {
    s.coordinator->CommitSubtransaction(); // the statement's writes join the transaction
  }
}

void MysqlAuxBackend::EndStatement(SessionId session, bool failed) {
  FinishStatement(SessionFor(session), failed);
}

void MysqlAuxBackend::PoisonSession(SessionId session) {
  Session &s = SessionFor(session);
  std::lock_guard<std::mutex> lock(mu);
  s.poisoned = true;
}

bool MysqlAuxBackend::SessionHasWork(SessionId session) {
  std::lock_guard<std::mutex> lock(mu);
  auto it = sessions.find(factory ? session : 0);
  if (it == sessions.end()) {
    return false;
  }
  const Session &s = *it->second;
  return s.stmt_open || !s.pending_log.empty() || s.failure_open || s.poisoned ||
         s.coordinator->HasPendingWork();
}

void MysqlAuxBackend::OnPreCommit(SessionId session) {
  Session &s = SessionFor(session);
  FinishStatement(s, false);
  {
    std::lock_guard<std::mutex> lock(mu);
    if (s.poisoned) {
      return; // discarded at commit
    }
  }
  std::lock_guard<std::mutex> module_lock(module_mutex);
  s.coordinator->PreCommit();
}

void MysqlAuxBackend::OnCommit(SessionId session) {
  Session &s = SessionFor(session);
  FinishStatement(s, false);
  bool poisoned = false;
  {
    std::lock_guard<std::mutex> lock(mu);
    poisoned = s.poisoned;
  }
  auto finish = [&](bool publish_log) {
    std::lock_guard<std::mutex> lock(mu);
    s.failure_open = false;
    s.poisoned = false;
    if (publish_log && log_active && !log_overflow && !s.pending_log.empty()) {
      committed_log.insert(committed_log.end(),
                           std::make_move_iterator(s.pending_log.begin()),
                           std::make_move_iterator(s.pending_log.end()));
    }
    s.pending_log.clear();
  };
  try {
    std::lock_guard<std::mutex> module_lock(module_mutex);
    if (poisoned) {
      s.coordinator->Abort();
    } else {
      s.coordinator->Commit();
    }
  } catch (...) {
    finish(false);
    throw;
  }
  finish(!poisoned);
  if (poisoned) {
    // The transaction committed in the engine but its writes were not tracked
    // exactly: the state is rebuilt from the tables on the next read.
    NoteCaptureFailure(/*transaction_open=*/false, session);
  }
}

void MysqlAuxBackend::OnAbort(SessionId session) {
  Session &s = SessionFor(session);
  {
    std::lock_guard<std::mutex> module_lock(module_mutex);
    s.coordinator->Abort(); // also drops an open statement frame
  }
  std::lock_guard<std::mutex> lock(mu);
  s.stmt_open = false;
  s.failure_open = false;
  s.poisoned = false;
  s.pending_log.clear();
}

void MysqlAuxBackend::EndSession(SessionId session) {
  std::unique_ptr<Session> gone;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!factory || session == 0) {
      return;
    }
    auto it = sessions.find(session);
    if (it == sessions.end()) {
      return;
    }
    gone = std::move(it->second);
    sessions.erase(it);
  }
  std::lock_guard<std::mutex> module_lock(module_mutex);
  gone->coordinator->Abort();
}

bool MysqlAuxBackend::HasPendingWork() {
  std::lock_guard<std::mutex> lock(mu);
  return HasPendingWorkLocked();
}

bool MysqlAuxBackend::HasPendingWorkLocked() {
  if (coordinator.HasPendingWork()) {
    return true;
  }
  for (const auto &entry : sessions) {
    const Session &s = *entry.second;
    if (s.stmt_open || !s.pending_log.empty() || s.failure_open || s.poisoned ||
        s.coordinator->HasPendingWork()) {
      return true;
    }
  }
  return false;
}

bool MysqlAuxBackend::BeginBuildLog(uint64_t expected_staged) {
  std::lock_guard<std::mutex> commit_lock(commit_mutex); // no commit in flight
  std::lock_guard<std::mutex> lock(mu);
  if (staged_count.load() != expected_staged || HasPendingWorkLocked()) {
    return false;
  }
  log_active = true;
  log_overflow = false;
  log_rows = 0;
  log_failures_at_start = capture_failures.load();
  committed_log.clear();
  for (auto &entry : sessions) {
    entry.second->pending_log.clear();
  }
  return true;
}

MysqlAuxBackend::BuildLog MysqlAuxBackend::EndBuildLog() {
  std::lock_guard<std::mutex> lock(mu);
  BuildLog out;
  out.complete = log_active && !log_overflow &&
                 capture_failures.load() == log_failures_at_start;
  out.batches = std::move(committed_log);
  committed_log.clear();
  // Transactions still open now start after the view: the installed state
  // gets their writes through their own commit.
  for (auto &entry : sessions) {
    entry.second->pending_log.clear();
  }
  log_active = false;
  log_overflow = false;
  log_rows = 0;
  return out;
}

void MysqlAuxBackend::NoteCaptureFailure(bool transaction_open, SessionId session) {
  Session *s = transaction_open ? &SessionFor(session) : nullptr;
  {
    std::lock_guard<std::mutex> lock(mu);
    staged_count.fetch_add(1); // a captured event, for BeginBuildLog's check
    capture_failures.fetch_add(1);
    if (s) s->failure_open = true;
  }
  state_store.Invalidate(core::AllPartitionsPlan());
}

core::VisibilityToken MysqlAuxBackend::CurrentVisibility() const {
  return visibility ? visibility() : core::VisibilityToken{};
}

core::AuxStateStore &MysqlAuxBackend::StateStore() { return state_store; }

const maintenance::CostModelParams &
MysqlAuxBackend::MaintenanceCostModel() const {
  return cost_model;
}

double MysqlAuxBackend::ExpectedQueriesBeforeNextWrite() const {
  const double override_q = core::ExpectedQueriesOverride();
  if (override_q >= 0.0) {
    return override_q;
  }
  return workload ? std::max(0.0, workload()) : 10.0;
}

bool MysqlAuxBackend::AllowsSynchronousRebuildOnCommit() const {
  return false;
}

std::size_t MysqlAuxBackend::WorkerCount() const {
  return worker_count;
}

std::optional<std::vector<int32_t>>
MysqlAuxBackend::MutationCaptureAttnums(core::TableId table) const {
  return coordinator.MutationCaptureAttnums(table);
}

} // namespace accelerator_codegen::aux_state::engine::mysql
