#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "duckdb/common/typedefs.hpp"

namespace accelerator_codegen::aux_state {

enum class PartitionStatus { Absent, Invalid, Valid };

struct PartitionSlot {
	PartitionStatus status = PartitionStatus::Absent;
	std::shared_ptr<const void> payload;
};

class PartitionedStateStore {
public:
	PartitionStatus GetStatus(PartitionKey key) const;
	std::shared_ptr<const void> GetReadablePayload(PartitionKey key) const;
	void PublishPartition(PartitionKey key, std::shared_ptr<const void> payload);
	void InvalidatePartition(PartitionKey key);
	void InvalidateAll();
	duckdb::idx_t ValidPartitionCount() const;
	duckdb::idx_t TotalPartitionSlots() const;

private:
	mutable std::mutex mutex;
	std::unordered_map<PartitionKey, PartitionSlot> slots;
};

} // namespace accelerator_codegen::aux_state
