// GENERATED BACKEND
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

class MysqlStateStore final : public core::AuxStateStore {
public:
  using EpochProvider = std::function<core::Version()>;
  explicit MysqlStateStore(EpochProvider epoch_provider = {});

  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::StateSnapshot
  ReadPartitions(const core::VisibilityToken &visibility,
                 const std::vector<core::PartitionId> &partitions) const override;
  core::ValidityResult CheckValidity(const core::QueryKey &,
                                     const core::VisibilityToken &) const override;
  // Empty QueryKey.value means "all published partitions for this accelerator
  // are valid" (not partition 0). Non-empty value is a decimal partition id.
  void Invalidate(const core::InvalidationPlan &) override;
  bool TryPublish(const core::StagedMaintenanceOutcome &,
                  core::Version expected_version) override;

  void Install(std::vector<core::StateImage> built, std::string accelerator_id);
  bool NeedsRebuild() const;

private:
  PartitionedStateStore store;
  EpochProvider epoch_provider;

  mutable std::mutex accel_mutex;
  std::unordered_map<PartitionKey, core::AcceleratorId> accel_index;
};

} // namespace accelerator_codegen::aux_state::engine::mysql
