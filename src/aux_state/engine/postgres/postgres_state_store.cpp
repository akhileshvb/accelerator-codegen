#include "accelerator_codegen/aux_state/engine/postgres/postgres_state_store.hpp"

#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::engine::postgres {

PostgresStateStore::PostgresStateStore(ReadCallback read_p,
                                       DeserializeCallback deserialize_p,
                                       SerializeCallback serialize_p,
                                       ValidityCallback validity_p,
                                       InvalidateCallback invalidate_p,
                                       PublishCallback publish_p)
    : read(std::move(read_p)), deserialize(std::move(deserialize_p)),
      serialize(std::move(serialize_p)), validity(std::move(validity_p)),
      invalidate(std::move(invalidate_p)), publish(std::move(publish_p)) {
  if (!read || !deserialize || !serialize || !validity || !invalidate ||
      !publish) {
    throw std::invalid_argument(
        "PostgresStateStore requires shared-memory callbacks");
  }
}

core::StateSnapshot
PostgresStateStore::Read(const core::VisibilityToken &visibility) const {
  const auto serialized = read(visibility);
  core::StateSnapshot snapshot;
  snapshot.accelerator = serialized.accelerator;
  snapshot.visibility = serialized.visibility;
  snapshot.available = serialized.available;
  snapshot.partitions.reserve(serialized.partitions.size());
  for (const auto &image : serialized.partitions) {
    snapshot.partitions.push_back(deserialize(image));
  }
  return snapshot;
}

core::ValidityResult PostgresStateStore::CheckValidity(
    const core::QueryKey &key, const core::VisibilityToken &visibility) const {
  return validity(key, visibility);
}

void PostgresStateStore::Invalidate(const core::InvalidationPlan &plan) {
  invalidate(plan);
}

bool PostgresStateStore::TryPublish(
    const core::StagedMaintenanceOutcome &outcome,
    core::Version expected_version) {
  std::vector<SerializedStateImage> images;
  images.reserve(outcome.updates.size());
  for (const auto &image : outcome.updates) {
    images.push_back(serialize(image));
  }
  return publish(images, expected_version);
}

} // namespace accelerator_codegen::aux_state::engine::postgres
