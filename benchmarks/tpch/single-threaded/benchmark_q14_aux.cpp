#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "test_support/q14_aux_helpers.hpp"
#include "test_support/q14_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return std::stoi(value);
}

std::string GetEnvString(const char *name, const std::string &fallback = "") {
	const char *value = std::getenv(name);
	return value ? value : fallback;
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::vector<std::string> SplitCommaSeparated(const std::string &input) {
	std::vector<std::string> result;
	std::stringstream stream(input);
	std::string item;
	while (std::getline(stream, item, ',')) {
		if (!item.empty()) {
			result.push_back(item);
		}
	}
	return result;
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr && !result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			(void)materialized.GetValue(col, row);
		}
	}
}

struct TimingStats {
	double median_us = 0.0;
	double p95_us = 0.0;
	double mean_us = 0.0;
};

TimingStats SummarizeSamples(std::vector<double> samples_us) {
	TimingStats stats;
	if (samples_us.empty()) {
		return stats;
	}
	double sum = 0.0;
	for (const auto sample : samples_us) {
		sum += sample;
	}
	stats.mean_us = sum / static_cast<double>(samples_us.size());
	std::sort(samples_us.begin(), samples_us.end());
	const auto middle = samples_us.size() / 2;
	stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
	                                              : samples_us[middle];
	const auto p95_idx = static_cast<size_t>(std::floor(0.95 * static_cast<double>(samples_us.size() - 1)));
	stats.p95_us = samples_us[p95_idx];
	return stats;
}

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
	return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

