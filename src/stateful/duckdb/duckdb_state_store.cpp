// GENERATED BACKEND
#include "accelerator_codegen/stateful/core/aux_store_diagnostics.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::engine::duckdb {

DuckDBStateStore::DuckDBStateStore(
    std::string accelerator_id_p,
    ::accelerator_codegen::aux_state::PartitionedStateStore &store_p,
    EpochProvider epoch_provider_p)
    : accelerator_id(std::move(accelerator_id_p)), store(store_p),
      epoch_provider(std::move(epoch_provider_p)) {
}

core::StateSnapshot
DuckDBStateStore::Read(const core::VisibilityToken &visibility) const {
  return ReadInternal(visibility, nullptr);
}

core::StateSnapshot DuckDBStateStore::ReadPartitions(
    const core::VisibilityToken &visibility,
    const std::vector<core::PartitionId> &partitions) const {
  return ReadInternal(visibility, &partitions);
}

core::StateSnapshot DuckDBStateStore::ReadInternal(
    const core::VisibilityToken &visibility,
    const std::vector<core::PartitionId> *filter) const {
  const auto scan_start = std::chrono::steady_clock::now();
  core::StateSnapshot snapshot;
  snapshot.accelerator = accelerator_id;
  snapshot.visibility = visibility;
  std::vector<core::PartitionId> partitions;
  uint64_t bytes_read = 0;
  store.ForEachSlot(
      [&](::accelerator_codegen::aux_state::PartitionKey key,
          ::accelerator_codegen::aux_state::PartitionStatus status,
          const std::shared_ptr<const void> &payload,
          uint64_t valid_through_epoch) {
        if (filter && std::find(filter->begin(), filter->end(), key) ==
                          filter->end()) {
          return;
        }
        if (status !=
                ::accelerator_codegen::aux_state::PartitionStatus::Valid ||
            !payload || valid_through_epoch > visibility.mutation_epoch) {
          return;
        }
        core::StateImage image;
        image.accelerator = accelerator_id;
        image.partition = key;
        image.payload = payload;
        snapshot.partitions.emplace_back(std::move(image));
        partitions.emplace_back(key);
        if (core::AuxStoreProfilingBytesEnabled() && payload) {
          bytes_read += 1;
        }
      });
  for (std::size_t index = 0; index < partitions.size(); ++index) {
    snapshot.partitions[index].version =
        store.GetPartitionVersion(partitions[index]);
  }
  snapshot.available = !snapshot.partitions.empty();
  core::RecordPartitionsRead(snapshot.partitions.size(), bytes_read);
  (void)scan_start;
  return snapshot;
}

core::ValidityResult
DuckDBStateStore::CheckValidity(const core::QueryKey &key,
                                const core::VisibilityToken &visibility) const {
  if (key.accelerator != accelerator_id) {
    return {core::ValidityCode::Missing, 0};
  }
  const auto scan_start = std::chrono::steady_clock::now();
  core::ValidityResult result;
  result.code = core::ValidityCode::Missing;
  bool saw_slot = false;
  bool all_valid = true;
  uint64_t slots_scanned = 0;
  std::vector<core::PartitionId> valid_partitions;
  store.ForEachSlot(
      [&](::accelerator_codegen::aux_state::PartitionKey partition,
          ::accelerator_codegen::aux_state::PartitionStatus status,
          const std::shared_ptr<const void> &, uint64_t valid_through_epoch) {
        slots_scanned++;
        saw_slot = true;
        if (status ==
                ::accelerator_codegen::aux_state::PartitionStatus::Absent ||
            status ==
                ::accelerator_codegen::aux_state::PartitionStatus::Invalid ||
            valid_through_epoch > visibility.mutation_epoch) {
          all_valid = false;
          result.code = valid_through_epoch > visibility.mutation_epoch
                            ? core::ValidityCode::NotVisible
                            : core::ValidityCode::Invalidated;
          return;
        }
        valid_partitions.emplace_back(partition);
      });
  if (saw_slot && all_valid) {
    result.code = core::ValidityCode::Valid;
    for (const auto partition : valid_partitions) {
      result.visible_version = std::max(result.visible_version,
                                        store.GetPartitionVersion(partition));
    }
  }
  const auto scan_end = std::chrono::steady_clock::now();
  const double scan_us = static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(scan_end - scan_start)
          .count());
  core::RecordValidityScan(slots_scanned, scan_us);
  return result;
}

void DuckDBStateStore::Invalidate(const core::InvalidationPlan &plan) {
  if (plan.scope ==
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions) {
    store.InvalidateAll();
    return;
  }
  if (plan.scope !=
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet) {
    return;
  }
  for (const auto partition : plan.partitions) {
    store.InvalidatePartition(partition);
  }
}

bool DuckDBStateStore::TryPublish(const core::StagedMaintenanceOutcome &outcome,
                                  core::Version expected_version) {
  const auto publish_epoch = epoch_provider ? epoch_provider() : 0;
  for (const auto &image : outcome.updates) {
    const auto expected_partition_version =
        image.version != 0 ? image.version : expected_version;
    if (!image.payload ||
        !store.TryPublishPartition(image.partition, image.payload, publish_epoch,
                                    expected_partition_version)) {
      return false;
    }
  }
  return true;
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
