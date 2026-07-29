#include "accelerator_codegen/aux_state/maintenance/interference.hpp"

#include <algorithm>
#include <cmath>

namespace accelerator_codegen::aux_state::maintenance {

namespace {

uint32_t OverlappingQueries(const ExecutionLoadSnapshot &load) {
	return std::max<uint32_t>(1, load.total_active) - 1;
}

} // namespace

double EstimateInterferenceUs(MaintenanceAction action, const ActionResourceEstimate &resources,
                              const ExecutionLoadSnapshot &load, const CostModelParams &params) {
	const auto overlapping = resources.expected_overlapping_queries > 0 ? resources.expected_overlapping_queries
	                                                                      : OverlappingQueries(load);
	if (overlapping == 0) {
		return 0.0;
	}

	double interference = 0.0;
	switch (action) {
	case MaintenanceAction::RebuildPartition:
	case MaintenanceAction::RebuildAll:
		interference += params.interference_rebuild_per_query_us * static_cast<double>(overlapping);
		interference += params.interference_rebuild_scan_byte_us * resources.estimated_scan_bytes;
		interference += params.interference_query_slowdown_factor * resources.estimated_duration_us *
		                static_cast<double>(overlapping);
		break;
	case MaintenanceAction::ApplyDelta:
		interference += params.interference_delta_per_query_us * static_cast<double>(overlapping);
		break;
	case MaintenanceAction::InvalidateAndFallback:
		interference += params.interference_invalidate_per_query_us * static_cast<double>(overlapping);
		break;
	case MaintenanceAction::NoOp:
		break;
	}

	if (load.active_rebuilds > 0 &&
	    (action == MaintenanceAction::InvalidateAndFallback || action == MaintenanceAction::ApplyDelta)) {
		interference += params.interference_query_slowdown_factor * static_cast<double>(load.active_rebuilds);
	}

	(void)resources.memory_bandwidth_demand;
	(void)resources.partitions_locked;
	return std::max(0.0, interference);
}

} // namespace accelerator_codegen::aux_state::maintenance