double TimeBuildMs(duckdb::Connection &connection) {
	const auto start = Clock::now();
	ExecuteAndConsume(connection, "SELECT * FROM accel_q14_aux_build()");
	const auto end = Clock::now();
	return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

double TimePartitionRebuildMs(duckdb::Connection &connection, const std::string &start_date) {
	const auto start = Clock::now();
	ExecuteAndConsume(connection, "SELECT * FROM accel_q14_aux_rebuild_partition(DATE '" + start_date + "')");
	const auto end = Clock::now();
	return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

std::string QueryMonthEndExclusive(const std::string &query_start_date) {
	return "DATE '" + query_start_date + "' + INTERVAL '1' month";
}

std::string LineitemMonthFilter(const std::string &query_start_date) {
	return "l_shipdate >= DATE '" + query_start_date + "' AND l_shipdate < " + QueryMonthEndExclusive(query_start_date);
}

std::string PickLineitemTargetSubquery(const std::string &query_start_date, int seq, bool restrict_to_query_month) {
	std::string sql = "SELECT l_orderkey, l_linenumber FROM lineitem";
	if (restrict_to_query_month) {
		sql += " WHERE " + LineitemMonthFilter(query_start_date);
	}
	sql += " ORDER BY l_orderkey, l_linenumber LIMIT 1 OFFSET " + std::to_string(seq % 256);
	return sql;
}

std::string PickPartTargetSubquery(int seq) {
	return "SELECT p_partkey FROM part ORDER BY p_partkey LIMIT 1 OFFSET " + std::to_string(seq % 256);
}

int64_t CountLineitemsInMonth(duckdb::Connection &connection, const std::string &query_start_date) {
	auto result = connection.Query("SELECT COUNT(*) FROM lineitem WHERE " + LineitemMonthFilter(query_start_date));
	Require(result != nullptr && !result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	Require(materialized.RowCount() == 1, "expected lineitem count row");
	return materialized.GetValue(0, 0).GetValue<int64_t>();
}

void ApplyBulkLineitemMonthWrite(duckdb::Connection &connection, const std::string &query_start_date, int write_seq) {
	(void)write_seq;
	ExecuteAndConsume(connection,
	                  "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0000001 "
	                  "WHERE " +
	                      LineitemMonthFilter(query_start_date));
}

void ApplyBulkPartMonthWrite(duckdb::Connection &connection, const std::string &query_start_date, int write_seq,
                             int part_limit) {
	(void)write_seq;
	ExecuteAndConsume(connection,
	                  "UPDATE part SET p_type = CASE WHEN p_type LIKE 'PROMO%' THEN 'LARGE STEEL' "
	                  "ELSE 'PROMO STEEL' END "
	                  "WHERE p_partkey IN ("
	                  "SELECT l_partkey FROM lineitem WHERE " +
	                      LineitemMonthFilter(query_start_date) +
	                      " GROUP BY l_partkey ORDER BY COUNT(*) DESC LIMIT " + std::to_string(part_limit) + ")");
}

int BulkPartLimit() {
	return std::max(1, GetEnvInt("ACCEL_Q14_BULK_PART_LIMIT", 50));
}

bool IsBulkWriteSuite(const std::string &suite) {
	return suite == "bulk_lineitem_month" || suite == "bulk_part_month";
}

void ApplyWriteSuite(duckdb::Connection &connection, const std::string &suite, std::mt19937 &rng, int write_count,
                     int write_seq, const std::string &locality, const std::string &query_start_date) {
	std::uniform_int_distribution<int> bucket_dist(0, 99);
	for (int i = 0; i < write_count; i++) {
		const int seq = write_seq + i;
		if (suite == "bulk_lineitem_month") {
			ApplyBulkLineitemMonthWrite(connection, query_start_date, seq);
			continue;
		}
		if (suite == "bulk_part_month") {
			ApplyBulkPartMonthWrite(connection, query_start_date, seq, BulkPartLimit());
			continue;
		}
		const int bucket = bucket_dist(rng);
		if (suite == "lineitem_only") {
			const std::string month = locality == "single_month" ? query_start_date.substr(0, 7)
			                                                     : "1995-" + std::to_string((seq % 12) + 1);
			const std::string shipdate = month + "-15";
			const auto target = PickLineitemTargetSubquery(query_start_date, seq, true);
			if (bucket < 25) {
				ExecuteAndConsume(connection,
				                  "UPDATE lineitem SET l_comment = l_comment "
				                  "WHERE (l_orderkey, l_linenumber) IN (" +
				                      target + ")");
			} else if (bucket < 60) {
				ExecuteAndConsume(connection,
				                  "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0001, "
				                  "l_discount = l_discount "
				                  "WHERE (l_orderkey, l_linenumber) IN (" +
				                      target + ")");
			} else if (bucket < 80) {
				ExecuteAndConsume(connection, "UPDATE lineitem SET l_shipdate = DATE '" + shipdate +
				                                  "' WHERE (l_orderkey, l_linenumber) IN (" + target + ")");
			} else if (bucket < 90) {
				ExecuteAndConsume(connection,
				                  "INSERT INTO lineitem "
				                  "SELECT " +
				                      std::to_string(9000000000LL + seq) +
				                      ", l_partkey, l_suppkey, l_linenumber + 100, l_quantity, l_extendedprice, "
				                      "l_discount, l_tax, l_returnflag, l_linestatus, DATE '" +
				                      shipdate +
				                      "', l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment "
				                      "FROM lineitem WHERE (l_orderkey, l_linenumber) IN (" +
				                      target + ")");
			} else {
				ExecuteAndConsume(connection, "DELETE FROM lineitem WHERE (l_orderkey, l_linenumber) IN (" + target + ")");
			}
		} else if (suite == "part_only") {
			const auto target = PickPartTargetSubquery(seq);
			if (bucket < 30) {
				ExecuteAndConsume(connection,
				                  "UPDATE part SET p_name = p_name WHERE p_partkey IN (" + target + ")");
			} else if (bucket < 70) {
				ExecuteAndConsume(connection, "UPDATE part SET p_type = CASE WHEN p_type LIKE 'PROMO%' THEN 'LARGE STEEL' "
				                              "ELSE 'PROMO STEEL' END WHERE p_partkey IN (" +
				                                  target + ")");
			} else if (bucket < 85) {
				ExecuteAndConsume(connection,
				                  "INSERT INTO part "
				                  "SELECT " +
				                      std::to_string(9000000000LL + seq) +
				                      ", p_name, p_mfgr, p_brand, p_type, p_size, p_container, p_retailprice, p_comment "
				                      "FROM part WHERE p_partkey IN (" +
				                      target + ")");
			} else {
				ExecuteAndConsume(connection, "DELETE FROM part WHERE p_partkey IN (" + target + ")");
			}
		}
	}
}

double TimeWriteCommitUs(duckdb::Connection &connection, const std::string &suite, std::mt19937 &rng, int write_seq,
                         const std::string &locality, const std::string &query_start_date) {
	const auto start = Clock::now();
	connection.BeginTransaction();
	ApplyWriteSuite(connection, suite, rng, 1, write_seq, locality, query_start_date);
	connection.Commit();
	const auto end = Clock::now();
	return ElapsedUs(start, end);
}

int ExpectedWritesPerThousand(int write_frequency_pct) {
	if (write_frequency_pct <= 0) {
		return 0;
	}
	return (write_frequency_pct * 1000) / 100;
}

int ExpectedWritesForQueries(int num_queries, int write_frequency_pct) {
	if (write_frequency_pct <= 0 || num_queries <= 0) {
		return 0;
	}
	return (ExpectedWritesPerThousand(write_frequency_pct) * num_queries) / 1000;
}

bool ShouldWriteAtQueryIndex(int query_index, int write_frequency_pct) {
	if (write_frequency_pct <= 0) {
		return false;
	}
	if (write_frequency_pct == 1) {
		return query_index % 100 == 0;
	}
	if (write_frequency_pct == 10) {
		return query_index % 10 == 0;
	}
	if (write_frequency_pct == 50) {
		return query_index % 2 == 0;
	}
	const int stride = 100 / write_frequency_pct;
	return stride > 0 && (query_index % stride) == 0;
}

struct WorkloadTimings {
	int num_queries = 0;
	int writes_scheduled = 0;
	int writes_executed = 0;
	double duckdb_ref_query_median_us = 0.0;
	double base_write_median_us = 0.0;
	double accel_hit_median_us = 0.0;
	double fallback_median_us = 0.0;
	double accel_hit_rate = 0.0;
	double fallback_rate = 0.0;
	double total_duckdb_query_us = 0.0;
	double total_accel_query_us = 0.0;
	double total_base_write_us = 0.0;
	double total_accel_write_us = 0.0;
	double total_incremental_apply_us = 0.0;
	uint64_t incremental_apply_attempts = 0;
	uint64_t dependent_mutation_events_with_plan = 0;
	uint64_t dependent_mutation_events_empty_plan = 0;
	uint64_t whole_state_invalidations = 0;
	uint64_t partition_invalidations = 0;
	uint64_t fallback_queries = 0;
	uint64_t auxiliary_hits = 0;
	uint64_t rebuild_cost_model_skips = 0;
	uint64_t hybrid_decisions = 0;
	uint64_t low_confidence_decisions = 0;
	uint64_t lazy_rebuild_schedules = 0;
	uint64_t action_selected_noop = 0;
	uint64_t action_selected_apply_delta = 0;
	uint64_t action_selected_rebuild_partition = 0;
	uint64_t action_selected_rebuild_all = 0;
	uint64_t action_selected_invalidate = 0;
	double prediction_error_mean_us = 0.0;
	double median_prediction_error_us = 0.0;
	double p95_prediction_error_us = 0.0;
	double oracle_regret_pct = 0.0;
	int64_t rows_touched_per_write = 0;
};

void ApplySingleWrite(duckdb::Connection &connection, const std::string &suite, std::mt19937 &rng, int write_seq,
                      const std::string &locality, const std::string &query_start_date) {
	ApplyWriteSuite(connection, suite, rng, 1, write_seq, locality, query_start_date);
}

void AppendBenchTrace(const std::string &trace_path, const std::string &suite, const std::string &locality,
                      const std::string &query_month, int write_seq) {
	if (trace_path.empty()) {
		return;
	}
	std::ofstream out(trace_path, std::ios::app);
	out << suite << ',' << locality << ',' << query_month << ',' << write_seq << '\n';
}

std::pair<double, double> SummarizePredictionErrors() {
	std::vector<double> errors;
	for (const auto &record : accelerator_codegen::GetQ14MaintenanceDecisionLog()) {
		errors.push_back(std::abs(record.prediction_error_us));
	}
	if (errors.empty()) {
		return {0.0, 0.0};
	}
	std::sort(errors.begin(), errors.end());
	const auto median = errors[errors.size() / 2];
	const auto p95_idx = static_cast<size_t>(std::floor(0.95 * static_cast<double>(errors.size() - 1)));
	return {median, errors[p95_idx]};
}

WorkloadTimings RunMeasuredWorkload(duckdb::Connection &connection, const std::string &suite,
                                    const std::string &locality, const std::string &query_start_date,
                                    const std::string &reference_sql, const std::string &aux_sql, int num_queries,
                                    int write_frequency_pct, std::mt19937 &rng, int warmup_queries,
                                    const std::string &trace_path = "") {
	WorkloadTimings result;
	result.num_queries = num_queries;
	result.writes_scheduled = ExpectedWritesForQueries(num_queries, write_frequency_pct);

	for (int i = 0; i < warmup_queries; i++) {
		ExecuteAndConsume(connection, aux_sql);
	}

	const auto counters_start = accelerator_codegen::GetQ14AuxCounters();
	int write_seq = 0;
	int accel_hits = 0;
	int fallbacks = 0;
	std::vector<double> hit_samples;
	std::vector<double> fallback_samples;
	std::vector<double> write_samples;
	hit_samples.reserve(num_queries);
	fallback_samples.reserve(num_queries);
	write_samples.reserve(result.writes_scheduled);

	for (int query_index = 0; query_index < num_queries; query_index++) {
		if (ShouldWriteAtQueryIndex(query_index, write_frequency_pct)) {
			write_samples.push_back(
			    TimeWriteCommitUs(connection, suite, rng, write_seq, locality, query_start_date));
			AppendBenchTrace(trace_path, suite, locality, query_start_date, write_seq);
			write_seq++;
			result.writes_executed++;
		}

		const auto query_start = Clock::now();
		ExecuteAndConsume(connection, aux_sql);
		const auto query_end = Clock::now();
		const auto sample = ElapsedUs(query_start, query_end);
		result.total_accel_query_us += sample;

		const auto &diag = accelerator_codegen::GetQ14AuxDiagnostics();
		if (diag.used_aux_path) {
			accel_hits++;
			hit_samples.push_back(sample);
		} else {
			fallbacks++;
			fallback_samples.push_back(sample);
		}
	}

	const auto counters_end = accelerator_codegen::GetQ14AuxCounters();
	auto counter_delta = [&](auto member) { return counters_end.*member - counters_start.*member; };

	result.total_accel_write_us = 0.0;
	for (const auto sample : write_samples) {
		result.total_accel_write_us += sample;
	}
	result.accel_hit_rate = static_cast<double>(accel_hits) / static_cast<double>(num_queries);
	result.fallback_rate = static_cast<double>(fallbacks) / static_cast<double>(num_queries);
	result.accel_hit_median_us =
	    hit_samples.empty() ? 0.0 : SummarizeSamples(hit_samples).median_us;
	result.fallback_median_us =
	    fallback_samples.empty() ? 0.0 : SummarizeSamples(fallback_samples).median_us;
	result.incremental_apply_attempts = counter_delta(&accelerator_codegen::Q14AuxCounters::incremental_apply_attempts);
	result.total_incremental_apply_us =
	    static_cast<double>(counter_delta(&accelerator_codegen::Q14AuxCounters::incremental_apply_ns)) / 1000.0;
	result.dependent_mutation_events_with_plan =
	    counter_delta(&accelerator_codegen::Q14AuxCounters::dependent_mutation_events_with_plan);
	result.dependent_mutation_events_empty_plan =
	    counter_delta(&accelerator_codegen::Q14AuxCounters::dependent_mutation_events_empty_plan);
	result.whole_state_invalidations = counter_delta(&accelerator_codegen::Q14AuxCounters::whole_state_invalidations);
	result.partition_invalidations = counter_delta(&accelerator_codegen::Q14AuxCounters::partition_invalidations);
	result.fallback_queries = counter_delta(&accelerator_codegen::Q14AuxCounters::fallback_queries);
	result.auxiliary_hits = counter_delta(&accelerator_codegen::Q14AuxCounters::auxiliary_hits);
	result.rebuild_cost_model_skips = counter_delta(&accelerator_codegen::Q14AuxCounters::rebuild_cost_model_skips);
	result.hybrid_decisions = counter_delta(&accelerator_codegen::Q14AuxCounters::hybrid_decisions);
	result.low_confidence_decisions = counter_delta(&accelerator_codegen::Q14AuxCounters::low_confidence_decisions);
	result.lazy_rebuild_schedules = counter_delta(&accelerator_codegen::Q14AuxCounters::lazy_rebuild_schedules);
	result.action_selected_noop = counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_noop);
	result.action_selected_apply_delta =
	    counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_apply_delta);
	result.action_selected_rebuild_partition =
	    counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_rebuild_partition);
	result.action_selected_rebuild_all =
	    counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_rebuild_all);
	result.action_selected_invalidate =
	    counter_delta(&accelerator_codegen::Q14AuxCounters::action_selected_invalidate);
	const auto prediction_samples = counter_delta(&accelerator_codegen::Q14AuxCounters::prediction_error_samples);
	const auto prediction_sum = counter_delta(&accelerator_codegen::Q14AuxCounters::prediction_error_sum_us);
	result.prediction_error_mean_us =
	    prediction_samples > 0 ? prediction_sum / static_cast<double>(prediction_samples) : 0.0;
	const auto [median_error, p95_error] = SummarizePredictionErrors();
	result.median_prediction_error_us = median_error;
	result.p95_prediction_error_us = p95_error;
	return result;
}

