#include "accelerator_codegen/tpch_q3_maintenance.hpp"

#include <algorithm>

#include "accelerator_codegen/tpch_q3_incremental.hpp"

namespace accelerator_codegen {

Q3MutationBatchFeatures BuildQ3MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                     const Q3IncrementalTxnBuffer &buffer, uint64_t rows_touched) {
	Q3MutationBatchFeatures features;
	const auto cheap_rows = buffer.CheapRowsTouched();
	features.rows_touched = cheap_rows > 0 ? cheap_rows : rows_touched;
	features.bulk_mutation = buffer.BulkMutation();
	features.cross_partition_moves = buffer.CrossPartitionOpCount();
	features.captured_row_op_count = buffer.RowOpCount();
	features.heavy_capture_available = !buffer.IsPoisoned() && !buffer.BulkMutation() &&
	                                   features.captured_row_op_count + features.cross_partition_moves > 0 &&
	                                   buffer.PendingLightOpCount() == 0;
	features.incremental_buffer_poisoned = buffer.IsPoisoned();
	features.incremental_buffer_complete = features.heavy_capture_available;
	features.old_new_values_available = features.incremental_buffer_complete;
	features.whole_state_required = plan.scope == aux_state::InvalidationScope::AllPartitions;

	if (plan.IsEmpty() && !buffer.HasPendingChanges() && cheap_rows == 0) {
		features.class_id = Q3MutationClass::Irrelevant;
		features.relevant_columns = false;
		features.incremental_supported = true;
		return features;
	}

	features.relevant_columns = true;
	features.incremental_supported = !features.whole_state_required && !features.incremental_buffer_poisoned;
	features.recent_incremental_success_rate = Q3RecentIncrementalSuccessRate();
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
	const auto order_fanout = buffer.CheapOrderFanout();
	const bool customer_mutation = order_fanout > 0 || features.cross_partition_moves > 0;
	const bool point_customer = customer_mutation && features.rows_touched <= 1 && order_fanout <= 1;

	if (features.whole_state_required && !customer_mutation) {
		features.class_id = Q3MutationClass::BulkCustomerMutation;
	} else if (!point_customer && (order_fanout > 1 || (order_fanout > 0 && features.rows_touched > 1))) {
		features.class_id = Q3MutationClass::BulkCustomerMutation;
	} else if (features.affected_partition_count > 1 && row_ops > 0) {
		features.class_id = Q3MutationClass::CrossPartitionMove;
	} else if ((features.rows_touched > 1 || features.bulk_mutation) && !customer_mutation) {
		features.class_id = Q3MutationClass::BulkLineitem;
	} else if (row_ops > 0 && !customer_mutation) {
		features.class_id = Q3MutationClass::PointLineitemUpdate;
	} else if (customer_mutation && row_ops > 0 && features.cross_partition_moves == 0) {
		features.class_id = Q3MutationClass::OrderInsertDelete;
	} else if (customer_mutation || cheap_rows > 0) {
		features.class_id = Q3MutationClass::CustomerSegmentChange;
	} else {
		features.class_id = Q3MutationClass::Unknown;
	}

	if (customer_mutation) {
		features.estimated_order_fanout = std::max<uint64_t>(1, order_fanout);
	} else if (features.estimated_lineitem_scan_rows > 0) {
		features.estimated_order_fanout = features.estimated_lineitem_scan_rows;
	} else {
		features.estimated_order_fanout = std::max<uint64_t>(row_ops, cheap_rows);
	}
	return features;
}

const char *Q3MutationClassName(Q3MutationClass class_id) {
	switch (class_id) {
	case Q3MutationClass::Irrelevant:
		return "irrelevant";
	case Q3MutationClass::PointLineitemUpdate:
		return "point_lineitem_update";
	case Q3MutationClass::LineitemInsertDelete:
		return "lineitem_insert_delete";
	case Q3MutationClass::ShipdateMove:
		return "shipdate_move";
	case Q3MutationClass::BulkLineitem:
		return "bulk_lineitem";
	case Q3MutationClass::OrderInsertDelete:
		return "order_insert_delete";
	case Q3MutationClass::OrderMetadataUpdate:
		return "order_metadata_update";
	case Q3MutationClass::CustomerSegmentChange:
		return "customer_segment_change";
	case Q3MutationClass::BulkCustomerMutation:
		return "bulk_customer_mutation";
	case Q3MutationClass::CrossPartitionMove:
		return "cross_partition_move";
	case Q3MutationClass::Unknown:
		break;
	}
	return "unknown";
}

} // namespace accelerator_codegen
