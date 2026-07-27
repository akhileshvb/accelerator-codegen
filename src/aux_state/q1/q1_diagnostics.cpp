#include "accelerator_codegen/tpch_q1_write_policy.hpp"

#include "accelerator_codegen/tpch_q1_rebuild_policy.hpp"

namespace accelerator_codegen {
namespace {

Q1AuxCounters g_q1_aux_counters;

} // namespace

void ResetQ1AuxCounters() {
	g_q1_aux_counters = Q1AuxCounters {};
	ResetQ1RebuildPolicyTiming();
}

const Q1AuxCounters &GetQ1AuxCounters() {
	return g_q1_aux_counters;
}

void RecordQ1DependentTableMutation(bool plan_is_empty) {
	if (plan_is_empty) {
		g_q1_aux_counters.dependent_mutation_events_empty_plan++;
	} else {
		g_q1_aux_counters.dependent_mutation_events_with_plan++;
	}
}

void RecordQ1InvalidationApplied(bool whole_state, uint64_t partition_count) {
	if (whole_state) {
		g_q1_aux_counters.whole_state_invalidations++;
		return;
	}
	g_q1_aux_counters.partition_invalidations += partition_count;
}

void RecordQ1IncrementalApplyAttempt(bool success, uint64_t apply_ns) {
	g_q1_aux_counters.incremental_apply_attempts++;
	g_q1_aux_counters.incremental_apply_ns += apply_ns;
	if (success) {
		g_q1_aux_counters.incremental_apply_successes++;
	} else {
		g_q1_aux_counters.incremental_apply_failures++;
	}
}

void RecordQ1AuxQueryOutcome(bool used_aux) {
	if (used_aux) {
		g_q1_aux_counters.auxiliary_hits++;
	} else {
		g_q1_aux_counters.fallback_queries++;
	}
}

void RecordQ1Rebuild() {
	g_q1_aux_counters.rebuilds++;
}

void RecordQ1RebuildCostModelSkip() {
	g_q1_aux_counters.rebuild_cost_model_skips++;
}

void RecordQ1HybridActionSelected(aux_state::maintenance::MaintenanceAction action, bool low_confidence) {
	g_q1_aux_counters.hybrid_decisions++;
	if (low_confidence) {
		g_q1_aux_counters.low_confidence_decisions++;
	}
	switch (action) {
	case aux_state::maintenance::MaintenanceAction::NoOp:
		g_q1_aux_counters.action_selected_noop++;
		break;
	case aux_state::maintenance::MaintenanceAction::ApplyDelta:
		g_q1_aux_counters.action_selected_apply_delta++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildPartition:
		g_q1_aux_counters.action_selected_rebuild_partition++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildAll:
		g_q1_aux_counters.action_selected_rebuild_all++;
		break;
	case aux_state::maintenance::MaintenanceAction::InvalidateAndFallback:
		g_q1_aux_counters.action_selected_invalidate++;
		break;
	}
}

void RecordQ1HybridPredictionError(double error_us) {
	g_q1_aux_counters.prediction_error_samples++;
	g_q1_aux_counters.prediction_error_sum_us += error_us;
}

void RecordQ1LazyRebuildScheduled() {
	g_q1_aux_counters.lazy_rebuild_schedules++;
}

} // namespace accelerator_codegen
