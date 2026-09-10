// SCAFFOLDING
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state {

PartitionStatus PartitionedStateStore::ToLegacyStatus(PartitionSlot::Status status) {
	switch (status) {
	case PartitionSlot::Status::Valid:
		return PartitionStatus::Valid;
	case PartitionSlot::Status::Invalid:
		return PartitionStatus::Invalid;
	case PartitionSlot::Status::Absent:
	default:
		return PartitionStatus::Absent;
	}
}

void PartitionedStateStore::AdjustValidCount(PartitionSlot::Status before, PartitionSlot::Status after) {
	if (before == PartitionSlot::Status::Valid && after != PartitionSlot::Status::Valid) {
		valid_partition_count--;
	} else if (before != PartitionSlot::Status::Valid && after == PartitionSlot::Status::Valid) {
		valid_partition_count++;
	}
}

PartitionStatus PartitionedStateStore::GetStatus(PartitionKey key) const {
	std::lock_guard<std::mutex> guard(mutex);
	auto it = slots.find(key);
	if (it == slots.end()) {
		return PartitionStatus::Absent;
	}
	return ToLegacyStatus(it->second.status);
}

std::shared_ptr<const void> PartitionedStateStore::GetReadablePayload(PartitionKey key) const {
	std::lock_guard<std::mutex> guard(mutex);
	auto it = slots.find(key);
	if (it == slots.end() || it->second.status != PartitionSlot::Status::Valid) {
		return nullptr;
	}
	return it->second.payload;
}

uint64_t PartitionedStateStore::GetPartitionVersion(PartitionKey key) const {
	std::lock_guard<std::mutex> guard(mutex);
	auto it = slots.find(key);
	if (it == slots.end()) {
		return 0;
	}
	return it->second.partition_version;
}

uint64_t PartitionedStateStore::GetValidThroughMutationEpoch(PartitionKey key) const {
	std::lock_guard<std::mutex> guard(mutex);
	auto it = slots.find(key);
	if (it == slots.end()) {
		return 0;
	}
	return it->second.valid_through_mutation_epoch;
}

void PartitionedStateStore::PublishPartition(PartitionKey key, std::shared_ptr<const void> payload,
                                            uint64_t valid_through_mutation_epoch) {
	std::lock_guard<std::mutex> guard(mutex);
	auto &slot = slots[key];
	const auto before = slot.status;
	slot.status = PartitionSlot::Status::Valid;
	slot.payload = std::move(payload);
	slot.partition_version++;
	slot.valid_through_mutation_epoch = valid_through_mutation_epoch;
	AdjustValidCount(before, slot.status);
}

bool PartitionedStateStore::TryPublishPartition(PartitionKey key, std::shared_ptr<const void> payload,
                                                uint64_t valid_through_mutation_epoch,
                                                uint64_t expected_partition_version) {
	std::lock_guard<std::mutex> guard(mutex);
	auto it = slots.find(key);
	const uint64_t current_version = it == slots.end() ? 0 : it->second.partition_version;
	if (expected_partition_version != 0 && current_version != expected_partition_version) {
		auto &slot = slots[key];
		const auto before = slot.status;
		slot.status = PartitionSlot::Status::Invalid;
		slot.payload = nullptr;
		slot.partition_version++;
		AdjustValidCount(before, slot.status);
		return false;
	}
	auto &slot = slots[key];
	const auto before = slot.status;
	slot.status = PartitionSlot::Status::Valid;
	slot.payload = std::move(payload);
	slot.partition_version++;
	slot.valid_through_mutation_epoch = valid_through_mutation_epoch;
	AdjustValidCount(before, slot.status);
	return true;
}

void PartitionedStateStore::InvalidatePartition(PartitionKey key) {
	std::lock_guard<std::mutex> guard(mutex);
	auto &slot = slots[key];
	const auto before = slot.status;
	slot.status = PartitionSlot::Status::Invalid;
	slot.payload = nullptr;
	slot.partition_version++;
	slot.valid_through_mutation_epoch = 0;
	AdjustValidCount(before, slot.status);
}

void PartitionedStateStore::InvalidateAll() {
	std::lock_guard<std::mutex> guard(mutex);
	for (auto &entry : slots) {
		const auto before = entry.second.status;
		entry.second.status = PartitionSlot::Status::Invalid;
		entry.second.payload = nullptr;
		entry.second.partition_version++;
		entry.second.valid_through_mutation_epoch = 0;
		AdjustValidCount(before, entry.second.status);
	}
}

std::size_t PartitionedStateStore::ValidPartitionCount() const {
	std::lock_guard<std::mutex> guard(mutex);
	return valid_partition_count;
}

std::size_t PartitionedStateStore::TotalPartitionSlots() const {
	std::lock_guard<std::mutex> guard(mutex);
	return slots.size();
}

void PartitionedStateStore::ForEachSlot(
    const std::function<void(PartitionKey key, PartitionStatus status, const std::shared_ptr<const void> &payload,
                              uint64_t valid_through_mutation_epoch)> &visitor) const {
	std::lock_guard<std::mutex> guard(mutex);
	for (const auto &entry : slots) {
		visitor(entry.first, ToLegacyStatus(entry.second.status), entry.second.payload,
		        entry.second.valid_through_mutation_epoch);
	}
}

void PartitionedStateStore::VisitReadablePartitionsInRange(
    PartitionKey start_partition, PartitionKey end_partition,
    const std::function<void(PartitionKey key, const std::shared_ptr<const void> &payload)> &visitor) const {
	std::lock_guard<std::mutex> guard(mutex);
	for (PartitionKey key = start_partition; key < end_partition; ++key) {
		auto it = slots.find(key);
		if (it == slots.end() || it->second.status != PartitionSlot::Status::Valid || !it->second.payload) {
			continue;
		}
		visitor(key, it->second.payload);
	}
}

} // namespace accelerator_codegen::aux_state
