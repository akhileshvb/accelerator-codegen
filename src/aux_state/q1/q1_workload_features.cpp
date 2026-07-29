#include "accelerator_codegen/tpch_q1_maintenance.hpp"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>

#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/maintenance/online_stats.hpp"
#include "accelerator_codegen/aux_state/maintenance/json_params.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"

namespace accelerator_codegen {
namespace {

std::mutex g_workload_mutex;
aux_state::maintenance::CostModelParams g_cost_params;
aux_state::maintenance::EwmaStat g_aux_hit_us;
aux_state::maintenance::EwmaStat g_fallback_us;
aux_state::maintenance::EwmaStat g_partition_rebuild_us;
aux_state::maintenance::EwmaStat g_full_rebuild_us;
aux_state::maintenance::EwmaStat g_first_query_blocking_us;
aux_state::maintenance::EwmaStat g_apply_delta_overhead_us;
aux_state::maintenance::RateEwma g_query_rate;
aux_state::maintenance::RateEwma g_write_rate;
aux_state::maintenance::EwmaStat g_incremental_success_rate;
std::chrono::steady_clock::time_point g_last_query_time {};
std::chrono::steady_clock::time_point g_last_write_time {};
bool g_has_query_time = false;
bool g_has_write_time = false;
uint64_t g_queries_since_last_write = 0;
std::deque<Q1MaintenanceDecisionRecord> g_decision_log;
constexpr size_t kMaxDecisionLog = 256;

using aux_state::maintenance::ParseJsonDouble;
using aux_state::maintenance::ParseJsonUint64;

void ObserveInterval(aux_state::maintenance::RateEwma &rate, std::chrono::steady_clock::time_point &last_time,
                     bool &has_time) {
	const auto now = std::chrono::steady_clock::now();
	if (has_time) {
		const auto us = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(now - last_time).count());
		rate.ObserveIntervalUs(us);
	}
	last_time = now;
	has_time = true;
}

} // namespace

const aux_state::maintenance::CostModelParams &GetQ1CostModelParams() {
	return g_cost_params;
}

void LoadQ1CostModelParams() {
	const char *path = std::getenv("ACCEL_Q1_COST_MODEL_PATH");
	if (!path || !*path) {
		path = "config/q1_aux_cost_model.json";
	}
	std::ifstream in(path);
	if (in) {
		std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		g_cost_params.aux_hit_us = ParseJsonDouble(content, "aux_hit_us", g_cost_params.aux_hit_us);
		g_cost_params.fallback_us = ParseJsonDouble(content, "fallback_us", g_cost_params.fallback_us);
		g_cost_params.partition_rebuild_us =
		    ParseJsonDouble(content, "partition_rebuild_us", g_cost_params.partition_rebuild_us);
		g_cost_params.full_rebuild_us = ParseJsonDouble(content, "full_rebuild_us", g_cost_params.full_rebuild_us);
		g_cost_params.first_query_blocking_us =
		    ParseJsonDouble(content, "first_query_blocking_us", g_cost_params.first_query_blocking_us);
		g_cost_params.apply_delta_overhead_us =
		    ParseJsonDouble(content, "apply_delta_overhead_us", g_cost_params.apply_delta_overhead_us);
		g_cost_params.apply_delta_base_us = ParseJsonDouble(content, "apply_delta_base_us", g_cost_params.apply_delta_base_us);
		g_cost_params.apply_delta_per_row_us =
		    ParseJsonDouble(content, "apply_delta_per_row_us", g_cost_params.apply_delta_per_row_us);
		g_cost_params.apply_delta_part_lineitem_scan_us = ParseJsonDouble(
		    content, "apply_delta_part_lineitem_scan_us", g_cost_params.apply_delta_part_lineitem_scan_us);
		g_cost_params.apply_delta_max_rows =
		    ParseJsonUint64(content, "apply_delta_max_rows", g_cost_params.apply_delta_max_rows);
		g_cost_params.apply_delta_max_part_fanout =
		    ParseJsonUint64(content, "apply_delta_max_part_fanout", g_cost_params.apply_delta_max_part_fanout);
		g_cost_params.invalidation_overhead_us =
		    ParseJsonDouble(content, "invalidation_overhead_us", g_cost_params.invalidation_overhead_us);
		g_cost_params.decision_margin_us = ParseJsonDouble(content, "decision_margin_us", g_cost_params.decision_margin_us);
		g_cost_params.min_samples = ParseJsonUint64(content, "min_samples", g_cost_params.min_samples);
		g_cost_params.incremental_failure_prob =
		    ParseJsonDouble(content, "incremental_failure_prob", g_cost_params.incremental_failure_prob);
		g_cost_params.interference_rebuild_per_query_us =
		    ParseJsonDouble(content, "interference_rebuild_per_query_us", g_cost_params.interference_rebuild_per_query_us);
		g_cost_params.interference_rebuild_scan_byte_us =
		    ParseJsonDouble(content, "interference_rebuild_scan_byte_us", g_cost_params.interference_rebuild_scan_byte_us);
		g_cost_params.interference_delta_per_query_us =
		    ParseJsonDouble(content, "interference_delta_per_query_us", g_cost_params.interference_delta_per_query_us);
		g_cost_params.interference_invalidate_per_query_us = ParseJsonDouble(
		    content, "interference_invalidate_per_query_us", g_cost_params.interference_invalidate_per_query_us);
		g_cost_params.interference_query_slowdown_factor =
		    ParseJsonDouble(content, "interference_query_slowdown_factor", g_cost_params.interference_query_slowdown_factor);
	}

	g_aux_hit_us.Observe(g_cost_params.aux_hit_us);
	g_fallback_us.Observe(g_cost_params.fallback_us);
	g_partition_rebuild_us.Observe(g_cost_params.partition_rebuild_us);
	g_full_rebuild_us.Observe(g_cost_params.full_rebuild_us);
	g_first_query_blocking_us.Observe(g_cost_params.first_query_blocking_us);
	g_apply_delta_overhead_us.Observe(g_cost_params.apply_delta_overhead_us);
	for (uint64_t i = 0; i < g_cost_params.min_samples; i++) {
		g_incremental_success_rate.Observe(1.0);
	}
}

