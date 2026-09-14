// GENERATED BACKEND
#include "accelerator_codegen/stateful/{{engine_name}}/{{engine_name}}_state_store.hpp"

#include <utility>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

{{EngineName}}StateStore::{{EngineName}}StateStore(EpochProvider epoch_provider_p)
    : epoch_provider(std::move(epoch_provider_p)) {}

core::StateSnapshot
{{EngineName}}StateStore::Read(const core::VisibilityToken &visibility) const {
  // ForEachSlot: skip !Valid / empty payload / valid_through_epoch > visibility.mutation_epoch.
  // Copy visibility onto the snapshot; set image.version from GetPartitionVersion.
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
  // Must call store.TryPublishPartition (not PublishPartition). Return false on
  // a lost race. Stamp valid_through_mutation_epoch from epoch_provider() or 0.
  // Index CheckValidity from staged.updates[].accelerator + partition — no
  // RegisterQueryKey API.
  (void)staged;
  (void)expected_version;
  return false;
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
