// SCAFFOLDING
#include "substrate/aux/core/aux_engine_services.hpp"

#include <cstdlib>

namespace accelerator_codegen::aux_state::core {

const maintenance::CostModelParams &AuxEngineServices::MaintenanceCostModel()
    const {
  static const maintenance::CostModelParams defaults;
  return defaults;
}

double AuxEngineServices::ExpectedQueriesBeforeNextWrite() const {
  return 10.0;
}

uint64_t AuxEngineServices::DataScaleRows() const { return 0; }

bool AuxEngineServices::AllowsSynchronousRebuildOnCommit() const {
  return true;
}

bool AuxEngineServices::ReadersCanBuildState() const { return true; }

MaintenanceDecision AuxEngineServices::ChooseMaintenance(
    const MaintenanceRequest &request) const {
  MaintenanceRequest with_engine = request;
  with_engine.deferred_rebuild_supported =
      request.deferred_rebuild_supported && ReadersCanBuildState();
  if (with_engine.data_rows_hint == 0) {
    with_engine.data_rows_hint = DataScaleRows();
  }
  return SelectAuxMaintenanceAction(with_engine, MaintenanceCostModel());
}

std::size_t AuxEngineServices::WorkerCount() const {
  const char *value = std::getenv("ACCEL_AUX_THREADS");
  if (value && *value) {
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed > 0) {
      return static_cast<std::size_t>(parsed);
    }
  }
  return 1;
}

} // namespace accelerator_codegen::aux_state::core
