#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"

namespace accelerator_codegen::aux_state::maintenance {

struct MaintenanceCounters {
	uint64_t writes_attempted = 0;
	uint64_t writes_committed = 0;
	uint64_t rows_touched = 0;
	uint64_t noop_decisions = 0;
	uint64_t low_confidence_decisions = 0;
	uint64_t action_candidates_considered = 0;
	uint64_t incremental_attempts = 0;
	uint64_t incremental_successes = 0;
	uint64_t incremental_failures = 0;
	uint64_t partition_rebuild_attempts = 0;
	uint64_t partition_rebuild_successes = 0;
	uint64_t full_rebuild_attempts = 0;
	uint64_t full_rebuild_successes = 0;
	uint64_t whole_state_invalidations = 0;
	uint64_t partition_invalidations = 0;
	uint64_t lazy_rebuild_schedules = 0;
	uint64_t prediction_error_samples = 0;
	double prediction_error_sum_us = 0.0;
	uint64_t action_selected[5] = {};
};

struct MaintenanceDecisionRecord {
	MaintenanceAction selected = MaintenanceAction::NoOp;
	bool low_confidence = true;
	double selected_predicted_cost_us = 0.0;
	double actual_cost_us = 0.0;
	double prediction_error_us = 0.0;
	std::vector<std::pair<MaintenanceAction, double>> candidate_costs;
};

void ResetMaintenanceCounters();
const MaintenanceCounters &GetMaintenanceCounters();
void RecordMaintenanceActionSelected(MaintenanceAction action, bool low_confidence);
void RecordMaintenancePredictionError(double error_us);
void AppendMaintenanceDecisionRecord(const MaintenanceDecisionRecord &record);
const std::deque<MaintenanceDecisionRecord> &GetMaintenanceDecisionLog();
void MaybeAppendDecisionCsv(const MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen::aux_state::maintenance
