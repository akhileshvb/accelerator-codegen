#include "q14_hybrid_eval_common.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace {

using namespace q14_eval;

struct RunMetrics {
	int num_queries = 0;
	int queries_per_write = 10;
	int writes_executed = 0;
	double accel_workload_us = 0.0;
	double total_maintenance_overhead_us = 0.0;
	double accel_write_median_us = 0.0;
	double accel_hit_median_us = 0.0;
	double fallback_median_us = 0.0;
	double accel_hit_rate = 0.0;
	double fallback_rate = 0.0;
	uint64_t hybrid_decisions = 0;
	uint64_t action_selected_noop = 0;
	uint64_t action_selected_apply_delta = 0;
	uint64_t action_selected_rebuild_partition = 0;
	uint64_t action_selected_rebuild_all = 0;
	uint64_t action_selected_invalidate = 0;
	double prediction_error_mean_us = 0.0;
	double median_prediction_error_us = 0.0;
};

struct TimingStats {
	double median_us = 0.0;
};

TimingStats SummarizeSamples(std::vector<double> samples_us) {
	TimingStats stats;
	if (samples_us.empty()) {
		return stats;
	}
	std::sort(samples_us.begin(), samples_us.end());
	stats.median_us = samples_us[samples_us.size() / 2];
	return stats;
}

double MeasureReferenceQueryMedianUs(duckdb::Connection &connection, const std::string &reference_sql, int samples) {
	std::vector<double> query_samples;
	for (int i = 0; i < samples; i++) {
		const auto start = Clock::now();
		ExecuteAndConsume(connection, reference_sql);
		query_samples.push_back(ElapsedUs(start, Clock::now()));
	}
	return SummarizeSamples(std::move(query_samples)).median_us;
}

double MeasureBaseWriteMedianUs(duckdb::Connection &connection, const std::string &axis, const std::string &value,
                                const std::string &query_month, int samples, const FanoutCatalog &catalog) {
	std::vector<double> write_samples;
	for (int i = 0; i < samples; i++) {
		const auto start = Clock::now();
		connection.BeginTransaction();
		ApplyCrossoverWrite(connection, axis, value, query_month, 100000 + i, catalog);
		connection.Commit();
		write_samples.push_back(ElapsedUs(start, Clock::now()));
	}
	return SummarizeSamples(std::move(write_samples)).median_us;
}

