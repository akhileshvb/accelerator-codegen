#include "accelerator_codegen/tpch_q1_maintenance.hpp"

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
	accelerator_codegen::Q1MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q1MutationClass::PointLineitemUpdate;
	mutation.incremental_supported = true;
	mutation.incremental_buffer_complete = true;
	mutation.old_new_values_available = true;
	mutation.heavy_capture_available = true;
	mutation.affected_partition_count = 1;
	mutation.rows_touched = 1;
	accelerator_codegen::Q1WorkloadFeatures workload;
	const auto &params = accelerator_codegen::GetQ1CostModelParams();
	auto candidates = accelerator_codegen::EnumerateQ1ActionCandidates(mutation, workload, params, true);
	bool apply_supported = false;
	for (const auto &candidate : candidates) {
		if (candidate.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::ApplyDelta) {
			apply_supported = candidate.supported;
		}
	}
	Require(!apply_supported, "low confidence must exclude ApplyDelta");
}

void TestTrainedModeSelectsApplyDelta() {
	accelerator_codegen::LoadQ1CostModelParams();
	accelerator_codegen::Q1MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q1MutationClass::PointLineitemUpdate;
	mutation.incremental_supported = true;
	mutation.incremental_buffer_complete = true;
	mutation.old_new_values_available = true;
	mutation.heavy_capture_available = true;
	mutation.affected_partition_count = 1;
	mutation.rows_touched = 1;
	mutation.recent_incremental_success_rate = 1.0;
	accelerator_codegen::Q1WorkloadFeatures workload;
	workload.expected_queries_before_next_write = 10.0;
	workload.ewma_fallback_latency_us = 50000.0;
	workload.ewma_aux_hit_latency_us = 500.0;
	const auto selection = accelerator_codegen::SelectQ1MaintenanceAction(
	    mutation, workload, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	Require(selection.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::ApplyDelta,
	        "trained mode should select ApplyDelta for cheap point update");
}

void TestBulkLineitemSelectsRebuildPartition() {
	accelerator_codegen::LoadQ1CostModelParams();
	accelerator_codegen::Q1MutationBatchFeatures mutation;
	mutation.class_id = accelerator_codegen::Q1MutationClass::BulkLineitem;
	mutation.bulk_mutation = true;
	mutation.affected_partition_count = 1;
	mutation.affected_partitions.push_back(199506);
	mutation.rows_touched = 245825;
	mutation.recent_incremental_success_rate = 1.0;
	accelerator_codegen::Q1WorkloadFeatures workload;
	workload.expected_queries_before_next_write = 10.0;
	workload.ewma_fallback_latency_us = 53000.0;
	workload.ewma_aux_hit_latency_us = 500.0;
	const auto selection = accelerator_codegen::SelectQ1MaintenanceAction(
	    mutation, workload, accelerator_codegen::aux_state::maintenance::MaintenanceAction::NoOp);
	Require(selection.action == accelerator_codegen::aux_state::maintenance::MaintenanceAction::RebuildPartition,
	        "bulk lineitem should prefer partition rebuild over invalidation");
}

} // namespace

int main() {
	try {
		TestLowConfidenceExcludesApplyDelta();
		TestTrainedModeSelectsApplyDelta();
		TestBulkLineitemSelectsRebuildPartition();
		std::cout << "test_q1_cost_model passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q1_cost_model failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
