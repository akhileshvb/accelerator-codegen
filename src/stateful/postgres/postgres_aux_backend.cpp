// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"

#include <algorithm>
#include <utility>

namespace accelerator_codegen::aux_state::engine::postgres {

PostgresAuxBackend::PostgresAuxBackend(
    core::AuxTransactionCoordinator &coordinator_p,
    core::AuxStateStore &state_store_p,
    VisibilityCallback visibility_p,
    CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p ? cost_model_p() : maintenance::CostModelParams{}),
      workload(std::move(workload_p)) {
  coordinator.AttachServices(*this);
}

void PostgresAuxBackend::CaptureMutation(
    const core::MutationBatch &mutation) {
  coordinator.OnMutation(mutation);
}

void PostgresAuxBackend::OnPreCommit() { coordinator.PreCommit(); }

void PostgresAuxBackend::OnCommit() { coordinator.Commit(); }

void PostgresAuxBackend::OnAbort() { coordinator.Abort(); }

core::VisibilityToken PostgresAuxBackend::CurrentVisibility() const {
  return visibility ? visibility() : core::VisibilityToken{};
}

core::AuxStateStore &PostgresAuxBackend::StateStore() { return state_store; }

const maintenance::CostModelParams &
PostgresAuxBackend::MaintenanceCostModel() const {
  return cost_model;
}

double PostgresAuxBackend::ExpectedQueriesBeforeNextWrite() const {
  return workload ? std::max(0.0, workload()) : 10.0;
}

bool PostgresAuxBackend::AllowsSynchronousRebuildOnCommit() const {
  return false;
}

std::optional<std::vector<int32_t>>
PostgresAuxBackend::MutationCaptureAttnums(core::TableId table) const {
  return coordinator.MutationCaptureAttnums(table);
}

} // namespace accelerator_codegen::aux_state::engine::postgres