RunMetrics RunCrossoverWorkload(duckdb::Connection &connection, const std::string &axis, const std::string &value,
                                const std::string &query_month, const std::string &aux_sql, int num_queries,
                                int warmup_queries, int queries_per_write, const FanoutCatalog &catalog) {
	RunMetrics result;
	result.num_queries = num_queries;
	result.queries_per_write = queries_per_write;

	const auto counters_start = accelerator_codegen::GetQ14AuxCounters();
	std::vector<double> hit_samples;
	std::vector<double> fallback_samples;
	std::vector<double> write_samples;
	int write_seq = 0;
	int accel_hits = 0;
	int fallbacks = 0;

	for (int i = 0; i < warmup_queries; i++) {
		ExecuteAndConsume(connection, aux_sql);
	}

	for (int query_index = 0; query_index < num_queries; query_index++) {
		if (ShouldWriteAtQueryIndex(query_index, queries_per_write)) {
			const auto write_start = Clock::now();
			connection.BeginTransaction();
			ApplyCrossoverWrite(connection, axis, value, query_month, write_seq, catalog);
			connection.Commit();
			write_samples.push_back(ElapsedUs(write_start, Clock::now()));
			write_seq++;
			result.writes_executed++;
		}

		const auto query_start = Clock::now();
		ExecuteAndConsume(connection, aux_sql);
		const auto sample = ElapsedUs(query_start, Clock::now());
		if (accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path) {
			accel_hits++;
			hit_samples.push_back(sample);
		} else {
			fallbacks++;
			fallback_samples.push_back(sample);
		}
	}

	const auto counters_end = accelerator_codegen::GetQ14AuxCounters();
	auto counter_delta = [&](auto member) { return counters_end.*member - counters_start.*member; };

	double total_accel_query_us = 0.0;
	for (const auto sample : hit_samples) {
		total_accel_query_us += sample;
	}
	for (const auto sample : fallback_samples) {
		total_accel_query_us += sample;
	}

	double total_accel_write_us = 0.0;
	for (const auto sample : write_samples) {
		total_accel_write_us += sample;
	}

	result.accel_hit_rate = static_cast<double>(accel_hits) / static_cast<double>(num_queries);
	result.fallback_rate = static_cast<double>(fallbacks) / static_cast<double>(num_queries);
	result.accel_hit_median_us = hit_samples.empty() ? 0.0 : SummarizeSamples(hit_samples).median_us;
	result.fallback_median_us = fallback_samples.empty() ? 0.0 : SummarizeSamples(fallback_samples).median_us;
	result.accel_write_median_us = write_samples.empty() ? 0.0 : SummarizeSamples(std::move(write_samples)).median_us;
	result.accel_workload_us = total_accel_query_us + total_accel_write_us;
	result.total_maintenance_overhead_us = SumMaintenanceOverheadUs();
	result.hybrid_decisions = counter_delta(&accelerator_codegen::Q14AuxCounters::hybrid_decisions);
	result.action_selected_noop = counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_noop);
	result.action_selected_apply_delta = counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_apply_delta);
	result.action_selected_rebuild_partition =
	    counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_rebuild_partition);
	result.action_selected_rebuild_all = counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_rebuild_all);
	result.action_selected_invalidate = counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_invalidate);
	const auto prediction_samples = counter_delta(&accelerator_codegen::Q14AuxCounters::prediction_error_samples);
	const auto prediction_sum = counter_delta(&accelerator_codegen::Q14AuxCounters::prediction_error_sum_us);
	result.prediction_error_mean_us =
	    prediction_samples > 0 ? prediction_sum / static_cast<double>(prediction_samples) : 0.0;
	std::vector<double> errors;
	for (const auto &record : accelerator_codegen::GetQ14MaintenanceDecisionLog()) {
		errors.push_back(std::abs(record.prediction_error_us));
	}
	if (!errors.empty()) {
		std::sort(errors.begin(), errors.end());
		result.median_prediction_error_us = errors[errors.size() / 2];
	}
	return result;
}

void PrintCrossoverHeader(std::ostream &out) {
	out << "crossover_axis,crossover_value,write_policy,query_month,num_queries,queries_per_write,writes_executed,"
	       "duckdb_workload_us,accel_workload_us,workload_speedup,total_maintenance_overhead_us,accel_write_median_us,"
	       "accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,hybrid_decisions,"
	       "action_selected_noop,action_selected_apply_delta,action_selected_rebuild_partition,"
	       "action_selected_rebuild_all,action_selected_invalidate,prediction_error_mean_us,"
	       "median_prediction_error_us\n";
}

void PrintCrossoverRow(std::ostream &out, const std::string &axis, const std::string &value,
                        const std::string &write_policy, const std::string &query_month, const RunMetrics &metrics,
                        double duckdb_workload_us) {
	const double workload_speedup =
	    metrics.accel_workload_us > 0.0 ? duckdb_workload_us / metrics.accel_workload_us : 0.0;
	out << axis << ',' << value << ',' << write_policy << ',' << query_month << ',' << metrics.num_queries << ','
	    << metrics.queries_per_write << ',' << metrics.writes_executed << ',' << std::fixed << std::setprecision(2)
	    << duckdb_workload_us << ',' << metrics.accel_workload_us << ',' << workload_speedup << ','
	    << metrics.total_maintenance_overhead_us << ',' << metrics.accel_write_median_us << ','
	    << metrics.accel_hit_median_us << ',' << metrics.fallback_median_us << ',' << metrics.accel_hit_rate << ','
	    << metrics.fallback_rate << ',' << metrics.hybrid_decisions << ',' << metrics.action_selected_noop << ','
	    << metrics.action_selected_apply_delta << ',' << metrics.action_selected_rebuild_partition << ','
	    << metrics.action_selected_rebuild_all << ',' << metrics.action_selected_invalidate << ','
	    << metrics.prediction_error_mean_us << ',' << metrics.median_prediction_error_us << '\n';
}

