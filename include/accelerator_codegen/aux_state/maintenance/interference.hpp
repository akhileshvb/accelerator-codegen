#pragma once

#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/maintenance/cost_model.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"

namespace accelerator_codegen::aux_state::maintenance {

struct ActionResourceEstimate {
	double estimated_scan_bytes = 0.0;
	double estimated_duration_us = 0.0;
	double memory_bandwidth_demand = 0.0;
	uint64_t partitions_locked = 0;
	uint64_t expected_overlapping_queries = 0;
};

double EstimateInterferenceUs(MaintenanceAction action, const ActionResourceEstimate &resources,
                              const ExecutionLoadSnapshot &load, const CostModelParams &params);

} // namespace accelerator_codegen::aux_state::maintenance
