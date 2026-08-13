// GENERATED BACKEND
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_state_store.hpp"
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

class DuckDBStateStore final : public core::AuxStateStore {
public:
  using EpochProvider = std::function<core::Version()>;

  DuckDBStateStore(
      std::string accelerator_id,
      ::accelerator_codegen::aux_state::PartitionedStateStore &store,
      EpochProvider epoch_provider = {});

  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::StateSnapshot ReadPartitions(
      const core::VisibilityToken &,
      const std::vector<core::PartitionId> &) const override;
  core::ValidityResult
  CheckValidity(const core::QueryKey &,
                const core::VisibilityToken &) const override;
  void Invalidate(const core::InvalidationPlan &) override;
  bool TryPublish(const core::StagedMaintenanceOutcome &,
                  core::Version expected_version) override;

private:
  core::StateSnapshot ReadInternal(
      const core::VisibilityToken &,
      const std::vector<core::PartitionId> *filter) const;

  std::string accelerator_id;
  ::accelerator_codegen::aux_state::PartitionedStateStore &store;
  EpochProvider epoch_provider;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
