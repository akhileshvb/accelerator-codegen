// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

PostgresStateStore::PostgresStateStore(EpochProvider epoch_provider_p)
    : epoch_provider(std::move(epoch_provider_p)) {}

core::StateSnapshot
PostgresStateStore::Read(const core::VisibilityToken &visibility) const {
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;

  struct Collected {
    PartitionKey key;
    std::shared_ptr<const void> payload;
  };
  std::vector<Collected> collected;

  {
    std::lock_guard<std::mutex> guard(accel_mutex);
    store.ForEachSlot(
        [&](PartitionKey key, PartitionStatus status,
            const std::shared_ptr<const void> &payload,
            uint64_t valid_through_mutation_epoch) {
          if (status != PartitionStatus::Valid || !payload) {
            return;
          }
          if (visibility.mutation_epoch > 0 &&
              valid_through_mutation_epoch > visibility.mutation_epoch) {
            return;
          }
          auto it = accel_index.find(key);
          if (it == accel_index.end()) {
            return;
          }
          collected.push_back(Collected{key, payload});
        });
  }

  for (const auto &c : collected) {
    auto it = accel_index.find(c.key);
    core::StateImage image;
    image.accelerator = it->second;
    image.partition = static_cast<core::PartitionId>(c.key);
    image.version = store.GetPartitionVersion(c.key);
    image.payload = c.payload;
    snapshot.partitions.push_back(std::move(image));
  }

  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::StateSnapshot
PostgresStateStore::ReadPartitions(
    const core::VisibilityToken &visibility,
    const std::vector<core::PartitionId> &partitions) const {
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;

  std::lock_guard<std::mutex> guard(accel_mutex);

  for (auto partition_id : partitions) {
    PartitionKey key = static_cast<PartitionKey>(partition_id);
    auto status = store.GetStatus(key);
    if (status != PartitionStatus::Valid) {
      continue;
    }
    auto payload = store.GetReadablePayload(key);
    if (!payload) {
      continue;
    }
    uint64_t epoch = store.GetValidThroughMutationEpoch(key);
    if (visibility.mutation_epoch > 0 && epoch > visibility.mutation_epoch) {
      continue;
    }
    auto it = accel_index.find(key);
    if (it == accel_index.end()) {
      continue;
    }
    core::StateImage image;
    image.accelerator = it->second;
    image.partition = partition_id;
    image.version = store.GetPartitionVersion(key);
    image.payload = payload;
    snapshot.partitions.push_back(std::move(image));
  }

  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::ValidityResult
PostgresStateStore::CheckValidity(
    const core::QueryKey &key,
    const core::VisibilityToken &visibility) const {
  core::ValidityResult result;
  result.code = core::ValidityCode::Missing;

  std::lock_guard<std::mutex> guard(accel_mutex);

  // Empty QueryKey means "is this accelerator's published image valid?", not
  // partition 0. Same contract as DuckDBStateStore / MysqlStateStore.
  if (key.value.empty()) {
    bool saw_this = false;
    bool saw_invalid = false;
    bool saw_not_visible = false;
    std::vector<PartitionKey> visible_keys;
    store.ForEachSlot(
        [&](PartitionKey pk, PartitionStatus status,
            const std::shared_ptr<const void> &payload, uint64_t epoch) {
          auto it = accel_index.find(pk);
          if (it == accel_index.end() || it->second != key.accelerator) {
            return;
          }
          saw_this = true;
          if (status != PartitionStatus::Valid || !payload) {
            saw_invalid = true;
            return;
          }
          if (visibility.mutation_epoch > 0 &&
              epoch > visibility.mutation_epoch) {
            saw_not_visible = true;
            return;
          }
          visible_keys.push_back(pk);
        });
    if (!saw_this) {
      return result;
    }
    if (saw_invalid) {
      result.code = core::ValidityCode::Invalidated;
      return result;
    }
    if (saw_not_visible) {
      result.code = core::ValidityCode::NotVisible;
      return result;
    }
    core::Version max_version = 0;
    for (auto pk : visible_keys) {
      max_version = std::max(max_version, store.GetPartitionVersion(pk));
    }
    result.code = core::ValidityCode::Valid;
    result.visible_version = max_version;
    return result;
  }

  core::PartitionId partition_id = 0;
  auto fc_result = std::from_chars(
      key.value.data(), key.value.data() + key.value.size(), partition_id);
  if (fc_result.ec != std::errc{}) {
    return result;
  }

  PartitionKey pk = static_cast<PartitionKey>(partition_id);

  auto it = accel_index.find(pk);
  if (it == accel_index.end() || it->second != key.accelerator) {
    return result;
  }

  auto status = store.GetStatus(pk);
  if (status == PartitionStatus::Absent) {
    return result;
  }
  if (status == PartitionStatus::Invalid) {
    result.code = core::ValidityCode::Invalidated;
    return result;
  }

  uint64_t epoch = store.GetValidThroughMutationEpoch(pk);
  if (visibility.mutation_epoch > 0 && epoch > visibility.mutation_epoch) {
    result.code = core::ValidityCode::NotVisible;
    return result;
  }

  result.code = core::ValidityCode::Valid;
  result.visible_version = store.GetPartitionVersion(pk);
  return result;
}

void PostgresStateStore::Invalidate(const core::InvalidationPlan &plan) {
  std::lock_guard<std::mutex> guard(accel_mutex);

  if (plan.scope == InvalidationScope::AllPartitions) {
    store.InvalidateAll();
    return;
  }

  if (plan.scope == InvalidationScope::PartitionSet) {
    for (auto pk : plan.partitions) {
      store.InvalidatePartition(pk);
    }
  }
}

bool PostgresStateStore::TryPublish(
    const core::StagedMaintenanceOutcome &staged,
    core::Version expected_version) {
  core::Version epoch = epoch_provider ? epoch_provider() : 0;

  std::lock_guard<std::mutex> guard(accel_mutex);

  for (const auto &image : staged.updates) {
    PartitionKey key = static_cast<PartitionKey>(image.partition);
    bool ok = store.TryPublishPartition(key, image.payload, epoch,
                                        expected_version);
    if (!ok) {
      return false;
    }
    accel_index[key] = image.accelerator;
  }

  return true;
}

void PostgresStateStore::MarkTransactionDirty() {
  std::lock_guard<std::mutex> guard(accel_mutex);
  transaction_dirty_ = true;
}

void PostgresStateStore::ClearTransactionDirty() {
  std::lock_guard<std::mutex> guard(accel_mutex);
  transaction_dirty_ = false;
}

bool PostgresStateStore::IsTransactionDirty() const {
  std::lock_guard<std::mutex> guard(accel_mutex);
  return transaction_dirty_;
}

void PostgresStateStore::Install(std::vector<core::StateImage> built,
                                 std::string accelerator_id) {
  core::Version epoch = epoch_provider ? epoch_provider() : 0;
  std::lock_guard<std::mutex> guard(accel_mutex);
  store.InvalidateAll();
  accel_index.clear();
  for (auto &image : built) {
    if (!accelerator_id.empty()) {
      image.accelerator = accelerator_id;
    }
    PartitionKey key = static_cast<PartitionKey>(image.partition);
    // expected_version 0 = unconditional publish (initial Build).
    store.TryPublishPartition(key, image.payload, epoch, 0);
    accel_index[key] = image.accelerator;
  }
}

// Linearized ReadHit: under one accel lock + one store ForEachSlot pass,
// validate accelerator-wide then collect every required partition.
// Missing/Invalid/NotVisible ⇒ available=false (caller must fallback).
core::StateSnapshot PostgresStateStore::ReadHitPartitions(
    const core::VisibilityToken &visibility,
    const core::QueryKey &accel_key,
    const std::vector<core::PartitionId> &partitions) const {
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;
  snapshot.available = false;

  std::lock_guard<std::mutex> guard(accel_mutex);

  bool saw_this = false;
  bool saw_invalid = false;
  bool saw_not_visible = false;
  std::unordered_map<PartitionKey, core::StateImage> found;

  store.ForEachSlot(
      [&](PartitionKey pk, PartitionStatus status,
          const std::shared_ptr<const void> &payload, uint64_t epoch) {
        auto it = accel_index.find(pk);
        if (it == accel_index.end() || it->second != accel_key.accelerator) {
          return;
        }
        saw_this = true;
        if (status != PartitionStatus::Valid || !payload) {
          saw_invalid = true;
          return;
        }
        if (visibility.mutation_epoch > 0 &&
            epoch > visibility.mutation_epoch) {
          saw_not_visible = true;
          return;
        }
        core::StateImage image;
        image.accelerator = it->second;
        image.partition = static_cast<core::PartitionId>(pk);
        image.version = 0; // version unused by Execute; avoid nested store lock
        image.payload = payload;
        found[pk] = std::move(image);
      });

  if (!saw_this || saw_invalid || saw_not_visible) {
    return snapshot;
  }

  for (auto partition_id : partitions) {
    PartitionKey key = static_cast<PartitionKey>(partition_id);
    auto it = found.find(key);
    if (it == found.end()) {
      snapshot.partitions.clear();
      snapshot.available = false;
      return snapshot;
    }
    snapshot.partitions.push_back(it->second);
  }

  snapshot.available = snapshot.partitions.size() == partitions.size();
  if (!snapshot.available) {
    snapshot.partitions.clear();
  }
  return snapshot;
}

bool PostgresStateStore::NeedsRebuild() const {
  std::lock_guard<std::mutex> guard(accel_mutex);
  bool saw_valid = false;
  bool saw_invalid = false;
  store.ForEachSlot([&](PartitionKey pk, PartitionStatus status,
                        const std::shared_ptr<const void> &payload, uint64_t) {
    if (accel_index.find(pk) == accel_index.end()) {
      return;
    }
    if (status != PartitionStatus::Valid || !payload) {
      saw_invalid = true;
    } else {
      saw_valid = true;
    }
  });
  return !saw_valid || saw_invalid;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
