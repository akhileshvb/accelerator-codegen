#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"

namespace accelerator_codegen::aux_state::maintenance {

struct StagedPartitionUpdate {
	PartitionKey key = 0;
	std::shared_ptr<const void> payload;
	uint64_t expected_partition_version = 0;
};

struct MaintenanceDecisionTrace {
	MaintenanceAction selected = MaintenanceAction::NoOp;
	bool low_confidence = true;
	double selected_predicted_cost_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
};

struct StagedMaintenanceOutcome {
	MaintenanceAction selected = MaintenanceAction::NoOp;
	std::vector<StagedPartitionUpdate> partition_updates;
	InvalidationPlan invalidations;
	InvalidationPlan pending_lazy_rebuild;
	MaintenanceDecisionTrace trace;
	bool active = false;

	void Clear();
};

bool MaintenanceOutcomeHasPublishEffect(const StagedMaintenanceOutcome &outcome);

} // namespace accelerator_codegen::aux_state::maintenance
