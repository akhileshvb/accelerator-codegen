// GENERATED BACKEND - serialized_boundary store
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace accelerator_codegen::aux_state::engine::postgres {

// ----------------------------------------------------------------
// PostgresStateStore
// ----------------------------------------------------------------

PostgresStateStore::PostgresStateStore()
    : backing_dir("/tmp/accel_pg_aux_state") {
  mkdir(backing_dir.c_str(), 0700);
}

PostgresStateStore::PostgresStateStore(std::string backing_dir_p)
    : backing_dir(std::move(backing_dir_p)) {
  mkdir(backing_dir.c_str(), 0700);
}

PostgresStateStore::~PostgresStateStore() = default;

std::string PostgresStateStore::GetBackingPath() const {
  return backing_dir + "/accel_store_meta.bin";
}

std::string PostgresStateStore::GetMutationEpochPath() const {
  return backing_dir + "/accel_mutation_epoch.txt";
}

std::string PostgresStateStore::PartitionPath(PartitionKey key) const {
  return backing_dir + "/p_" + std::to_string(key) + ".dat";
}

void PostgresStateStore::LoadFromBacking() {
  std::lock_guard<std::recursive_mutex> lock(store_mutex);
  // Always refresh from durable files so long-lived backends see peer publishes
  // / invalidations (do not skip when in-process slots are already non-empty).
  mutation_epoch = 0;
  {
    std::ifstream in(GetMutationEpochPath());
    if (in) in >> mutation_epoch;
  }
  store.InvalidateAll();

  std::ifstream meta(GetBackingPath(), std::ios::binary);
  if (!meta) return;
  std::vector<uint8_t> meta_bytes(
      std::istreambuf_iterator<char>(meta), {});
  core::ByteReader reader(meta_bytes);

  uint32_t count = 0;
  if (!reader.U32(count)) return;
  for (uint32_t i = 0; i < count; ++i) {
    int32_t key_val = 0;
    uint64_t epoch = 0;
    if (!reader.I32(key_val) || !reader.U64(epoch)) return;
    std::ifstream pf(PartitionPath(key_val), std::ios::binary);
    if (!pf) continue;
    auto vec = std::vector<uint8_t>(
        std::istreambuf_iterator<char>(pf), {});
    store.PublishPartition(key_val, std::make_shared<const std::vector<uint8_t>>(std::move(vec)), epoch);
    if (epoch > mutation_epoch) mutation_epoch = epoch;
  }
}

void PostgresStateStore::SaveToBacking() const {
  // Write meta: count, then for each partition: key, epoch.
  // Also write each partition blob.
  std::vector<std::pair<PartitionKey, uint64_t>> parts;
  store.ForEachSlot([&](PartitionKey key, PartitionStatus status,
                        const std::shared_ptr<const void> &payload,
                        uint64_t valid_epoch) {
    if (status == PartitionStatus::Valid && payload) {
      // Persist the partition blob
      auto *blob = static_cast<const std::vector<uint8_t> *>(payload.get());
      std::ofstream pf(PartitionPath(key), std::ios::binary | std::ios::trunc);
      if (pf) pf.write(reinterpret_cast<const char *>(blob->data()),
                        static_cast<long>(blob->size()));
      parts.emplace_back(key, valid_epoch);
    }
  });
  
  {
    core::ByteWriter w;
    w.U32(static_cast<uint32_t>(parts.size()));
    for (const auto &[key, epoch] : parts) {
      w.I32(key);
      w.U64(epoch);
    }
    std::ofstream meta(GetBackingPath(), std::ios::binary | std::ios::trunc);
    if (meta) meta.write(reinterpret_cast<const char *>(w.bytes.data()),
                          static_cast<long>(w.bytes.size()));
  }
  
  // Persist epoch
  {
    std::ofstream eout(GetMutationEpochPath(), std::ios::trunc);
    if (eout) eout << mutation_epoch;
  }
}

core::Version PostgresStateStore::GetCurrentMutationEpoch() const {
  return mutation_epoch;
}

void PostgresStateStore::EnsureLoaded() {
  // serialized_boundary: always re-sync from backing (cross-backend freshness).
  LoadFromBacking();
}

core::StateSnapshot
PostgresStateStore::Read(const core::VisibilityToken &visibility) const {
  std::lock_guard<std::recursive_mutex> lock(store_mutex);
  const_cast<PostgresStateStore *>(this)->LoadFromBacking();
  core::StateSnapshot snapshot;
  snapshot.accelerator = "postgres";
  snapshot.visibility = visibility;

  store.ForEachSlot(
      [&](PartitionKey key, PartitionStatus status,
          const std::shared_ptr<const void> &payload,
          uint64_t valid_through_epoch) {
        if (status != PartitionStatus::Valid || !payload ||
            valid_through_epoch > visibility.mutation_epoch) {
          return;
        }
        core::StateImage image;
        image.accelerator = "postgres";
        image.partition = key;
        image.version = 0; // avoid nested lock in ForEachSlot visitor
        image.payload = payload;
        snapshot.partitions.emplace_back(std::move(image));
      });
  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::ValidityResult
PostgresStateStore::CheckValidity(const core::QueryKey &key,
                                  const core::VisibilityToken &visibility) const {
  std::lock_guard<std::recursive_mutex> lock(store_mutex);
  const_cast<PostgresStateStore *>(this)->LoadFromBacking();
  if (key.accelerator != "postgres") {
    return {core::ValidityCode::Missing, 0};
  }

  core::ValidityResult result;
  result.code = core::ValidityCode::Missing;

  bool saw_matching = false;
  bool matching_valid = false;
  bool matching_not_visible = false;
  bool matching_invalidated = false;

  store.ForEachSlot(
      [&](PartitionKey, PartitionStatus status,
          const std::shared_ptr<const void> &payload,
          uint64_t valid_through_epoch) {
        if (!payload) return;
        const auto *raw =
            static_cast<const std::vector<uint8_t> *>(payload.get());
        if (!raw) return;
        std::string payload_key;
        try {
          payload_key = DecodeToyPayload(*raw).first;
        } catch (...) {
          return;
        }
        if (!key.value.empty() && payload_key != key.value) {
          return;
        }
        saw_matching = true;
        if (status == PartitionStatus::Absent) {
          return;
        }
        if (status == PartitionStatus::Invalid) {
          matching_invalidated = true;
          return;
        }
        if (valid_through_epoch > visibility.mutation_epoch) {
          matching_not_visible = true;
          return;
        }
        if (status == PartitionStatus::Valid) {
          matching_valid = true;
        }
      });

  if (!saw_matching && !key.value.empty()) {
    // Also accept empty-key checks that mean "any postgres partition".
    return {core::ValidityCode::Missing, 0};
  }
  if (matching_valid) {
    result.code = core::ValidityCode::Valid;
    result.visible_version = mutation_epoch;
  } else if (matching_not_visible) {
    result.code = core::ValidityCode::NotVisible;
  } else if (matching_invalidated || saw_matching) {
    result.code = core::ValidityCode::Invalidated;
  } else {
    // No key filter: fall back to all-slot validity.
    bool saw_slot = false;
    bool all_valid = true;
    store.ForEachSlot(
        [&](PartitionKey, PartitionStatus status,
            const std::shared_ptr<const void> &,
            uint64_t valid_through_epoch) {
          saw_slot = true;
          if (status == PartitionStatus::Absent ||
              status == PartitionStatus::Invalid ||
              valid_through_epoch > visibility.mutation_epoch) {
            all_valid = false;
            result.code = valid_through_epoch > visibility.mutation_epoch
                              ? core::ValidityCode::NotVisible
                              : core::ValidityCode::Invalidated;
          }
        });
    if (saw_slot && all_valid) {
      result.code = core::ValidityCode::Valid;
      result.visible_version = mutation_epoch;
    }
  }
  return result;
}

void PostgresStateStore::Invalidate(const core::InvalidationPlan &plan) {
  std::lock_guard<std::recursive_mutex> lock(store_mutex);
  if (plan.scope == InvalidationScope::AllPartitions) {
    store.InvalidateAll();
  } else if (plan.scope == InvalidationScope::PartitionSet) {
    for (const auto partition : plan.partitions) {
      store.InvalidatePartition(partition);
    }
  }
  // Persist invalidation to backing files so other backends see it.
  SaveToBacking();
}

bool PostgresStateStore::TryPublish(const core::StagedMaintenanceOutcome &outcome,
                                   core::Version expected_version) {
  std::lock_guard<std::recursive_mutex> lock(store_mutex);
  LoadFromBacking();
  uint64_t publish_epoch = mutation_epoch + 1;
  for (const auto &image : outcome.updates) {
    if (!image.payload) return false;
    if (!store.TryPublishPartition(image.partition, image.payload,
                                   publish_epoch, expected_version)) {
      return false;
    }
  }
  if (!outcome.updates.empty()) {
    mutation_epoch = publish_epoch;
    SaveToBacking();
  }
  return true;
}

// ----------------------------------------------------------------
// Toy Serialized Payload (contract test support)
// ----------------------------------------------------------------

namespace {
static constexpr uint32_t kToyMagic = 0xACC1;
static constexpr uint32_t kToyVersion = 1;
} // anonymous namespace

std::vector<uint8_t> ToySerializedPayload::Encode() const {
  core::ByteWriter w;
  core::WriteImageHeader(w, kToyMagic, kToyVersion, 1);
  w.String(key);
  w.I64(value);
  return w.bytes;
}

ToySerializedPayload ToySerializedPayload::Decode(const std::vector<uint8_t> &data) {
  core::ByteReader r(data);
  uint32_t count = 0;
  ToySerializedPayload p;
  if (!core::ReadImageHeader(r, kToyMagic, kToyVersion, count) || count != 1) {
    throw std::runtime_error("ToySerializedPayload: invalid header");
  }
  if (!r.String(p.key)) {
    throw std::runtime_error("ToySerializedPayload: truncated key");
  }
  if (!r.I64(p.value)) {
    throw std::runtime_error("ToySerializedPayload: truncated value");
  }
  if (!r.FullyConsumed()) {
    throw std::runtime_error("ToySerializedPayload: trailing bytes");
  }
  return p;
}

std::vector<uint8_t> EncodeToyPayload(const std::string &key, int64_t value) {
  ToySerializedPayload p;
  p.key = key;
  p.value = value;
  return p.Encode();
}

std::pair<std::string, int64_t> DecodeToyPayload(const std::vector<uint8_t> &data) {
  auto p = ToySerializedPayload::Decode(data);
  return {p.key, p.value};
}

} // namespace accelerator_codegen::aux_state::engine::postgres
