// GENERATED BACKEND
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"

#include <algorithm>
#include <cstdlib>
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

void MysqlAuxBackend::CaptureMutation(
    const core::MutationBatch &mutation) {
  coordinator.OnMutation(mutation);
}

void MysqlAuxBackend::OnPreCommit() { coordinator.PreCommit(); }

void MysqlAuxBackend::OnCommit() { coordinator.Commit(); }

void MysqlAuxBackend::OnAbort() { coordinator.Abort(); }

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
