// GENERATED BACKEND
#pragma once

#include <map>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_state_store.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

class PostgresStateStore final : public core::AuxStateStore {
public:
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

  void MarkTransactionDirty() override;
  void ClearTransactionDirty() override;

  // Initial Build path: replace the in-process image set.
  void Install(std::vector<core::StateImage> built,
               std::string accelerator_id);

private:
  std::string accelerator;
  std::map<core::PartitionId, core::StateImage> images;
  core::Version version = 0;
  bool valid = false;
  bool transaction_dirty = false;
};

} // namespace accelerator_codegen::aux_state::engine::postgres
