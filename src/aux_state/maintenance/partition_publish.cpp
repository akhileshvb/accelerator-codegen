#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"

namespace accelerator_codegen::aux_state::maintenance {

void PublishValidatedPartitionUpdates(PartitionedStateStore &store, const std::vector<StagedPartitionUpdate> &updates,
                                      uint64_t valid_through_mutation_epoch) {
	for (const auto &update : updates) {
		if (!update.payload) {
			store.InvalidatePartition(update.key);
			continue;
		}
		if (update.expected_partition_version == 0) {
			store.PublishPartition(update.key, update.payload, valid_through_mutation_epoch);
			continue;
		}
		store.TryPublishPartition(update.key, update.payload, valid_through_mutation_epoch,
		                        update.expected_partition_version);
	}
}

} // namespace accelerator_codegen::aux_state::maintenance