double CandidateCost(const accelerator_codegen::Q14MaintenanceDecisionRecord &record,
                       accelerator_codegen::aux_state::maintenance::MaintenanceAction action) {
	for (const auto &candidate : record.candidate_details) {
		if (candidate.action == action) {
			return candidate.total_overhead_us;
		}
	}
	return 0.0;
}

int ActionToCode(accelerator_codegen::aux_state::maintenance::MaintenanceAction action) {
	using accelerator_codegen::aux_state::maintenance::MaintenanceAction;
	switch (action) {
	case MaintenanceAction::NoOp:
		return 0;
	case MaintenanceAction::ApplyDelta:
		return 1;
	case MaintenanceAction::RebuildPartition:
		return 2;
	case MaintenanceAction::RebuildAll:
		return 3;
	case MaintenanceAction::InvalidateAndFallback:
		return 4;
	}
	return -1;
}

int RunCrossoverMode() {
	const auto axis = GetEnvString("ACCEL_Q14_CROSSOVER_AXIS", "lineitem_rows");
	const auto value = GetEnvString("ACCEL_Q14_CROSSOVER_VALUE", "1");
	constexpr const char *kWritePolicy = "hybrid";
	const auto query_month = GetEnvString("ACCEL_Q14_BENCH_MONTH", "1995-06-01");
	const int num_queries = GetEnvInt("ACCEL_BENCH_QUERIES", 500);
	const int warmup_queries = GetEnvInt("ACCEL_BENCH_WARMUP", 50);
	const int ref_samples = GetEnvInt("ACCEL_BENCH_REF_SAMPLES", 30);
	const int base_write_samples = GetEnvInt("ACCEL_BENCH_BASE_WRITE_SAMPLES", 5);
	const int queries_per_write = axis == "queries_per_write" ? std::max(1, std::stoi(value))
	                                                            : GetEnvInt("ACCEL_Q14_QUERIES_PER_WRITE", 10);

	const auto db_path = GetTpchDbPath();
	duckdb::DuckDB database(db_path.c_str());
	duckdb::Connection base_connection(database);
	duckdb::Connection accel_connection(database);
	Require(base_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
	Require(accel_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");

	const auto reference_sql = accelerator_codegen::test_support::ReferenceQ14Sql(query_month);
	const auto aux_sql = accelerator_codegen::test_support::AcceleratorQ14AuxSql(query_month);
	const auto catalog = LoadFanoutCatalog(accel_connection);
	const double duckdb_ref_query_median_us = MeasureReferenceQueryMedianUs(accel_connection, reference_sql, ref_samples);
	const double base_write_median_us =
	    MeasureBaseWriteMedianUs(base_connection, axis, value, query_month, base_write_samples, catalog);

	accelerator_codegen::RegisterAcceleratorFunctions(accel_connection);
	ExecuteAndConsume(accel_connection, "SELECT * FROM accel_q14_aux_build()");
	accelerator_codegen::ResetQ14AuxCounters();

	const auto metrics =
	    RunCrossoverWorkload(accel_connection, axis, value, query_month, aux_sql, num_queries, warmup_queries,
	                         queries_per_write, catalog);
	const double duckdb_workload_us =
	    duckdb_ref_query_median_us * static_cast<double>(num_queries) +
	    base_write_median_us * static_cast<double>(metrics.writes_executed);

	const bool print_header = GetEnvInt("ACCEL_Q14_EVAL_PRINT_HEADER", 0) != 0;
	if (print_header) {
		PrintCrossoverHeader(std::cout);
	}
	PrintCrossoverRow(std::cout, axis, value, kWritePolicy, query_month, metrics, duckdb_workload_us);
	return EXIT_SUCCESS;
}

struct PhaseSpec {
	const char *id;
	int num_queries;
	int queries_per_write;
	const char *kind;
};

void PrintPhasedHeader(std::ostream &out) {
	out << "phase_id,phase_query_index,global_query_index,event_type,selected_action,selected_action_code,"
	       "predicted_apply_delta_us,predicted_rebuild_partition_us,predicted_rebuild_all_us,predicted_invalidate_us,"
	       "predicted_selected_us,actual_maintenance_us,prediction_error_us,ewma_query_rate_eps,ewma_write_rate_eps,"
	       "expected_queries_before_next_write,ewma_aux_hit_us,ewma_fallback_us,accel_hit_rate_so_far,"
	       "fallback_rate_so_far,cumulative_maintenance_us,writes_in_phase,query_latency_us,write_latency_us,"
	       "duckdb_query_median_us,query_speedup\n";
}

int RunPhasedMode() {
	const auto query_month = GetEnvString("ACCEL_Q14_BENCH_MONTH", "1995-06-01");
	const auto timeline_path = GetEnvString("ACCEL_Q14_PHASED_TIMELINE_CSV", "");

	const std::vector<PhaseSpec> phases = {
	    {"A", GetEnvInt("ACCEL_Q14_PHASE_A_QUERIES", 150), GetEnvInt("ACCEL_Q14_PHASE_A_QPW", 10), "point_lineitem"},
	    {"B", GetEnvInt("ACCEL_Q14_PHASE_B_QUERIES", 150), GetEnvInt("ACCEL_Q14_PHASE_B_QPW", 15), "bulk_lineitem"},
	    {"C", GetEnvInt("ACCEL_Q14_PHASE_C_QUERIES", 150), GetEnvInt("ACCEL_Q14_PHASE_C_QPW", 3), "point_lineitem"},
	    {"D", GetEnvInt("ACCEL_Q14_PHASE_D_QUERIES", 150), GetEnvInt("ACCEL_Q14_PHASE_D_QPW", 10), "point_lineitem"},
	};

	const auto db_path = GetTpchDbPath();
	duckdb::DuckDB database(db_path.c_str());
	duckdb::Connection connection(database);
	Require(connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	ExecuteAndConsume(connection, "SELECT * FROM accel_q14_aux_build()");
	accelerator_codegen::ResetQ14AuxCounters();

	const auto aux_sql = accelerator_codegen::test_support::AcceleratorQ14AuxSql(query_month);
	const auto catalog = LoadFanoutCatalog(connection);

	std::ofstream timeline;
	const bool write_timeline_file = !timeline_path.empty();
	if (write_timeline_file) {
		timeline.open(timeline_path, std::ios::trunc);
		PrintPhasedHeader(timeline);
	} else {
		PrintPhasedHeader(std::cout);
	}
	std::ostream &events_out = write_timeline_file ? timeline : std::cout;

	const auto reference_sql = accelerator_codegen::test_support::ReferenceQ14Sql(query_month);
	const int ref_samples = GetEnvInt("ACCEL_BENCH_REF_SAMPLES", 30);
	const double duckdb_query_median_us = MeasureReferenceQueryMedianUs(connection, reference_sql, ref_samples);

	auto emit_event = [&](const std::string &phase_id, int phase_query_index, int global_query_index,
	                      const std::string &event_type,
	                      const accelerator_codegen::Q14MaintenanceDecisionRecord *decision, int accel_hits,
	                      int fallbacks, int queries_so_far, int writes_in_phase, double cumulative_maintenance_us,
	                      double query_latency_us, double write_latency_us) {
		const auto workload = accelerator_codegen::BuildQ14WorkloadFeatures(
		    accelerator_codegen::aux_state::AcceleratorStateRegistry::GetOrCreate(*connection.context->db)
		        ->GetStore("tpch_q14_aux"));
		std::string selected_action;
		int selected_code = -1;
		double predicted_apply = 0.0;
		double predicted_rebuild_part = 0.0;
		double predicted_rebuild_all = 0.0;
		double predicted_invalidate = 0.0;
		double predicted_selected = 0.0;
		double actual_maintenance = 0.0;
		double prediction_error = 0.0;
		if (decision) {
			selected_action = accelerator_codegen::aux_state::maintenance::MaintenanceActionName(decision->selected);
			selected_code = ActionToCode(decision->selected);
			predicted_apply = CandidateCost(*decision, accelerator_codegen::aux_state::maintenance::MaintenanceAction::ApplyDelta);
			predicted_rebuild_part =
			    CandidateCost(*decision, accelerator_codegen::aux_state::maintenance::MaintenanceAction::RebuildPartition);
			predicted_rebuild_all =
			    CandidateCost(*decision, accelerator_codegen::aux_state::maintenance::MaintenanceAction::RebuildAll);
			predicted_invalidate = CandidateCost(
			    *decision, accelerator_codegen::aux_state::maintenance::MaintenanceAction::InvalidateAndFallback);
			predicted_selected = decision->selected_predicted_cost_us;
			actual_maintenance = decision->actual_cost_us;
			prediction_error = decision->prediction_error_us;
		}
		const double hit_rate =
		    queries_so_far > 0 ? static_cast<double>(accel_hits) / static_cast<double>(queries_so_far) : 0.0;
		const double fallback_rate =
		    queries_so_far > 0 ? static_cast<double>(fallbacks) / static_cast<double>(queries_so_far) : 0.0;
		const double query_speedup =
		    event_type == "query" && query_latency_us > 0.0 ? duckdb_query_median_us / query_latency_us : 0.0;
		events_out << phase_id << ',' << phase_query_index << ',' << global_query_index << ',' << event_type << ','
		           << selected_action << ',' << selected_code << ',' << std::fixed << std::setprecision(3)
		           << predicted_apply << ',' << predicted_rebuild_part << ',' << predicted_rebuild_all << ','
		           << predicted_invalidate << ',' << predicted_selected << ',' << actual_maintenance << ','
		           << prediction_error << ',' << workload.q14_query_rate_eps << ',' << workload.relevant_write_rate_eps
		           << ',' << workload.expected_queries_before_next_write << ',' << workload.ewma_aux_hit_latency_us
		           << ',' << workload.ewma_fallback_latency_us << ',' << hit_rate << ',' << fallback_rate << ','
		           << cumulative_maintenance_us << ',' << writes_in_phase << ',' << query_latency_us << ','
		           << write_latency_us << ',' << duckdb_query_median_us << ',' << query_speedup << '\n';
	};

	int global_query_index = 0;
	int accel_hits = 0;
	int fallbacks = 0;
	int write_seq = 0;
	double cumulative_maintenance_us = 0.0;

	for (const auto &phase : phases) {
		int writes_in_phase = 0;
		emit_event(phase.id, 0, global_query_index, "phase_start", nullptr, accel_hits, fallbacks, global_query_index,
		           writes_in_phase, cumulative_maintenance_us, 0.0, 0.0);
		for (int phase_query_index = 0; phase_query_index < phase.num_queries; phase_query_index++, global_query_index++) {
			double write_latency_us = 0.0;
			const accelerator_codegen::Q14MaintenanceDecisionRecord *decision = nullptr;
			if (ShouldWriteAtQueryIndex(phase_query_index, phase.queries_per_write)) {
				const auto write_start = Clock::now();
				connection.BeginTransaction();
				if (std::string(phase.kind) == "bulk_lineitem") {
					ApplyBulkLineitemMonthWrite(connection, query_month);
				} else {
					ApplyPointLineitemWrite(connection, query_month, write_seq);
				}
				connection.Commit();
				write_latency_us = ElapsedUs(write_start, Clock::now());
				write_seq++;
				writes_in_phase++;
				const auto &log = accelerator_codegen::GetQ14MaintenanceDecisionLog();
				if (!log.empty()) {
					decision = &log.back();
					cumulative_maintenance_us += decision->actual_cost_us;
				}
				emit_event(phase.id, phase_query_index, global_query_index, "write", decision, accel_hits, fallbacks,
				           global_query_index, writes_in_phase, cumulative_maintenance_us, 0.0, write_latency_us);
			}

			const auto query_start = Clock::now();
			ExecuteAndConsume(connection, aux_sql);
			const auto query_latency_us = ElapsedUs(query_start, Clock::now());
			if (accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path) {
				accel_hits++;
			} else {
				fallbacks++;
			}
			emit_event(phase.id, phase_query_index, global_query_index, "query", nullptr, accel_hits, fallbacks,
			           global_query_index + 1, writes_in_phase, cumulative_maintenance_us, query_latency_us, 0.0);
		}
	}

	if (write_timeline_file) {
		std::cerr << "wrote phased timeline to " << timeline_path << '\n';
	}
	return EXIT_SUCCESS;
}

} // namespace

int main() {
	try {
		const auto mode = GetEnvString("ACCEL_Q14_EVAL_MODE", "crossover");
		if (mode == "phased") {
			return RunPhasedMode();
		}
		return RunCrossoverMode();
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q14_hybrid_eval failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
