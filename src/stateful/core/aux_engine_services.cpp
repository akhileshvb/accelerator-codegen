#include "accelerator_codegen/stateful/core/aux_engine_services.hpp"

namespace accelerator_codegen::aux_state::core {

const maintenance::CostModelParams &AuxEngineServices::MaintenanceCostModel()
    const {
  static const maintenance::CostModelParams defaults;
  return defaults;
}

double AuxEngineServices::ExpectedQueriesBeforeNextWrite() const {
  return 10.0;
}

MaintenanceDecision AuxEngineServices::ChooseMaintenance(
    const MaintenanceRequest &request) const {
  return SelectAuxMaintenanceAction(request, MaintenanceCostModel());
}

} // namespace accelerator_codegen::aux_state::core
