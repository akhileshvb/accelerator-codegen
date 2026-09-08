// GENERATED BACKEND
#include "accelerator_codegen/stateful/{{engine_name}}/{{engine_name}}_aux_backend.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

{{EngineName}}AuxBackend::{{EngineName}}AuxBackend(
    core::AuxTransactionCoordinator &coordinator_p, core::AuxStateStore &state_store_p,
    VisibilityCallback visibility_p, CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p ? cost_model_p() : maintenance::CostModelParams{}),
      workload(std::move(workload_p)) {}

void {{EngineName}}AuxBackend::CaptureMutation(const core::MutationBatch &mutation) {
  coordinator.CaptureMutation(mutation);
}

void {{EngineName}}AuxBackend::OnPreCommit() { coordinator.OnPreCommit(*this); }

void {{EngineName}}AuxBackend::OnCommit() { coordinator.OnCommit(*this); }

void {{EngineName}}AuxBackend::OnAbort() { coordinator.OnAbort(); }

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
