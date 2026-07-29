#include "accelerator_codegen/tpch_q12_write_policy.hpp"

#include "accelerator_codegen/tpch_q12_rebuild_policy.hpp"

namespace accelerator_codegen {
namespace {

Q12AuxCounters g_q12_aux_counters;

} // namespace

void ResetQ12AuxCounters() {
	g_q12_aux_counters = Q12AuxCounters {};
	ResetQ12RebuildPolicyTiming();
}

const Q12AuxCounters &GetQ12AuxCounters() {
	return g_q12_aux_counters;
}

void RecordQ12DependentTableMutation(bool plan_is_empty) {
	if (plan_is_empty) {
		g_q12_aux_counters.dependent_mutation_events_empty_plan++;
	} else {
		g_q12_aux_counters.dependent_mutation_events_with_plan++;
	}
}

void RecordQ12InvalidationApplied(bool whole_state, uint64_t partition_count) {
	if (whole_state) {
		g_q12_aux_counters.whole_state_invalidations++;
		return;
	}
	g_q12_aux_counters.partition_invalidations += partition_count;
}

void RecordQ12IncrementalApplyAttempt(bool success, uint64_t apply_ns) {
	g_q12_aux_counters.incremental_apply_attempts++;
	g_q12_aux_counters.incremental_apply_ns += apply_ns;
	if (success) {
		g_q12_aux_counters.incremental_apply_successes++;
	} else {
		g_q12_aux_counters.incremental_apply_failures++;
	}
}

void RecordQ12AuxQueryOutcome(bool used_aux) {
	if (used_aux) {
		g_q12_aux_counters.auxiliary_hits++;
	} else {
		g_q12_aux_counters.fallback_queries++;
	}
}

void RecordQ12Rebuild() {
	g_q12_aux_counters.rebuilds++;
}

void RecordQ12RebuildCostModelSkip() {
	g_q12_aux_counters.rebuild_cost_model_skips++;
}

void RecordQ12HybridActionSelected(aux_state::maintenance::MaintenanceAction action, bool low_confidence) {
	g_q12_aux_counters.hybrid_decisions++;
	if (low_confidence) {
		g_q12_aux_counters.low_confidence_decisions++;
	}
	switch (action) {
	case aux_state::maintenance::MaintenanceAction::NoOp:
		g_q12_aux_counters.action_selected_noop++;
		break;
	case aux_state::maintenance::MaintenanceAction::ApplyDelta:
		g_q12_aux_counters.action_selected_apply_delta++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildPartition:
		g_q12_aux_counters.action_selected_rebuild_partition++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildAll:
		g_q12_aux_counters.action_selected_rebuild_all++;
		break;
	case aux_state::maintenance::MaintenanceAction::InvalidateAndFallback:
		g_q12_aux_counters.action_selected_invalidate++;
		break;
	}
}

void RecordQ12HybridPredictionError(double error_us) {
	g_q12_aux_counters.prediction_error_samples++;
	g_q12_aux_counters.prediction_error_sum_us += error_us;
}

void RecordQ12LazyRebuildScheduled() {
	g_q12_aux_counters.lazy_rebuild_schedules++;
}

} // namespace accelerator_codegen
