// SCAFFOLDING
#pragma once

#include <cstdint>
#include <string>

namespace accelerator_codegen::aux_state::maintenance {

struct CoreSelectorParams {
	double decision_margin_us = 1000.0;
	uint64_t min_samples = 8;
};

struct ActionCostEstimate {
	double immediate_us = 0.0;
	double differential_future_query_us = 0.0;
	double interference_us = 0.0;

	double TotalOverheadUs() const {
		return immediate_us + differential_future_query_us + interference_us;
	}
};

// Per-accelerator cost-model coefficients. Selector thresholds are also exposed via CoreSelectorParams.
struct CostModelParams {
	double aux_hit_us = 500.0;
	double fallback_us = 50000.0;
	double partition_rebuild_us = 60000.0;
	double full_rebuild_us = 261000.0;
	double first_query_blocking_us = 0.0;
	double apply_delta_overhead_us = 100.0;
	double apply_delta_base_us = 0.0;
	double apply_delta_per_row_us = 0.5;
	double apply_delta_part_lineitem_scan_us = 27000.0;
	uint64_t apply_delta_max_rows = 1048576;
	uint64_t apply_delta_max_part_fanout = 16;
	double invalidation_overhead_us = 10.0;
	double decision_margin_us = 1000.0;
	uint64_t min_samples = 8;
	double incremental_failure_prob = 0.01;

	double interference_rebuild_per_query_us = 0.0;
	double interference_rebuild_scan_byte_us = 0.0;
	double interference_delta_per_query_us = 0.0;
	double interference_invalidate_per_query_us = 0.0;
	double interference_query_slowdown_factor = 0.0;

	CoreSelectorParams SelectorParams() const {
		return CoreSelectorParams {decision_margin_us, min_samples};
	}
};

} // namespace accelerator_codegen::aux_state::maintenance