double MeasureReferenceQueryMedianUs(duckdb::Connection &connection, const std::string &reference_sql, int samples) {
	std::vector<double> query_samples;
	query_samples.reserve(samples);
	for (int i = 0; i < samples; i++) {
		const auto start = Clock::now();
		ExecuteAndConsume(connection, reference_sql);
		query_samples.push_back(ElapsedUs(start, Clock::now()));
	}
	return SummarizeSamples(std::move(query_samples)).median_us;
}

double MeasureBaseWriteMedianUs(duckdb::Connection &connection, const std::string &suite, const std::string &locality,
                                const std::string &query_start_date, int samples, std::mt19937 &rng) {
	std::vector<double> write_samples;
	write_samples.reserve(samples);
	for (int i = 0; i < samples; i++) {
		write_samples.push_back(TimeWriteCommitUs(connection, suite, rng, 100000 + i, locality, query_start_date));
	}
	return SummarizeSamples(std::move(write_samples)).median_us;
}

} // namespace

int main() {
	try {
		const int warmup_queries = GetEnvInt("ACCEL_BENCH_WARMUP", 100);
		const int measured_queries = GetEnvInt("ACCEL_BENCH_QUERIES", 1000);
		const int ref_samples = GetEnvInt("ACCEL_BENCH_REF_SAMPLES", 50);
		const int base_write_samples = GetEnvInt("ACCEL_BENCH_BASE_WRITE_SAMPLES", 20);
		const auto suite = GetEnvString("ACCEL_Q14_BENCH_SUITE", "lineitem_only");
		constexpr const char *kWritePolicy = "hybrid";
		const auto locality = GetEnvString("ACCEL_Q14_WRITE_LOCALITY", "single_month");
		const int write_frequency_pct = GetEnvInt("ACCEL_Q14_WRITE_FREQUENCY_PCT", 10);
		const auto query_month = GetEnvString("ACCEL_Q14_BENCH_MONTH", "1995-06-01");
		const auto trace_path = GetEnvString("ACCEL_Q14_BENCH_TRACE");

		const auto db_path = GetTpchDbPath();

		duckdb::DuckDB database(db_path.c_str());
		duckdb::Connection base_connection(database);
		duckdb::Connection accel_connection(database);
		Require(base_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
		Require(accel_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");

		const auto reference_sql = accelerator_codegen::test_support::ReferenceQ14Sql(query_month);
		const auto aux_sql = accelerator_codegen::test_support::AcceleratorQ14AuxSql(query_month);

		const int base_write_samples_effective =
		    IsBulkWriteSuite(suite) ? std::min(base_write_samples, 3) : base_write_samples;
		std::mt19937 rng(42);
		const double base_write_median_us = MeasureBaseWriteMedianUs(base_connection, suite, locality, query_month,
		                                                           base_write_samples_effective, rng);

		accelerator_codegen::RegisterAcceleratorFunctions(accel_connection);
		const double setup_full_build_ms = TimeBuildMs(accel_connection);
		const double partition_rebuild_ms = TimePartitionRebuildMs(accel_connection, query_month);

		int64_t rows_touched_per_write = 0;
		if (suite == "bulk_lineitem_month") {
			rows_touched_per_write = CountLineitemsInMonth(accel_connection, query_month);
		} else if (suite == "bulk_part_month") {
			rows_touched_per_write = BulkPartLimit();
		}

		const double duckdb_ref_query_median_us =
		    MeasureReferenceQueryMedianUs(accel_connection, reference_sql, ref_samples);

		accelerator_codegen::ResetQ14AuxCounters();
		auto workload = RunMeasuredWorkload(accel_connection, suite, locality, query_month, reference_sql, aux_sql,
		                                    measured_queries, write_frequency_pct, rng, warmup_queries, trace_path);
		workload.duckdb_ref_query_median_us = duckdb_ref_query_median_us;
		workload.base_write_median_us = base_write_median_us;
		workload.total_duckdb_query_us = duckdb_ref_query_median_us * static_cast<double>(measured_queries);
		workload.total_base_write_us =
		    base_write_median_us * static_cast<double>(workload.writes_executed);
		workload.rows_touched_per_write = rows_touched_per_write;

		const double duckdb_workload_us = workload.total_duckdb_query_us + workload.total_base_write_us;
		const double accel_workload_us = workload.total_accel_query_us + workload.total_accel_write_us;
		const double workload_speedup = accel_workload_us > 0.0 ? duckdb_workload_us / accel_workload_us : 0.0;
		const double query_only_speedup = workload.accel_hit_median_us > 0.0
		                                      ? duckdb_ref_query_median_us / workload.accel_hit_median_us
		                                      : 0.0;
		const double blended_query_us = workload.accel_hit_rate * workload.accel_hit_median_us +
		                                workload.fallback_rate *
		                                    (workload.fallback_median_us > 0.0 ? workload.fallback_median_us
		                                                                       : duckdb_ref_query_median_us);
		const double blended_query_speedup =
		    blended_query_us > 0.0 ? duckdb_ref_query_median_us / blended_query_us : 0.0;
		const double incremental_apply_per_write_us =
		    workload.writes_executed > 0 ? workload.total_incremental_apply_us / workload.writes_executed : 0.0;
		const double rebuild_vs_incremental_ratio =
		    incremental_apply_per_write_us > 0.0 ? (partition_rebuild_ms * 1000.0) / incremental_apply_per_write_us : 0.0;

		std::cout << "suite,write_policy,locality,write_frequency_pct,query_month,num_queries,writes_scheduled,"
		             "writes_executed,rows_touched_per_write,duckdb_ref_query_median_us,base_write_median_us,"
		             "accel_write_median_us,accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,"
		             "total_duckdb_query_us,total_accel_query_us,total_base_write_us,total_accel_write_us,"
		             "total_incremental_apply_us,incremental_apply_per_write_us,incremental_apply_attempts,"
		             "duckdb_workload_us,accel_workload_us,workload_speedup,query_only_speedup,blended_query_speedup,"
		             "setup_full_build_ms,partition_rebuild_ms,rebuild_vs_incremental_apply_ratio,"
		             "dependent_mutation_events_with_plan,dependent_mutation_events_empty_plan,whole_state_invalidations,partition_invalidations,"
		             "fallback_queries,auxiliary_hits,rebuild_cost_model_skips,hybrid_decisions,low_confidence_decisions,"
		             "lazy_rebuild_schedules,action_selected_noop,action_selected_apply_delta,"
		             "action_selected_rebuild_partition,action_selected_rebuild_all,action_selected_invalidate,"
		             "prediction_error_mean_us,median_prediction_error_us,p95_prediction_error_us,oracle_regret_pct,"
		             "base_write_total_us\n";

		const auto accel_write_median_us = workload.writes_executed > 0
		                                       ? workload.total_accel_write_us / workload.writes_executed
		                                       : 0.0;

		std::cout << suite << ',' << kWritePolicy << ',' << locality << ',' << write_frequency_pct << ',' << query_month
		          << ',' << workload.num_queries << ',' << workload.writes_scheduled << ',' << workload.writes_executed
		          << ',' << rows_touched_per_write << ',' << std::fixed << std::setprecision(2)
		          << workload.duckdb_ref_query_median_us << ',' << workload.base_write_median_us << ','
		          << accel_write_median_us << ',' << workload.accel_hit_median_us << ',' << workload.fallback_median_us
		          << ',' << workload.accel_hit_rate << ',' << workload.fallback_rate << ','
		          << workload.total_duckdb_query_us << ',' << workload.total_accel_query_us << ','
		          << workload.total_base_write_us << ',' << workload.total_accel_write_us << ','
		          << workload.total_incremental_apply_us << ',' << incremental_apply_per_write_us << ','
		          << workload.incremental_apply_attempts << ',' << duckdb_workload_us << ',' << accel_workload_us << ','
		          << workload_speedup << ',' << query_only_speedup << ',' << blended_query_speedup << ','
		          << setup_full_build_ms << ',' << partition_rebuild_ms << ',' << rebuild_vs_incremental_ratio << ','
		          << workload.dependent_mutation_events_with_plan << ',' << workload.dependent_mutation_events_empty_plan
		          << ','
		          << workload.whole_state_invalidations << ',' << workload.partition_invalidations << ','
		          << workload.fallback_queries << ',' << workload.auxiliary_hits << ','
		          << workload.rebuild_cost_model_skips << ',' << workload.hybrid_decisions << ','
		          << workload.low_confidence_decisions << ',' << workload.lazy_rebuild_schedules << ','
		          << workload.action_selected_noop << ',' << workload.action_selected_apply_delta << ','
		          << workload.action_selected_rebuild_partition << ',' << workload.action_selected_rebuild_all << ','
		          << workload.action_selected_invalidate << ','
		          << workload.prediction_error_mean_us << ',' << workload.median_prediction_error_us << ','
		          << workload.p95_prediction_error_us << ',' << workload.oracle_regret_pct << ','
		          << workload.total_base_write_us << '\n';

		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q14_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
