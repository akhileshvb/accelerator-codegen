#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state {

PartitionStatus PartitionedStateStore::GetStatus(PartitionKey key) const {
	std::lock_guard<std::mutex> guard(mutex);
	auto it = slots.find(key);
	if (it == slots.end()) {
		return PartitionStatus::Absent;
	}
	return it->second.status;
}

std::shared_ptr<const void> PartitionedStateStore::GetReadablePayload(PartitionKey key) const {
	std::lock_guard<std::mutex> guard(mutex);
	auto it = slots.find(key);
	if (it == slots.end() || it->second.status != PartitionStatus::Valid) {
		return nullptr;
	}
	return it->second.payload;
}

void PartitionedStateStore::PublishPartition(PartitionKey key, std::shared_ptr<const void> payload) {
	std::lock_guard<std::mutex> guard(mutex);
	PartitionSlot slot;
	slot.status = PartitionStatus::Valid;
	slot.payload = std::move(payload);
	slots[key] = std::move(slot);
}

void PartitionedStateStore::InvalidatePartition(PartitionKey key) {
	std::lock_guard<std::mutex> guard(mutex);
	PartitionSlot slot;
	slot.status = PartitionStatus::Invalid;
	slot.payload = nullptr;
	slots[key] = std::move(slot);
}

void PartitionedStateStore::InvalidateAll() {
	std::lock_guard<std::mutex> guard(mutex);
	for (auto &entry : slots) {
		entry.second.status = PartitionStatus::Invalid;
		entry.second.payload = nullptr;
	}
}

duckdb::idx_t PartitionedStateStore::ValidPartitionCount() const {
	std::lock_guard<std::mutex> guard(mutex);
	duckdb::idx_t count = 0;
	for (const auto &entry : slots) {
		if (entry.second.status == PartitionStatus::Valid) {
			count++;
		}
	}
	return count;
}

duckdb::idx_t PartitionedStateStore::TotalPartitionSlots() const {
	std::lock_guard<std::mutex> guard(mutex);
	return slots.size();
}

void PartitionedStateStore::ForEachSlot(
    const std::function<void(PartitionKey key, PartitionStatus status, const std::shared_ptr<const void> &payload)> &visitor)
    const {
	std::lock_guard<std::mutex> guard(mutex);
	for (const auto &entry : slots) {
		visitor(entry.first, entry.second.status, entry.second.payload);
	}
}

} // namespace accelerator_codegen::aux_state
