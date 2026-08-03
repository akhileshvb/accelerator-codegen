#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_finalize.hpp"
#include "accelerator_codegen/sec_edgar_q1_write_policy.hpp"
#include "test_support/result_compare.hpp"
#include "test_support/sec_edgar_q1_aux_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

constexpr const char *kDefaultDb = "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";

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
	return stats;
}

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
	return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

double TimeBuildMs(duckdb::Connection &connection) {
	const auto start = Clock::now();
	ExecuteAndConsume(connection, "SELECT * FROM accel_sec_edgar_q1_aux_build()");
	const auto end = Clock::now();
	return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

double TimePartitionRebuildMs(duckdb::Connection &connection, int64_t partition_key) {
	const auto start = Clock::now();
	ExecuteAndConsume(connection,
	                  "SELECT * FROM accel_sec_edgar_q1_aux_rebuild_partition(" + std::to_string(partition_key) + ")");
	const auto end = Clock::now();
	return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

struct FilingAppendTarget {
	std::string stmt;
	std::string rfile;
};

std::vector<FilingAppendTarget> LoadFilingAppendTargets(duckdb::Connection &connection, const std::string &bench_group,
                                                      const std::string &locality) {
	std::string filter = "stmt IS NOT NULL AND rfile IS NOT NULL";
	if (locality == "single_group" && !bench_group.empty()) {
		const auto comma = bench_group.find(',');
		const auto pipe = bench_group.find('|');
		const auto sep = comma != std::string::npos ? comma : pipe;
		if (sep != std::string::npos) {
			const auto stmt = bench_group.substr(0, sep);
			const auto rfile = bench_group.substr(sep + 1);
			filter += " AND stmt = '" + stmt + "' AND rfile = '" + rfile + "'";
		}
	}
	auto result = connection.Query("SELECT DISTINCT stmt, rfile FROM pre WHERE " + filter + " LIMIT 256");
	Require(result != nullptr && !result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	std::vector<FilingAppendTarget> targets;
	targets.reserve(materialized.RowCount() > 0 ? materialized.RowCount() : 1);
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		targets.push_back({materialized.GetValue(0, row).ToString(), materialized.GetValue(1, row).ToString()});
	}
	if (targets.empty()) {
		targets.push_back({"BS", "H"});
	}
	return targets;
}

std::string BuildFilingAppendSql(const FilingAppendTarget &target, int write_seq) {
	return "INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating) "
	       "SELECT 'bench-" +
	       std::to_string(write_seq) + "-' || adsh, report, line + " + std::to_string(1 + (write_seq % 1000)) +
	       ", stmt, inpth, rfile, tag, version, plabel, negating FROM pre WHERE stmt = '" + target.stmt +
	       "' AND rfile = '" + target.rfile + "' LIMIT 1";
}

void ApplyFilingAppendWrite(duckdb::Connection &connection, const std::vector<FilingAppendTarget> &targets,
                            int write_seq, const std::string &locality) {
	const auto index = locality == "single_group" ? 0 : static_cast<size_t>(write_seq % targets.size());
	ExecuteAndConsume(connection, BuildFilingAppendSql(targets[index], write_seq));
}

void ApplyWriteSuite(duckdb::Connection &connection, const std::string &suite, int write_seq,
                     const std::string &locality, const std::vector<FilingAppendTarget> &targets) {
	if (suite == "filing_append") {
		ApplyFilingAppendWrite(connection, targets, write_seq, locality);
		return;
	}
	throw std::runtime_error("unsupported bench suite: " + suite);
}

double TimeWriteCommitUs(duckdb::Connection &connection, const std::string &suite, int write_seq,
                           const std::string &locality, const std::vector<FilingAppendTarget> &targets) {
	const auto start = Clock::now();
	connection.BeginTransaction();
	ApplyWriteSuite(connection, suite, write_seq, locality, targets);
	connection.Commit();
	const auto end = Clock::now();
	return ElapsedUs(start, end);
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
	uint64_t auxiliary_hits = 0;
	uint64_t fallback_queries = 0;
};

WorkloadTimings RunMeasuredWorkload(duckdb::Connection &connection, const std::string &suite,
                                    const std::string &locality, const std::string &aux_sql, int num_queries,
                                    int write_frequency_pct, const std::vector<FilingAppendTarget> &targets,
                                    int warmup_queries) {
	WorkloadTimings result;
	result.num_queries = num_queries;

	for (int i = 0; i < warmup_queries; i++) {
		ExecuteAndConsume(connection, aux_sql);
	}

	const auto counters_start = accelerator_codegen::GetSecEdgarQ1AuxCounters();
	int write_seq = 0;
	int accel_hits = 0;
	int fallbacks = 0;
	std::vector<double> hit_samples;
	std::vector<double> fallback_samples;
	std::vector<double> write_samples;

	for (int query_index = 0; query_index < num_queries; query_index++) {
		if (ShouldWriteAtQueryIndex(query_index, write_frequency_pct)) {
			write_samples.push_back(TimeWriteCommitUs(connection, suite, write_seq, locality, targets));
			write_seq++;
			result.writes_executed++;
		}

		const auto query_start = Clock::now();
		ExecuteAndConsume(connection, aux_sql);
		const auto query_end = Clock::now();
		const auto sample = ElapsedUs(query_start, query_end);
		result.total_accel_query_us += sample;

		const auto &diag = accelerator_codegen::GetSecEdgarQ1AuxDiagnostics();
		if (diag.used_aux_path) {
			accel_hits++;
			hit_samples.push_back(sample);
		} else {
			fallbacks++;
			fallback_samples.push_back(sample);
		}
	}

	const auto counters_end = accelerator_codegen::GetSecEdgarQ1AuxCounters();
	result.auxiliary_hits = counters_end.auxiliary_hits - counters_start.auxiliary_hits;
	result.fallback_queries = counters_end.fallback_queries - counters_start.fallback_queries;
	result.total_accel_write_us = 0.0;
	for (const auto sample : write_samples) {
		result.total_accel_write_us += sample;
	}
	result.accel_hit_rate = static_cast<double>(accel_hits) / static_cast<double>(num_queries);
	result.fallback_rate = static_cast<double>(fallbacks) / static_cast<double>(num_queries);
	result.accel_hit_median_us = hit_samples.empty() ? 0.0 : SummarizeSamples(hit_samples).median_us;
	result.fallback_median_us =
	    fallback_samples.empty() ? 0.0 : SummarizeSamples(std::move(fallback_samples)).median_us;
	return result;
}

struct MeasuredQueryTimings {
	double median_us = 0.0;
	double total_us = 0.0;
};

MeasuredQueryTimings MeasureQueryWorkload(duckdb::Connection &connection, const std::string &sql, int warmup_queries,
                                          int measured_queries) {
	MeasuredQueryTimings result;
	for (int i = 0; i < warmup_queries; i++) {
		ExecuteAndConsume(connection, sql);
	}
	std::vector<double> samples;
	samples.reserve(measured_queries);
	for (int i = 0; i < measured_queries; i++) {
		const auto start = Clock::now();
		ExecuteAndConsume(connection, sql);
		const auto sample = ElapsedUs(start, Clock::now());
		samples.push_back(sample);
		result.total_us += sample;
	}
	result.median_us = SummarizeSamples(std::move(samples)).median_us;
	return result;
}

double MeasureBaseWriteMedianUs(duckdb::Connection &connection, const std::string &suite, const std::string &locality,
                                const std::vector<FilingAppendTarget> &targets, int samples) {
	std::vector<double> write_samples;
	write_samples.reserve(samples);
	for (int i = 0; i < samples; i++) {
		write_samples.push_back(TimeWriteCommitUs(connection, suite, 100000 + i, locality, targets));
	}
	return SummarizeSamples(std::move(write_samples)).median_us;
}

} // namespace

int main() {
	try {
		setenv("ACCEL_AUX_MAINTENANCE_SELECTOR", "hybrid", 1);

		const int warmup_queries = GetEnvInt("ACCEL_BENCH_WARMUP", 10);
		const int measured_queries = GetEnvInt("ACCEL_BENCH_QUERIES", 100);
		const int base_write_samples = GetEnvInt("ACCEL_BENCH_BASE_WRITE_SAMPLES", 3);
		const auto suite = GetEnvString("ACCEL_Q1_BENCH_SUITE", "filing_append");
		constexpr const char *kWritePolicy = "hybrid";
		const auto locality = GetEnvString("ACCEL_Q1_WRITE_LOCALITY", "single_group");
		const int write_frequency_pct = GetEnvInt("ACCEL_Q1_WRITE_FREQUENCY_PCT", 0);
		const auto bench_group = GetEnvString("ACCEL_Q1_BENCH_GROUP", "BS|H");
		const int bench_trace = GetEnvInt("ACCEL_Q1_BENCH_TRACE", 0);

		const auto db_path = accelerator_codegen::test_support::GetBenchmarkDbPath("ACCEL_SEC_EDGAR_DB", kDefaultDb);

		duckdb::DuckDB database(db_path.c_str());
		duckdb::Connection base_connection(database);
		duckdb::Connection accel_connection(database);
		Require(base_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
		Require(accel_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");

		const auto reference_sql = accelerator_codegen::test_support::ReferenceSecEdgarQ1Sql();
		const auto base_sql = accelerator_codegen::test_support::AcceleratorSecEdgarQ1Sql();
		const auto aux_sql = accelerator_codegen::test_support::AcceleratorSecEdgarQ1AuxSql();
		const auto targets = LoadFilingAppendTargets(base_connection, bench_group, locality);

		const double base_write_median_us =
		    MeasureBaseWriteMedianUs(base_connection, suite, locality, targets, base_write_samples);

		accelerator_codegen::RegisterAcceleratorFunctions(accel_connection);
		const double setup_full_build_ms = TimeBuildMs(accel_connection);
		const int64_t partition_key = static_cast<int64_t>(
		    accelerator_codegen::PartitionKeyFromStmtRfileStrings("BS", "H", false));
		const double partition_rebuild_ms =
		    GetEnvInt("ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB", 0) != 0
		        ? 0.0
		        : TimePartitionRebuildMs(accel_connection, partition_key);

		const auto duckdb_ref_timings =
		    MeasureQueryWorkload(base_connection, reference_sql, warmup_queries, measured_queries);
		const auto base_kernel_timings =
		    MeasureQueryWorkload(accel_connection, base_sql, warmup_queries, measured_queries);
		const double duckdb_ref_query_median_us = duckdb_ref_timings.median_us;
		const double base_kernel_median_us = base_kernel_timings.median_us;

		accelerator_codegen::ResetSecEdgarQ1AuxCounters();
		auto workload = RunMeasuredWorkload(accel_connection, suite, locality, aux_sql, measured_queries,
		                                    write_frequency_pct, targets, warmup_queries);
		workload.duckdb_ref_query_median_us = duckdb_ref_query_median_us;
		workload.base_write_median_us = base_write_median_us;
		workload.total_duckdb_query_us = duckdb_ref_timings.total_us;
		workload.total_base_write_us = base_write_median_us * static_cast<double>(workload.writes_executed);

		const double duckdb_workload_us = workload.total_duckdb_query_us + workload.total_base_write_us;
		const double accel_workload_us = workload.total_accel_query_us + workload.total_accel_write_us;
		const double setup_full_build_us = setup_full_build_ms * 1000.0;
		const double cold_start_accel_workload_us = accel_workload_us + setup_full_build_us;
		const double workload_speedup = accel_workload_us > 0.0 ? duckdb_workload_us / accel_workload_us : 0.0;
		const double cold_start_workload_speedup =
		    cold_start_accel_workload_us > 0.0 ? duckdb_workload_us / cold_start_accel_workload_us : 0.0;
		const double query_only_speedup = workload.accel_hit_median_us > 0.0
		                                      ? base_kernel_median_us / workload.accel_hit_median_us
		                                      : 0.0;
		const double blended_query_us = workload.accel_hit_rate * workload.accel_hit_median_us +
		                                workload.fallback_rate *
		                                    (workload.fallback_median_us > 0.0 ? workload.fallback_median_us
		                                                                       : base_kernel_median_us);
		const double blended_query_speedup = blended_query_us > 0.0 ? base_kernel_median_us / blended_query_us : 0.0;

		std::cout << "suite,write_policy,locality,write_frequency_pct,bench_group,bench_trace,num_queries,writes_executed,"
		             "duckdb_ref_query_median_us,base_kernel_median_us,base_write_median_us,"
		             "accel_write_median_us,accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,"
		             "total_duckdb_query_us,total_accel_query_us,total_base_write_us,total_accel_write_us,"
		             "duckdb_workload_us,accel_workload_us,cold_start_accel_workload_us,workload_speedup,"
		             "cold_start_workload_speedup,query_only_speedup,blended_query_speedup,"
		             "setup_full_build_ms,partition_rebuild_ms,auxiliary_hits,fallback_queries\n";

		const auto accel_write_median_us = workload.writes_executed > 0
		                                       ? workload.total_accel_write_us / workload.writes_executed
		                                       : 0.0;

		std::cout << suite << ',' << kWritePolicy << ',' << locality << ',' << write_frequency_pct << ',' << bench_group
		          << ',' << bench_trace << ',' << workload.num_queries << ',' << workload.writes_executed << ','
		          << std::fixed << std::setprecision(2) << workload.duckdb_ref_query_median_us << ','
		          << base_kernel_median_us << ',' << workload.base_write_median_us << ',' << accel_write_median_us << ','
		          << workload.accel_hit_median_us << ',' << workload.fallback_median_us << ',' << workload.accel_hit_rate
		          << ',' << workload.fallback_rate << ',' << workload.total_duckdb_query_us << ','
		          << workload.total_accel_query_us << ',' << workload.total_base_write_us << ','
		          << workload.total_accel_write_us << ',' << duckdb_workload_us << ',' << accel_workload_us << ','
		          << cold_start_accel_workload_us << ',' << workload_speedup << ',' << cold_start_workload_speedup << ','
		          << query_only_speedup << ',' << blended_query_speedup << ',' << setup_full_build_ms << ','
		          << partition_rebuild_ms << ',' << workload.auxiliary_hits << ',' << workload.fallback_queries << '\n';

		std::cout << "speedup_gate wall_time_base_us=" << std::fixed << std::setprecision(2) << base_kernel_median_us
		          << '\n';
		std::cout << "speedup_gate duckdb_ref_median_us=" << duckdb_ref_query_median_us << '\n';
		std::cout << "speedup_gate median_us=" << workload.accel_hit_median_us << '\n';
		std::cout << "speedup_gate workload_speedup=" << workload_speedup << '\n';
		std::cout << "speedup_gate cold_start_workload_speedup=" << cold_start_workload_speedup << '\n';
		std::cout << "speedup_gate query_only_speedup=" << query_only_speedup << '\n';
		std::cout << "speedup_gate accel_hit_rate=" << workload.accel_hit_rate << '\n';

		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_sec_edgar_q1_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
