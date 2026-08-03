#include "accelerator_codegen/sec_edgar_q6_maintenance.hpp"

#include <algorithm>

#include "accelerator_codegen/sec_edgar_q6_incremental.hpp"

namespace accelerator_codegen {

SecEdgarQ6MutationBatchFeatures BuildSecEdgarQ6MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const SecEdgarQ6IncrementalTxnBuffer &buffer, uint64_t rows_touched) {
	SecEdgarQ6MutationBatchFeatures features;
	const auto cheap_rows = buffer.CheapRowsTouched();
	features.rows_touched = cheap_rows > 0 ? cheap_rows : rows_touched;
	features.bulk_mutation = buffer.BulkMutation();
	features.captured_row_op_count = buffer.RowOpCount();
	features.heavy_capture_available =
	    !buffer.IsPoisoned() && !buffer.BulkMutation() && features.captured_row_op_count > 0 &&
	    buffer.PendingLightOpCount() == 0;
	features.incremental_buffer_poisoned = buffer.IsPoisoned();
	features.incremental_buffer_complete = features.heavy_capture_available;
	features.old_new_values_available = features.incremental_buffer_complete;
	features.whole_state_required = plan.scope == aux_state::InvalidationScope::AllPartitions;

	if (plan.IsEmpty() && !buffer.HasPendingChanges() && cheap_rows == 0) {
		features.class_id = SecEdgarQ6MutationClass::Irrelevant;
		features.relevant_columns = false;
		features.incremental_supported = true;
		return features;
	}

	features.relevant_columns = true;
	features.incremental_supported = !features.whole_state_required && !features.incremental_buffer_poisoned;
	features.recent_incremental_success_rate = SecEdgarQ6RecentIncrementalSuccessRate();

	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		features.affected_partitions.assign(plan.partitions.begin(), plan.partitions.end());
	}
	auto extra = buffer.ExtraInvalidationPlan();
	if (extra.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : extra.partitions) {
			if (std::find(features.affected_partitions.begin(), features.affected_partitions.end(), partition) ==
			    features.affected_partitions.end()) {
				features.affected_partitions.push_back(partition);
			}
		}
	}
	features.affected_partition_count = features.affected_partitions.size();

	const auto row_ops = features.captured_row_op_count;
	if (features.affected_partition_count > 1 && row_ops > 0) {
		features.class_id = SecEdgarQ6MutationClass::ShipdateMove;
	} else if (features.rows_touched > 1 || features.bulk_mutation) {
		features.class_id = SecEdgarQ6MutationClass::BulkLineitem;
	} else if (row_ops > 0) {
		features.class_id = SecEdgarQ6MutationClass::PointLineitemUpdate;
	} else if (features.rows_touched > 0) {
		features.class_id = SecEdgarQ6MutationClass::LineitemInsertDelete;
	} else {
		features.class_id = SecEdgarQ6MutationClass::Unknown;
	}
	return features;
}

const char *SecEdgarQ6MutationClassName(SecEdgarQ6MutationClass class_id) {
	switch (class_id) {
	case SecEdgarQ6MutationClass::Irrelevant:
		return "irrelevant";
	case SecEdgarQ6MutationClass::PointLineitemUpdate:
		return "point_lineitem_update";
	case SecEdgarQ6MutationClass::LineitemInsertDelete:
		return "lineitem_insert_delete";
	case SecEdgarQ6MutationClass::ShipdateMove:
		return "shipdate_move";
	case SecEdgarQ6MutationClass::BulkLineitem:
		return "bulk_lineitem";
	case SecEdgarQ6MutationClass::Unknown:
		break;
	}
	return "unknown";
}

} // namespace accelerator_codegen
