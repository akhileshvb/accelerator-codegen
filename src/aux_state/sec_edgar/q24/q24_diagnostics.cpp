#include "accelerator_codegen/sec_edgar_q24_write_policy.hpp"

#include "accelerator_codegen/sec_edgar_q24_rebuild_policy.hpp"

namespace accelerator_codegen {
namespace {

SecEdgarQ24AuxCounters g_q24_aux_counters;

} // namespace

void ResetSecEdgarQ24AuxCounters() {
	g_q24_aux_counters = SecEdgarQ24AuxCounters {};
	ResetSecEdgarQ24RebuildPolicyTiming();
}

const SecEdgarQ24AuxCounters &GetSecEdgarQ24AuxCounters() {
	return g_q24_aux_counters;
}

void RecordSecEdgarQ24DependentTableMutation(bool plan_is_empty) {
	if (plan_is_empty) {
		g_q24_aux_counters.dependent_mutation_events_empty_plan++;
	} else {
		g_q24_aux_counters.dependent_mutation_events_with_plan++;
	}
}

void RecordSecEdgarQ24InvalidationApplied(bool whole_state, uint64_t partition_count) {
	if (whole_state) {
		g_q24_aux_counters.whole_state_invalidations++;
		return;
	}
	g_q24_aux_counters.partition_invalidations += partition_count;
}

void RecordSecEdgarQ24IncrementalApplyAttempt(bool success, uint64_t apply_ns) {
	g_q24_aux_counters.incremental_apply_attempts++;
	g_q24_aux_counters.incremental_apply_ns += apply_ns;
	if (success) {
		g_q24_aux_counters.incremental_apply_successes++;
	} else {
		g_q24_aux_counters.incremental_apply_failures++;
	}
}

void RecordSecEdgarQ24AuxQueryOutcome(bool used_aux) {
	if (used_aux) {
		g_q24_aux_counters.auxiliary_hits++;
	} else {
		g_q24_aux_counters.fallback_queries++;
	}
}

void RecordSecEdgarQ24Rebuild() {
	g_q24_aux_counters.rebuilds++;
}

void RecordSecEdgarQ24RebuildCostModelSkip() {
	g_q24_aux_counters.rebuild_cost_model_skips++;
}

void RecordSecEdgarQ24HybridActionSelected(aux_state::maintenance::MaintenanceAction action, bool low_confidence) {
	g_q24_aux_counters.hybrid_decisions++;
	if (low_confidence) {
		g_q24_aux_counters.low_confidence_decisions++;
	}
	switch (action) {
	case aux_state::maintenance::MaintenanceAction::NoOp:
		g_q24_aux_counters.action_selected_noop++;
		break;
	case aux_state::maintenance::MaintenanceAction::ApplyDelta:
		g_q24_aux_counters.action_selected_apply_delta++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildPartition:
		g_q24_aux_counters.action_selected_rebuild_partition++;
		break;
	case aux_state::maintenance::MaintenanceAction::RebuildAll:
		g_q24_aux_counters.action_selected_rebuild_all++;
		break;
	case aux_state::maintenance::MaintenanceAction::InvalidateAndFallback:
		g_q24_aux_counters.action_selected_invalidate++;
		break;
	}
}

void RecordSecEdgarQ24HybridPredictionError(double error_us) {
	g_q24_aux_counters.prediction_error_samples++;
	g_q24_aux_counters.prediction_error_sum_us += error_us;
}

void RecordSecEdgarQ24LazyRebuildScheduled() {
	g_q24_aux_counters.lazy_rebuild_schedules++;
}

} // namespace accelerator_codegen
