// GENERATED BACKEND
#pragma once

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

class {{EngineName}}StateStore final : public core::AuxStateStore {
public:
  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::ValidityResult CheckValidity(const core::QueryKey &,
                                     const core::VisibilityToken &) const override;
  void Invalidate(const core::InvalidationPlan &) override;
  bool TryPublish(const core::StagedMaintenanceOutcome &,
                  core::Version expected_version) override;

private:
  mutable PartitionedStateStore store;
};

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
