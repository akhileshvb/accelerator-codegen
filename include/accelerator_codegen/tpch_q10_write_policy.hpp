#pragma once

#include <cstdint>
#include <string>

#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"

namespace accelerator_codegen {

struct Q10AuxCounters {
	uint64_t dependent_mutation_events_with_plan = 0;
	uint64_t dependent_mutation_events_empty_plan = 0;
	uint64_t incremental_apply_attempts = 0;
	uint64_t incremental_apply_successes = 0;
	uint64_t incremental_apply_failures = 0;
	uint64_t whole_state_invalidations = 0;
	uint64_t partition_invalidations = 0;
	uint64_t fallback_queries = 0;
	uint64_t auxiliary_hits = 0;
	uint64_t rebuilds = 0;
	uint64_t rebuild_cost_model_skips = 0;
	uint64_t incremental_apply_ns = 0;
	uint64_t hybrid_decisions = 0;
	uint64_t low_confidence_decisions = 0;
	uint64_t noop_decisions = 0;
	uint64_t lazy_rebuild_schedules = 0;
	uint64_t action_selected_noop = 0;
	uint64_t action_selected_apply_delta = 0;
	uint64_t action_selected_rebuild_partition = 0;
	uint64_t action_selected_rebuild_all = 0;
	uint64_t action_selected_invalidate = 0;
	uint64_t prediction_error_samples = 0;
	double prediction_error_sum_us = 0.0;
};

void ResetQ10AuxCounters();
const Q10AuxCounters &GetQ10AuxCounters();

void RecordQ10DependentTableMutation(bool plan_is_empty);
void RecordQ10InvalidationApplied(bool whole_state, uint64_t partition_count);
void RecordQ10IncrementalApplyAttempt(bool success, uint64_t apply_ns);
void RecordQ10AuxQueryOutcome(bool used_aux);
void RecordQ10Rebuild();
void RecordQ10RebuildCostModelSkip();
void RecordQ10HybridActionSelected(aux_state::maintenance::MaintenanceAction action, bool low_confidence);
void RecordQ10HybridPredictionError(double error_us);
void RecordQ10LazyRebuildScheduled();

} // namespace accelerator_codegen
