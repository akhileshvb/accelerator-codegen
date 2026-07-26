#include "accelerator_codegen/tpch_q14_maintenance.hpp"

#include <algorithm>

#include "accelerator_codegen/tpch_q14_incremental.hpp"

namespace accelerator_codegen {

Q14MutationBatchFeatures BuildQ14MutationBatchFeatures(const aux_state::InvalidationPlan &plan,
                                                       const Q14IncrementalTxnBuffer &buffer, uint64_t rows_touched) {
	Q14MutationBatchFeatures features;
	const auto cheap_rows = buffer.CheapRowsTouched();
	features.rows_touched = cheap_rows > 0 ? cheap_rows : rows_touched;
	features.bulk_mutation = buffer.BulkMutation();
	features.promo_op_count = buffer.PromoOpCount();
	features.captured_row_op_count = buffer.RowOpCount();
	features.heavy_capture_available =
	    !buffer.IsPoisoned() && !buffer.BulkMutation() &&
	    features.captured_row_op_count + features.promo_op_count > 0 && buffer.PendingLightOpCount() == 0;
	features.incremental_buffer_poisoned = buffer.IsPoisoned();
	features.incremental_buffer_complete = features.heavy_capture_available;
	features.old_new_values_available = features.incremental_buffer_complete;
	features.whole_state_required = plan.scope == aux_state::InvalidationScope::AllPartitions;

	if (plan.IsEmpty() && !buffer.HasPendingChanges() && cheap_rows == 0) {
		features.class_id = Q14MutationClass::Irrelevant;
		features.relevant_columns = false;
		features.incremental_supported = true;
		return features;
	}

	features.relevant_columns = true;
	features.incremental_supported = !features.whole_state_required && !features.incremental_buffer_poisoned;
	features.recent_incremental_success_rate = Q14RecentIncrementalSuccessRate();
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

	if (features.whole_state_required && features.affected_partition_count > 0 &&
	    features.affected_partition_count <= 4) {
		features.whole_state_required = false;
		features.incremental_supported = !features.incremental_buffer_poisoned;
	}

	const auto row_ops = features.captured_row_op_count;
	const auto lineitem_scan_rows = features.estimated_lineitem_scan_rows;
	const auto part_key_fanout = buffer.CheapPartFanout();
	const bool part_mutation = part_key_fanout > 0 || features.promo_op_count > 0;
	const bool point_part = part_mutation && features.rows_touched <= 1 && part_key_fanout <= 1;

	if (features.whole_state_required && !part_mutation) {
		features.class_id = Q14MutationClass::BulkPartMutation;
	} else if (!point_part && (part_key_fanout > 1 || (part_key_fanout > 0 && features.rows_touched > 1))) {
		features.class_id = Q14MutationClass::BulkPartMutation;
	} else if (!point_part && lineitem_scan_rows > 1 && part_mutation && !features.bulk_mutation) {
		features.class_id = Q14MutationClass::BulkPartMutation;
	} else if (features.affected_partition_count > 1 && row_ops > 0 && !part_mutation) {
		features.class_id = Q14MutationClass::ShipdateMove;
	} else if ((features.rows_touched > 1 || features.bulk_mutation) && !part_mutation) {
		features.class_id = Q14MutationClass::BulkLineitem;
	} else if (row_ops > 0 && !part_mutation) {
		features.class_id = Q14MutationClass::PointLineitemUpdate;
	} else if (part_mutation && row_ops > 0 && features.promo_op_count == 0) {
		features.class_id = Q14MutationClass::PartInsertDelete;
	} else if (part_mutation || cheap_rows > 0) {
		features.class_id = Q14MutationClass::PointPartTypeChange;
	} else {
		features.class_id = Q14MutationClass::Unknown;
	}

	if (lineitem_scan_rows == 0 && part_key_fanout > 0) {
		features.estimated_lineitem_scan_rows = part_key_fanout;
	}
	if (features.bulk_mutation && features.estimated_lineitem_scan_rows == 0) {
		features.estimated_lineitem_scan_rows = 0;
	}

	if (part_mutation) {
		features.estimated_part_fanout = std::max<uint64_t>(1, part_key_fanout);
	} else if (features.estimated_lineitem_scan_rows > 0) {
		features.estimated_part_fanout = features.estimated_lineitem_scan_rows;
	} else {
		features.estimated_part_fanout = std::max<uint64_t>(row_ops, cheap_rows);
	}
	return features;
}

const char *Q14MutationClassName(Q14MutationClass class_id) {
	switch (class_id) {
	case Q14MutationClass::Irrelevant:
		return "irrelevant";
	case Q14MutationClass::PointLineitemUpdate:
		return "point_lineitem_update";
	case Q14MutationClass::LineitemInsertDelete:
		return "lineitem_insert_delete";
	case Q14MutationClass::ShipdateMove:
		return "shipdate_move";
	case Q14MutationClass::BulkLineitem:
		return "bulk_lineitem";
	case Q14MutationClass::PointPartTypeChange:
		return "point_part_type_change";
	case Q14MutationClass::BulkPartMutation:
		return "bulk_part_mutation";
	case Q14MutationClass::PartInsertDelete:
		return "part_insert_delete";
	case Q14MutationClass::Unknown:
		break;
	}
	return "unknown";
}

} // namespace accelerator_codegen
