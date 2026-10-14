// GENERATED BACKEND
#include "backends/{{engine_name}}/state/{{engine_name}}_aux_backend.hpp"

#include <algorithm>
#include <utility>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

{{EngineName}}AuxBackend::{{EngineName}}AuxBackend(
    core::AuxTransactionCoordinator &coordinator_p, core::AuxStateStore &state_store_p,
    VisibilityCallback visibility_p, CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p ? cost_model_p() : maintenance::CostModelParams{}),
      workload(std::move(workload_p)) {
  coordinator.AttachServices(*this);
}

void {{EngineName}}AuxBackend::CaptureMutation(const core::MutationBatch &mutation) {
  coordinator.OnMutation(mutation);
}

void {{EngineName}}AuxBackend::OnPreCommit() { coordinator.PreCommit(); }

void {{EngineName}}AuxBackend::OnCommit() { coordinator.Commit(); }

void {{EngineName}}AuxBackend::OnAbort() { coordinator.Abort(); }

core::VisibilityToken {{EngineName}}AuxBackend::CurrentVisibility() const {
  return visibility ? visibility() : core::VisibilityToken{};
}

core::AuxStateStore &{{EngineName}}AuxBackend::StateStore() { return state_store; }

const maintenance::CostModelParams &
{{EngineName}}AuxBackend::MaintenanceCostModel() const {
  return cost_model;
}

double {{EngineName}}AuxBackend::ExpectedQueriesBeforeNextWrite() const {
  return workload ? std::max(0.0, workload()) : 10.0;
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
