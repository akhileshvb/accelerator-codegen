// GENERATED BACKEND
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
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

void MysqlAuxBackend::CaptureMutation(
    const core::MutationBatch &mutation) {
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    staged_count.fetch_add(1);
    if (log_active && !log_overflow) {
      log_rows += RowCount(mutation);
      if (log_rows > kMaxBuildLogRows) {
        log_overflow = true;
        pending_log.clear();
        committed_log.clear();
      } else {
        pending_log.push_back(mutation);
      }
    }
  }
  coordinator.OnMutation(mutation);
}

void MysqlAuxBackend::OnPreCommit() { coordinator.PreCommit(); }

void MysqlAuxBackend::OnCommit() {
  coordinator.Commit();
  failure_open = false;
  std::lock_guard<std::mutex> lock(log_mutex);
  if (log_active && !pending_log.empty()) {
    committed_log.insert(committed_log.end(),
                         std::make_move_iterator(pending_log.begin()),
                         std::make_move_iterator(pending_log.end()));
  }
  pending_log.clear();
}

void MysqlAuxBackend::OnAbort() {
  coordinator.Abort();
  failure_open = false;
  std::lock_guard<std::mutex> lock(log_mutex);
  pending_log.clear();
}

bool MysqlAuxBackend::BeginBuildLog(uint64_t expected_staged) {
  std::lock_guard<std::mutex> commit_lock(commit_mutex); // no commit in flight
  std::lock_guard<std::mutex> lock(log_mutex);
  if (staged_count.load() != expected_staged || HasPendingWork()) {
    return false;
  }
  log_active = true;
  log_overflow = false;
  log_rows = 0;
  log_failures_at_start = capture_failures.load();
  pending_log.clear();
  committed_log.clear();
  return true;
}

MysqlAuxBackend::BuildLog MysqlAuxBackend::EndBuildLog() {
  std::lock_guard<std::mutex> lock(log_mutex);
  BuildLog out;
  out.complete = log_active && !log_overflow &&
                 capture_failures.load() == log_failures_at_start;
  out.batches = std::move(committed_log);
  committed_log.clear();
  pending_log.clear();
  log_active = false;
  log_overflow = false;
  log_rows = 0;
  return out;
}

void MysqlAuxBackend::NoteCaptureFailure(bool transaction_open) {
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    staged_count.fetch_add(1); // a captured event, for BeginBuildLog's check
    capture_failures.fetch_add(1);
    if (transaction_open) failure_open = true;
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
