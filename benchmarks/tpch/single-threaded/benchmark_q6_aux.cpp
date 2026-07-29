#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_write_policy.hpp"
#include "test_support/q6_aux_helpers.hpp"
#include "test_support/q6_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

struct LineitemTarget {
	int64_t orderkey = 0;
	int64_t linenumber = 0;
};

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
	ExecuteAndConsume(connection, "SELECT * FROM accel_q6_aux_build()");
	const auto end = Clock::now();
	return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

double TimePartitionRebuildMs(duckdb::Connection &connection, const std::string &start_date) {
	const auto start = Clock::now();
	ExecuteAndConsume(connection, "SELECT * FROM accel_q6_aux_rebuild_partition(DATE '" + start_date + "')");
	const auto end = Clock::now();
	return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

std::string QueryYearFilter(const std::string &start_date) {
	return "l_shipdate >= DATE '" + start_date + "' AND l_shipdate < DATE '" + start_date + "' + INTERVAL '1' YEAR";
}

std::vector<LineitemTarget> PrecomputeWriteTargets(duckdb::Connection &connection, const std::string &start_date) {
	auto result = connection.Query(
	    "SELECT l_orderkey, l_linenumber FROM lineitem WHERE " + QueryYearFilter(start_date) +
	    " ORDER BY l_orderkey, l_linenumber LIMIT 256");
	Require(result != nullptr && !result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	std::vector<LineitemTarget> targets;
	targets.reserve(materialized.RowCount());
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		LineitemTarget target;
		target.orderkey = materialized.GetValue(0, row).GetValue<int64_t>();
		target.linenumber = materialized.GetValue(1, row).GetValue<int64_t>();
		targets.push_back(target);
	}
	Require(!targets.empty(), "failed to precompute lineitem write targets");
	return targets;
}

const LineitemTarget &PickWriteTarget(const std::vector<LineitemTarget> &targets, int seq) {
	return targets[static_cast<size_t>(seq) % targets.size()];
}

void ApplyWriteSuite(duckdb::Connection &connection, const std::string &suite, std::mt19937 &rng, int write_count,
                     int write_seq, const std::string &locality, const std::string &bench_start_date,
                     const std::vector<LineitemTarget> &targets) {
	std::uniform_int_distribution<int> bucket_dist(0, 99);
	for (int i = 0; i < write_count; i++) {
		const int seq = write_seq + i;
		if (suite != "lineitem_only") {
			throw std::runtime_error("unsupported bench suite: " + suite);
		}
		const auto &target = PickWriteTarget(targets, seq);
		const std::string where = "l_orderkey = " + std::to_string(target.orderkey) + " AND l_linenumber = " +
		                            std::to_string(target.linenumber);
		const int bucket = bucket_dist(rng);
		const std::string year_prefix = locality == "single_year" ? bench_start_date.substr(0, 4) : "1995";
		const std::string shipdate = year_prefix + "-03-" + std::to_string((seq % 28) + 1);
		(void)locality;
		if (bucket < 25) {
			ExecuteAndConsume(connection, "UPDATE lineitem SET l_comment = l_comment WHERE " + where);
		} else if (bucket < 60) {
			ExecuteAndConsume(connection,
			                  "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0001, l_discount = l_discount "
			                  "WHERE " +
			                      where);
		} else if (bucket < 80) {
			ExecuteAndConsume(connection, "UPDATE lineitem SET l_shipdate = DATE '" + shipdate + "' WHERE " + where);
		} else if (bucket < 90) {
			ExecuteAndConsume(connection,
			                  "INSERT INTO lineitem "
			                  "SELECT " +
			                      std::to_string(9000000000LL + seq) +
			                      ", l_partkey, l_suppkey, l_linenumber + 100, l_quantity, l_extendedprice, "
			                      "l_discount, l_tax, l_returnflag, l_linestatus, DATE '" +
			                      shipdate +
			                      "', l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment "
			                      "FROM lineitem WHERE " +
			                      where);
		} else {
			ExecuteAndConsume(connection, "DELETE FROM lineitem WHERE " + where);
		}
	}
}

double TimeWriteCommitUs(duckdb::Connection &connection, const std::string &suite, std::mt19937 &rng, int write_seq,
                         const std::string &locality, const std::string &bench_start_date,
                         const std::vector<LineitemTarget> &targets) {
	const auto start = Clock::now();
	connection.BeginTransaction();
	ApplyWriteSuite(connection, suite, rng, 1, write_seq, locality, bench_start_date, targets);
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
	double base_kernel_median_us = 0.0;
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

std::string AuxSqlForQueryIndex(int query_index) {
	const auto &entry =
	    accelerator_codegen::test_support::kQ6Manifest[static_cast<size_t>(query_index) %
	                                                   accelerator_codegen::test_support::kQ6Manifest.size()];
	return accelerator_codegen::test_support::AcceleratorQ6AuxSql(entry.start_date, entry.discount, entry.quantity);
}

WorkloadTimings RunMeasuredWorkload(duckdb::Connection &connection, const std::string &suite,
                                    const std::string &locality, const std::string &bench_start_date,
                                    int num_queries, int write_frequency_pct, std::mt19937 &rng, int warmup_queries,
                                    const std::vector<LineitemTarget> &targets) {
	WorkloadTimings result;
	result.num_queries = num_queries;

	for (int i = 0; i < warmup_queries; i++) {
		ExecuteAndConsume(connection, AuxSqlForQueryIndex(i));
	}

	const auto counters_start = accelerator_codegen::GetQ6AuxCounters();
	int write_seq = 0;
	int accel_hits = 0;
	int fallbacks = 0;
	std::vector<double> hit_samples;
	std::vector<double> fallback_samples;
	std::vector<double> write_samples;

	for (int query_index = 0; query_index < num_queries; query_index++) {
		if (ShouldWriteAtQueryIndex(query_index, write_frequency_pct)) {
			write_samples.push_back(
			    TimeWriteCommitUs(connection, suite, rng, write_seq, locality, bench_start_date, targets));
			write_seq++;
			result.writes_executed++;
		}

		const auto query_start = Clock::now();
		ExecuteAndConsume(connection, AuxSqlForQueryIndex(query_index));
		const auto query_end = Clock::now();
		const auto sample = ElapsedUs(query_start, query_end);
		result.total_accel_query_us += sample;

		const auto &diag = accelerator_codegen::GetQ6AuxDiagnostics();
		if (diag.used_aux_path) {
			accel_hits++;
			hit_samples.push_back(sample);
		} else {
			fallbacks++;
			fallback_samples.push_back(sample);
		}
	}

	const auto counters_end = accelerator_codegen::GetQ6AuxCounters();
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

double MeasureBaseKernelMedianUs(duckdb::Connection &connection, const std::string &base_sql, int samples) {
	std::vector<double> query_samples;
	query_samples.reserve(samples);
	for (int i = 0; i < samples; i++) {
		const auto start = Clock::now();
		ExecuteAndConsume(connection, base_sql);
		query_samples.push_back(ElapsedUs(start, Clock::now()));
	}
	return SummarizeSamples(std::move(query_samples)).median_us;
}

double MeasureBaseWriteMedianUs(duckdb::Connection &connection, const std::string &suite, const std::string &locality,
                                const std::string &bench_start_date, int samples, std::mt19937 &rng,
                                const std::vector<LineitemTarget> &targets) {
	std::vector<double> write_samples;
	write_samples.reserve(samples);
	for (int i = 0; i < samples; i++) {
		write_samples.push_back(
		    TimeWriteCommitUs(connection, suite, rng, 100000 + i, locality, bench_start_date, targets));
	}
	return SummarizeSamples(std::move(write_samples)).median_us;
}

void EnsureHybridMaintenanceMode() {
	if (!std::getenv("ACCEL_Q6_MAINTENANCE_MODE")) {
		setenv("ACCEL_Q6_MAINTENANCE_MODE", "hybrid", 0);
	}
}

} // namespace

int main() {
	try {
		EnsureHybridMaintenanceMode();

		const int warmup_queries = GetEnvInt("ACCEL_BENCH_WARMUP", 10);
		const int measured_queries = GetEnvInt("ACCEL_BENCH_QUERIES", 100);
		const int ref_samples = GetEnvInt("ACCEL_BENCH_REF_SAMPLES", 5);
		const int base_write_samples = GetEnvInt("ACCEL_BENCH_BASE_WRITE_SAMPLES", 3);
		const auto suite = GetEnvString("ACCEL_Q6_BENCH_SUITE", "lineitem_only");
		constexpr const char *kWritePolicy = "hybrid";
		const auto locality = GetEnvString("ACCEL_Q6_WRITE_LOCALITY", "single_year");
		const int write_frequency_pct = GetEnvInt("ACCEL_Q6_WRITE_FREQUENCY_PCT", 0);
		const auto bench_start_date = GetEnvString("ACCEL_Q6_BENCH_START_DATE", "1996-01-01");
		const auto bench_discount = GetEnvString("ACCEL_Q6_BENCH_DISCOUNT", "0.06");
		const auto bench_quantity = GetEnvString("ACCEL_Q6_BENCH_QUANTITY", "24.00");

		const auto db_path = GetTpchDbPath();

		duckdb::DuckDB database(db_path.c_str());
		duckdb::Connection base_connection(database);
		duckdb::Connection accel_connection(database);
		Require(base_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
		Require(accel_connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");

		const auto reference_sql =
		    accelerator_codegen::test_support::ReferenceQ6Sql(bench_start_date, bench_discount, bench_quantity);
		const auto base_sql =
		    "SELECT revenue FROM accel_tpch_q6(DATE '" + bench_start_date + "', " + bench_discount + ", " +
		    bench_quantity + ")";
		const auto write_targets = PrecomputeWriteTargets(base_connection, bench_start_date);

		std::mt19937 rng(42);
		const double base_write_median_us = MeasureBaseWriteMedianUs(base_connection, suite, locality, bench_start_date,
		                                                           base_write_samples, rng, write_targets);

		accelerator_codegen::RegisterAcceleratorFunctions(accel_connection);
		const double setup_full_build_ms = TimeBuildMs(accel_connection);
		const double partition_rebuild_ms =
		    GetEnvInt("ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB", 0) != 0
		        ? 0.0
		        : TimePartitionRebuildMs(accel_connection, bench_start_date);

		const double duckdb_ref_query_median_us =
		    MeasureReferenceQueryMedianUs(accel_connection, reference_sql, ref_samples);
		const double base_kernel_median_us = MeasureBaseKernelMedianUs(accel_connection, base_sql, ref_samples);

		accelerator_codegen::ResetQ6AuxCounters();
		auto workload = RunMeasuredWorkload(accel_connection, suite, locality, bench_start_date, measured_queries,
		                                    write_frequency_pct, rng, warmup_queries, write_targets);
		workload.duckdb_ref_query_median_us = duckdb_ref_query_median_us;
		workload.base_kernel_median_us = base_kernel_median_us;
		workload.base_write_median_us = base_write_median_us;
		workload.total_duckdb_query_us = duckdb_ref_query_median_us * static_cast<double>(measured_queries);
		workload.total_base_write_us = base_write_median_us * static_cast<double>(workload.writes_executed);

		const double duckdb_workload_us = workload.total_duckdb_query_us + workload.total_base_write_us;
		const double accel_workload_us = workload.total_accel_query_us + workload.total_accel_write_us;
		const double workload_speedup = accel_workload_us > 0.0 ? duckdb_workload_us / accel_workload_us : 0.0;
		const double query_only_speedup = workload.accel_hit_median_us > 0.0
		                                      ? base_kernel_median_us / workload.accel_hit_median_us
		                                      : 0.0;
		const double blended_query_us = workload.accel_hit_rate * workload.accel_hit_median_us +
		                                workload.fallback_rate *
		                                    (workload.fallback_median_us > 0.0 ? workload.fallback_median_us
		                                                                       : base_kernel_median_us);
		const double blended_query_speedup = blended_query_us > 0.0 ? base_kernel_median_us / blended_query_us : 0.0;

		std::cout << "suite,write_policy,locality,write_frequency_pct,start_date,discount,quantity,num_queries,"
		             "writes_executed,duckdb_ref_query_median_us,base_kernel_median_us,base_write_median_us,"
		             "accel_write_median_us,accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,"
		             "total_duckdb_query_us,total_accel_query_us,total_base_write_us,total_accel_write_us,"
		             "duckdb_workload_us,accel_workload_us,workload_speedup,query_only_speedup,blended_query_speedup,"
		             "setup_full_build_ms,partition_rebuild_ms,auxiliary_hits,fallback_queries\n";

		const auto accel_write_median_us = workload.writes_executed > 0
		                                       ? workload.total_accel_write_us / workload.writes_executed
		                                       : 0.0;

		std::cout << suite << ',' << kWritePolicy << ',' << locality << ',' << write_frequency_pct << ','
		          << bench_start_date << ',' << bench_discount << ',' << bench_quantity << ',' << workload.num_queries
		          << ',' << workload.writes_executed << ',' << std::fixed << std::setprecision(2)
		          << workload.duckdb_ref_query_median_us << ',' << base_kernel_median_us << ','
		          << workload.base_write_median_us << ',' << accel_write_median_us << ',' << workload.accel_hit_median_us
		          << ',' << workload.fallback_median_us << ',' << workload.accel_hit_rate << ',' << workload.fallback_rate
		          << ',' << workload.total_duckdb_query_us << ',' << workload.total_accel_query_us << ','
		          << workload.total_base_write_us << ',' << workload.total_accel_write_us << ',' << duckdb_workload_us
		          << ',' << accel_workload_us << ',' << workload_speedup << ',' << query_only_speedup << ','
		          << blended_query_speedup << ',' << setup_full_build_ms << ',' << partition_rebuild_ms << ','
		          << workload.auxiliary_hits << ',' << workload.fallback_queries << '\n';

		std::cout << "speedup_gate wall_time_base_us=" << std::fixed << std::setprecision(2) << base_kernel_median_us
		          << '\n';
		std::cout << "speedup_gate median_us=" << workload.accel_hit_median_us << '\n';
		std::cout << "speedup_gate query_only_speedup=" << query_only_speedup << '\n';
		std::cout << "speedup_gate accel_hit_rate=" << workload.accel_hit_rate << '\n';

		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q6_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
