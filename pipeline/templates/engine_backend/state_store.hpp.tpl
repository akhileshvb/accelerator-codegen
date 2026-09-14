// GENERATED BACKEND
#pragma once

#include <functional>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

class {{EngineName}}StateStore final : public core::AuxStateStore {
public:
  using EpochProvider = std::function<core::Version()>;
  explicit {{EngineName}}StateStore(EpochProvider epoch_provider = {});

  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::ValidityResult CheckValidity(const core::QueryKey &,
                                     const core::VisibilityToken &) const override;
  void Invalidate(const core::InvalidationPlan &) override;
  bool TryPublish(const core::StagedMaintenanceOutcome &,
                  core::Version expected_version) override;

private:
  PartitionedStateStore store;
  EpochProvider epoch_provider;
};

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
