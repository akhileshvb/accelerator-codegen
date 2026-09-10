// GENERATED BACKEND
#pragma once

#include <functional>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

class PostgresAuxBackend final : public core::AuxEngineServices {
public:
  using VisibilityCallback = std::function<core::VisibilityToken()>;
  using CostModelCallback = std::function<maintenance::CostModelParams()>;
  using WorkloadCallback = std::function<double()>;

  PostgresAuxBackend(core::AuxTransactionCoordinator &coordinator,
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

  // PostgreSQL commit hooks cannot run table scans.
  bool AllowsSynchronousRebuildOnCommit() const override;

private:
  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
};

} // namespace accelerator_codegen::aux_state::engine::postgres
