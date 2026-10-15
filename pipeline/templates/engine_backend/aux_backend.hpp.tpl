// GENERATED BACKEND
#pragma once

#include <functional>

#include "substrate/aux/core/aux_registry.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

class {{EngineName}}AuxBackend final : public core::AuxEngineServices {
public:
  using VisibilityCallback = std::function<core::VisibilityToken()>;
  using CostModelCallback = std::function<maintenance::CostModelParams()>;
  using WorkloadCallback = std::function<double()>;

  {{EngineName}}AuxBackend(core::AuxTransactionCoordinator &coordinator,
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
  // Override when the engine differs from the defaults (aux_types.hpp):
  //   bool AllowsSynchronousRebuildOnCommit() const override;  // commit hook cannot scan
  //   bool ReadersCanBuildState() const override;              // reader cannot run SQL at its snapshot
  //   std::size_t WorkerCount() const override;                // host worker setting

private:
  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
};

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
