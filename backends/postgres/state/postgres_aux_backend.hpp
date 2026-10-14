// GENERATED BACKEND
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"

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
  // `depth` = PostgreSQL transaction nest level - 1 (0 = top level).
  void CaptureMutationAtDepth(const core::MutationBatch &mutation,
                              std::size_t depth);
  void OnSubCommit(std::size_t depth);
  void OnSubAbort(std::size_t depth);
  void OnPreCommit();
  void OnCommit();
  void OnAbort();

  core::VisibilityToken CurrentVisibility() const override;
  core::AuxStateStore &StateStore() override;
  const maintenance::CostModelParams &MaintenanceCostModel() const override;
  double ExpectedQueriesBeforeNextWrite() const override;
  bool AllowsSynchronousRebuildOnCommit() const override;

  // Physical attnums for statement-level transition capture (0-based).
  std::optional<std::vector<int32_t>>
  MutationCaptureAttnums(core::TableId table) const;

private:
  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
};

} // namespace accelerator_codegen::aux_state::engine::postgres
