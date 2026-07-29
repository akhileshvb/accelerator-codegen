#include "accelerator_codegen/tpch_q3_write_policy.hpp"

#include "accelerator_codegen/tpch_q3_rebuild_policy.hpp"

namespace accelerator_codegen {
namespace {

Q3AuxCounters g_q3_aux_counters;

} // namespace

void ResetQ3AuxCounters() {
	g_q3_aux_counters = Q3AuxCounters {};
	ResetQ3RebuildPolicyTiming();
}

const Q3AuxCounters &GetQ3AuxCounters() {
	return g_q3_aux_counters;
}

void RecordQ3DependentTableMutation(bool plan_is_empty) {
	if (plan_is_empty) {
		g_q3_aux_counters.dependent_mutation_events_empty_plan++;
	} else {
		g_q3_aux_counters.dependent_mutation_events_with_plan++;
	}
}

void RecordQ3InvalidationApplied(bool whole_state, uint64_t partition_count) {
	if (whole_state) {
		g_q3_aux_counters.whole_state_invalidations++;
		return;
	}
	g_q3_aux_counters.partition_invalidations += partition_count;
}

void RecordQ3IncrementalApplyAttempt(bool success, uint64_t apply_ns) {
	g_q3_aux_counters.incremental_apply_attempts++;
	g_q3_aux_counters.incremental_apply_ns += apply_ns;
	if (success) {
		g_q3_aux_counters.incremental_apply_successes++;
	} else {
		g_q3_aux_counters.incremental_apply_failures++;
	}
}

void RecordQ3AuxQueryOutcome(bool used_aux) {
	if (used_aux) {
		g_q3_aux_counters.auxiliary_hits++;
	} else {
		g_q3_aux_counters.fallback_queries++;
	}
}

void RecordQ3Rebuild() {
	g_q3_aux_counters.rebuilds++;
}

void RecordQ3RebuildCostModelSkip() {
	g_q3_aux_counters.rebuild_cost_model_skips++;
}

void RecordQ3HybridActionSelected(aux_state::maintenance::MaintenanceAction action, bool low_confidence) {
	g_q3_aux_counters.hybrid_decisions++;
	if (low_confidence) {
		g_q3_aux_counters.low_confidence_decisions++;
	}
	switch (action) {
	case aux_state::maintenance::MaintenanceAction::NoOp:
		g_q3_aux_counters.action_selected_noop++;
		break;
	case aux_state::maintenance::MaintenanceAction::ApplyDelta:
		g_q3_aux_counters.action_selected_apply_delta++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildPartition:
		g_q3_aux_counters.action_selected_rebuild_partition++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildAll:
		g_q3_aux_counters.action_selected_rebuild_all++;
		break;
	case aux_state::maintenance::MaintenanceAction::InvalidateAndFallback:
		g_q3_aux_counters.action_selected_invalidate++;
		break;
	}
}

void RecordQ3HybridPredictionError(double error_us) {
	g_q3_aux_counters.prediction_error_samples++;
	g_q3_aux_counters.prediction_error_sum_us += error_us;
}

void RecordQ3LazyRebuildScheduled() {
	g_q3_aux_counters.lazy_rebuild_schedules++;
}

} // namespace accelerator_codegen
