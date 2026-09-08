// GENERATED BACKEND
#include "accelerator_codegen/stateful/{{engine_name}}/{{engine_name}}_state_store.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

core::StateSnapshot
{{EngineName}}StateStore::Read(const core::VisibilityToken &visibility) const {
  // TODO: expose typed or decoded partition snapshots for this visibility.
  (void)visibility;
  return {};
}

core::ValidityResult
{{EngineName}}StateStore::CheckValidity(const core::QueryKey &key,
                                        const core::VisibilityToken &visibility) const {
  (void)key;
  (void)visibility;
  return {};
}

void {{EngineName}}StateStore::Invalidate(const core::InvalidationPlan &plan) {
  (void)plan;
}

bool {{EngineName}}StateStore::TryPublish(const core::StagedMaintenanceOutcome &staged,
                                          core::Version expected_version) {
  (void)staged;
  (void)expected_version;
  return false;
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
