#include "accelerator_codegen/tpch_q14_maintenance.hpp"

#include <iostream>
#include <stdexcept>

#include "accelerator_codegen/aux_state/maintenance/action_selector.hpp"

namespace {

void Require(bool condition, const char *message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void TestLowConfidenceExcludesApplyDelta() {
	accelerator_codegen::Q14MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q14MutationClass::PointLineitemUpdate;
	mutation.incremental_supported = true;
	mutation.incremental_buffer_complete = true;
	mutation.old_new_values_available = true;
	mutation.heavy_capture_available = true;
	mutation.affected_partition_count = 1;
	mutation.rows_touched = 1;
	accelerator_codegen::Q14WorkloadFeatures workload;
	const auto &params = accelerator_codegen::GetQ14CostModelParams();
	auto candidates = accelerator_codegen::EnumerateQ14ActionCandidates(mutation, workload, params, true);
	bool apply_supported = false;
	for (const auto &candidate : candidates) {
		if (candidate.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::ApplyDelta) {
			apply_supported = candidate.supported;
		}
	}
	Require(!apply_supported, "low confidence must exclude ApplyDelta");
}

void TestTrainedModeSelectsApplyDelta() {
	accelerator_codegen::LoadQ14CostModelParams();
	accelerator_codegen::Q14MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q14MutationClass::PointLineitemUpdate;
	mutation.incremental_supported = true;
	mutation.incremental_buffer_complete = true;
	mutation.old_new_values_available = true;
	mutation.heavy_capture_available = true;
	mutation.affected_partition_count = 1;
	mutation.rows_touched = 1;
	mutation.recent_incremental_success_rate = 1.0;
	accelerator_codegen::Q14WorkloadFeatures workload;
	workload.expected_queries_before_next_write = 10.0;
	workload.ewma_fallback_latency_us = 50000.0;
	workload.ewma_aux_hit_latency_us = 500.0;
	const auto selection = accelerator_codegen::SelectQ14MaintenanceAction(
	    mutation, workload, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	Require(selection.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::ApplyDelta,
	        "trained mode should select ApplyDelta for cheap point update");
}

void TestBulkLineitemSelectsRebuildPartition() {
	accelerator_codegen::LoadQ14CostModelParams();
	accelerator_codegen::Q14MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q14MutationClass::BulkLineitem;
	mutation.bulk_mutation = true;
	mutation.affected_partition_count = 1;
	mutation.affected_partitions.push_back(199506);
	mutation.rows_touched = 245825;
	mutation.recent_incremental_success_rate = 1.0;
	accelerator_codegen::Q14WorkloadFeatures workload;
	workload.expected_queries_before_next_write = 10.0;
	workload.ewma_fallback_latency_us = 53000.0;
	workload.ewma_aux_hit_latency_us = 500.0;
	const auto selection = accelerator_codegen::SelectQ14MaintenanceAction(
	    mutation, workload, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	Require(selection.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::RebuildPartition,
	        "bulk lineitem should prefer partition rebuild over invalidation");
}

void TestBulkPartExcludesApplyDelta() {
	accelerator_codegen::LoadQ14CostModelParams();
	accelerator_codegen::Q14MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q14MutationClass::BulkPartMutation;
	mutation.rows_touched = 50;
	mutation.estimated_part_fanout = 50;
	mutation.estimated_lineitem_scan_rows = 50;
	mutation.recent_incremental_success_rate = 1.0;
	accelerator_codegen::Q14WorkloadFeatures workload;
	const auto &params = accelerator_codegen::GetQ14CostModelParams();
	auto details = accelerator_codegen::EnumerateQ14ActionCandidatesDetailed(mutation, workload, params, false);
	bool apply_supported = false;
	for (const auto &candidate : details) {
		if (candidate.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::ApplyDelta) {
			apply_supported = candidate.supported;
		}
	}
	Require(!apply_supported, "bulk part fan-out should exclude ApplyDelta");
}

void TestBulkPartSelectsRebuildAll() {
	accelerator_codegen::LoadQ14CostModelParams();
	accelerator_codegen::Q14MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q14MutationClass::BulkPartMutation;
	mutation.rows_touched = 50;
	mutation.estimated_part_fanout = 50;
	mutation.estimated_lineitem_scan_rows = 50;
	mutation.recent_incremental_success_rate = 1.0;
	accelerator_codegen::Q14WorkloadFeatures workload;
	workload.expected_queries_before_next_write = 100.0;
	workload.ewma_fallback_latency_us = 55000.0;
	workload.ewma_aux_hit_latency_us = 500.0;
	const auto selection = accelerator_codegen::SelectQ14MaintenanceAction(
	    mutation, workload, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	Require(selection.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::RebuildAll,
	        "bulk part should prefer full rebuild over invalidation");
}

void TestPointPartSelectsApplyDelta() {
	accelerator_codegen::LoadQ14CostModelParams();
	accelerator_codegen::Q14MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q14MutationClass::PointPartTypeChange;
	mutation.incremental_supported = true;
	mutation.incremental_buffer_complete = true;
	mutation.old_new_values_available = true;
	mutation.heavy_capture_available = true;
	mutation.promo_op_count = 12;
	mutation.captured_row_op_count = 0;
	mutation.estimated_part_fanout = 1;
	mutation.estimated_lineitem_scan_rows = 240;
	mutation.affected_partition_count = 12;
	mutation.rows_touched = 1;
	mutation.recent_incremental_success_rate = 1.0;
	accelerator_codegen::Q14WorkloadFeatures workload;
	workload.expected_queries_before_next_write = 100.0;
	workload.ewma_fallback_latency_us = 55000.0;
	workload.ewma_aux_hit_latency_us = 500.0;
	const auto selection = accelerator_codegen::SelectQ14MaintenanceAction(
	    mutation, workload, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	Require(selection.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::ApplyDelta,
	        "point part promo flip should prefer ApplyDelta over rebuild/invalidate");
}

} // namespace

int main() {
	try {
		TestLowConfidenceExcludesApplyDelta();
		TestTrainedModeSelectsApplyDelta();
		TestBulkLineitemSelectsRebuildPartition();
		TestBulkPartExcludesApplyDelta();
		TestBulkPartSelectsRebuildAll();
		TestPointPartSelectsApplyDelta();
		std::cout << "test_q14_cost_model passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q14_cost_model failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
