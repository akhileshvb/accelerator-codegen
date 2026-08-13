// SCAFFOLDING
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "accelerator_codegen/stateful/state/partition_key.hpp"
#include "duckdb/common/typedefs.hpp"

namespace accelerator_codegen::aux_state {

struct PartitionSlot {
	enum class Status : uint8_t { Absent = 0, Invalid = 1, Valid = 2 };

	Status status = Status::Absent;
	std::shared_ptr<const void> payload;
	uint64_t partition_version = 0;
	uint64_t valid_through_mutation_epoch = 0;
};

enum class PartitionStatus { Absent, Invalid, Valid };

class PartitionedStateStore {
public:
	PartitionStatus GetStatus(PartitionKey key) const;
	std::shared_ptr<const void> GetReadablePayload(PartitionKey key) const;
	uint64_t GetPartitionVersion(PartitionKey key) const;
	uint64_t GetValidThroughMutationEpoch(PartitionKey key) const;

	void PublishPartition(PartitionKey key, std::shared_ptr<const void> payload, uint64_t valid_through_mutation_epoch);
	bool TryPublishPartition(PartitionKey key, std::shared_ptr<const void> payload, uint64_t valid_through_mutation_epoch,
	                         uint64_t expected_partition_version);

	void InvalidatePartition(PartitionKey key);
	void InvalidateAll();
	duckdb::idx_t ValidPartitionCount() const;
	duckdb::idx_t TotalPartitionSlots() const;
	void ForEachSlot(const std::function<void(PartitionKey key, PartitionStatus status,
	                                          const std::shared_ptr<const void> &payload,
	                                          uint64_t valid_through_mutation_epoch)> &visitor) const;
	void VisitReadablePartitionsInRange(
	    PartitionKey start_partition, PartitionKey end_partition,
	    const std::function<void(PartitionKey key, const std::shared_ptr<const void> &payload)> &visitor) const;

private:
	mutable std::mutex mutex;
	std::unordered_map<PartitionKey, PartitionSlot> slots;
	duckdb::idx_t valid_partition_count = 0;

	static PartitionStatus ToLegacyStatus(PartitionSlot::Status status);
	void AdjustValidCount(PartitionSlot::Status before, PartitionSlot::Status after);
};

} // namespace accelerator_codegen::aux_state