void RecordQ1WorkloadQuery() {
	std::lock_guard<std::mutex> lock(g_workload_mutex);
	g_queries_since_last_write++;
	ObserveInterval(g_query_rate, g_last_query_time, g_has_query_time);
}

void RecordQ1WorkloadRelevantWrite() {
	std::lock_guard<std::mutex> lock(g_workload_mutex);
	ObserveInterval(g_write_rate, g_last_write_time, g_has_write_time);
}

void ResetQ1QueriesSinceLastWrite() {
	std::lock_guard<std::mutex> lock(g_workload_mutex);
	g_queries_since_last_write = 0;
}

void RecordQ1LazyRebuildObservation(uint64_t rebuild_scan_us, uint64_t total_stall_us) {
	std::lock_guard<std::mutex> lock(g_workload_mutex);
	g_partition_rebuild_us.Observe(static_cast<double>(rebuild_scan_us));
	const auto blocking = rebuild_scan_us > total_stall_us ? 0ULL : total_stall_us - rebuild_scan_us;
	g_first_query_blocking_us.Observe(static_cast<double>(blocking));
}

Q1WorkloadFeatures BuildQ1WorkloadFeatures(aux_state::PartitionedStateStore &store) {
	Q1WorkloadFeatures features;
	const auto &params = GetQ1CostModelParams();
	features.q1_query_rate_eps = g_query_rate.events_per_second;
	features.relevant_write_rate_eps = g_write_rate.events_per_second;
	if (g_write_rate.events_per_second > 0.0) {
		features.ewma_inter_write_interval_us = 1000000.0 / g_write_rate.events_per_second;
		features.expected_queries_before_next_write =
		    g_query_rate.events_per_second / g_write_rate.events_per_second;
	} else if (g_query_rate.events_per_second > 0.0) {
		features.expected_queries_before_next_write = 1.0;
	} else {
		features.expected_queries_before_next_write = 1.0;
	}
	if (g_queries_since_last_write > 0) {
		features.expected_queries_before_next_write =
		    std::max(features.expected_queries_before_next_write, static_cast<double>(g_queries_since_last_write));
	}
	features.invalid_partition_count = store.TotalPartitionSlots() - store.ValidPartitionCount();
	features.ewma_aux_hit_latency_us =
	    g_aux_hit_us.HasConfidence(params.min_samples) ? g_aux_hit_us.value : params.aux_hit_us;
	features.ewma_fallback_latency_us =
	    g_fallback_us.HasConfidence(params.min_samples) ? g_fallback_us.value : params.fallback_us;
	features.active_query_count = aux_state::GetExecutionLoadRegistry().Snapshot().total_active;
	return features;
}

aux_state::maintenance::EwmaStat &Q1AuxHitStatForTests() {
	return g_aux_hit_us;
}

void RecordQ1IncrementalSuccessObservation(bool success);
bool Q1HasSufficientTrainingSamples();

double Q1RecentIncrementalSuccessRate() {
	const auto &params = GetQ1CostModelParams();
	if (!g_incremental_success_rate.HasConfidence(params.min_samples)) {
		return 1.0 - params.incremental_failure_prob;
	}
	return g_incremental_success_rate.BoundedValue(0.0, 1.0);
}

void RecordQ1IncrementalSuccessObservation(bool success) {
	std::lock_guard<std::mutex> lock(g_workload_mutex);
	g_incremental_success_rate.Observe(success ? 1.0 : 0.0);
}

bool Q1HasSufficientTrainingSamples() {
	const auto &params = GetQ1CostModelParams();
	return g_incremental_success_rate.HasConfidence(params.min_samples);
}

double Q1PartitionRebuildUs() {
	const auto &params = GetQ1CostModelParams();
	return g_partition_rebuild_us.HasConfidence(params.min_samples) ? g_partition_rebuild_us.value
	                                                                : params.partition_rebuild_us;
}

double Q1FullRebuildUs() {
	const auto &params = GetQ1CostModelParams();
	return g_full_rebuild_us.HasConfidence(params.min_samples) ? g_full_rebuild_us.value : params.full_rebuild_us;
}

double Q1FirstQueryBlockingUs() {
	const auto &params = GetQ1CostModelParams();
	return g_first_query_blocking_us.HasConfidence(params.min_samples) ? g_first_query_blocking_us.value
	                                                                   : params.first_query_blocking_us;
}

double Q1ApplyDeltaOverheadUs() {
	const auto &params = GetQ1CostModelParams();
	return g_apply_delta_overhead_us.HasConfidence(params.min_samples) ? g_apply_delta_overhead_us.value
	                                                                   : params.apply_delta_overhead_us;
}

const std::deque<Q1MaintenanceDecisionRecord> &GetQ1MaintenanceDecisionLog() {
	return g_decision_log;
}

void AppendQ1MaintenanceDecisionRecord(const Q1MaintenanceDecisionRecord &record) {
	std::lock_guard<std::mutex> lock(g_workload_mutex);
	g_decision_log.push_back(record);
	while (g_decision_log.size() > kMaxDecisionLog) {
		g_decision_log.pop_front();
	}
}

} // namespace accelerator_codegen
