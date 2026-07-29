#include "accelerator_codegen/tpch_q6_maintenance.hpp"

#include <algorithm>

#include "accelerator_codegen/tpch_q6_incremental.hpp"

namespace accelerator_codegen {

Q6MutationBatchFeatures BuildQ6MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const Q6IncrementalTxnBuffer &buffer, uint64_t rows_touched) {
	Q6MutationBatchFeatures features;
	const auto cheap_rows = buffer.CheapRowsTouched();
	features.rows_touched = cheap_rows > 0 ? cheap_rows : rows_touched;
	features.bulk_mutation = buffer.BulkMutation();
	features.cross_partition_moves = buffer.CrossPartitionOpCount();
	features.captured_row_op_count = buffer.RowOpCount();
	features.heavy_capture_available = !buffer.IsPoisoned() && !buffer.BulkMutation() && features.captured_row_op_count > 0;
	features.incremental_buffer_poisoned = buffer.IsPoisoned();
	features.incremental_buffer_complete = features.heavy_capture_available;
	features.old_new_values_available = features.incremental_buffer_complete;
	features.whole_state_required = plan.scope == aux_state::InvalidationScope::AllPartitions;

	if (plan.IsEmpty() && !buffer.HasPendingChanges() && cheap_rows == 0) {
		features.class_id = Q6MutationClass::Irrelevant;
		features.relevant_columns = false;
		features.incremental_supported = true;
		return features;
	}

	features.relevant_columns = true;
	features.incremental_supported = !features.whole_state_required && !features.incremental_buffer_poisoned;
	features.recent_incremental_success_rate = Q6RecentIncrementalSuccessRate();
	features.estimated_lineitem_scan_rows = buffer.EstimatedLineitemScanRows();

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
	if (features.whole_state_required) {
		features.class_id = Q6MutationClass::BulkLineitem;
	} else if (features.affected_partition_count > 1 && row_ops > 0) {
		features.class_id = Q6MutationClass::CrossPartitionMove;
	} else if (features.rows_touched > 1 || features.bulk_mutation) {
		features.class_id = Q6MutationClass::BulkLineitem;
	} else if (row_ops > 0 && features.cross_partition_moves > 0) {
		features.class_id = Q6MutationClass::ShipdateMove;
	} else if (row_ops > 0) {
		features.class_id = Q6MutationClass::PointLineitemUpdate;
	} else if (cheap_rows > 0) {
		features.class_id = Q6MutationClass::LineitemInsertDelete;
	} else {
		features.class_id = Q6MutationClass::Unknown;
	}
	return features;
}

const char *Q6MutationClassName(Q6MutationClass class_id) {
	switch (class_id) {
	case Q6MutationClass::Irrelevant:
		return "irrelevant";
	case Q6MutationClass::PointLineitemUpdate:
		return "point_lineitem_update";
	case Q6MutationClass::LineitemInsertDelete:
		return "lineitem_insert_delete";
	case Q6MutationClass::ShipdateMove:
		return "shipdate_move";
	case Q6MutationClass::BulkLineitem:
		return "bulk_lineitem";
	case Q6MutationClass::CrossPartitionMove:
		return "cross_partition_move";
	case Q6MutationClass::Unknown:
		break;
	}
	return "unknown";
}

} // namespace accelerator_codegen
