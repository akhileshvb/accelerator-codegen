// GENERATED BACKEND
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

class MysqlAuxBackend final : public core::AuxEngineServices {
public:
  using VisibilityCallback = std::function<core::VisibilityToken()>;
  using CostModelCallback = std::function<maintenance::CostModelParams()>;
  using WorkloadCallback = std::function<double()>;

  MysqlAuxBackend(core::AuxTransactionCoordinator &coordinator,
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
  bool AllowsSynchronousRebuildOnCommit() const override;
  std::size_t WorkerCount() const override;

  std::optional<std::vector<int32_t>>
  MutationCaptureAttnums(core::TableId table) const;

private:
  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
  std::size_t worker_count;
};

} // namespace accelerator_codegen::aux_state::engine::mysql
