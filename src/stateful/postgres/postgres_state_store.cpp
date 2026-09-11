// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"

#include <utility>

namespace accelerator_codegen::aux_state::engine::postgres {

core::StateSnapshot
PostgresStateStore::Read(const core::VisibilityToken &visibility) const {
  core::StateSnapshot snapshot;
  snapshot.accelerator = accelerator;
  snapshot.visibility = visibility;
  if (!valid || transaction_dirty) {
    return snapshot;
  }
  for (const auto &entry : images) {
    snapshot.partitions.push_back(entry.second);
  }
  snapshot.available = !images.empty();
  return snapshot;
}

core::StateSnapshot PostgresStateStore::ReadPartitions(
    const core::VisibilityToken &visibility,
    const std::vector<core::PartitionId> &partitions) const {
  core::StateSnapshot snapshot;
  snapshot.accelerator = accelerator;
  snapshot.visibility = visibility;
  if (!valid) {
    return snapshot;
  }
  for (const auto partition : partitions) {
    const auto it = images.find(partition);
    if (it != images.end()) {
      snapshot.partitions.push_back(it->second);
    }
  }
  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::ValidityResult
PostgresStateStore::CheckValidity(const core::QueryKey &key,
                                  const core::VisibilityToken &) const {
  if (key.accelerator != accelerator) {
    return {core::ValidityCode::Missing, 0};
  }
  if (transaction_dirty) {
    return {core::ValidityCode::NotVisible, 0};
  }
  return {valid ? core::ValidityCode::Valid : core::ValidityCode::Missing,
          valid ? version : 0};
}

void PostgresStateStore::Invalidate(const core::InvalidationPlan &) {
  valid = false;
}

void PostgresStateStore::MarkTransactionDirty() { transaction_dirty = true; }

void PostgresStateStore::ClearTransactionDirty() { transaction_dirty = false; }

bool PostgresStateStore::TryPublish(const core::StagedMaintenanceOutcome &outcome,
                                    core::Version expected_version) {
  if (expected_version != 0 && expected_version != version) {
    valid = false;
    return false;
  }
  for (const auto &image : outcome.updates) {
    auto published = image;
    published.version = ++version;
    images[published.partition] = std::move(published);
  }
  valid = !images.empty();
  return true;
}

void PostgresStateStore::Install(std::vector<core::StateImage> built,
                                 std::string accelerator_id) {
  images.clear();
  accelerator = std::move(accelerator_id);
  ++version;
  for (auto &image : built) {
    image.version = version;
    images.emplace(image.partition, std::move(image));
  }
  valid = !images.empty();
}

} // namespace accelerator_codegen::aux_state::engine::postgres
