#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/register.hpp"
#include "sec_edgar/q2/duckdb/base/query.hpp"
#include "sec_edgar/q2/duckdb/base/fused.hpp"
#include "eval/tests/test_support/result_compare.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

constexpr const char *kReferenceSqlPath = "workloads/sec_edgar/instances/seed_1/02.sql";

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return std::stoi(value);
}

bool GetEnvBool(const char *name) {
	const char *value = std::getenv(name);
	return value && value[0] == '1';
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string ReadFile(const std::string &path) {
	return accelerator_codegen::test_support::ReadFile(path);
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			(void)materialized.GetValue(col, row);
		}
	}
}

struct TimingStats {
	double mean_us = 0.0;
	double median_us = 0.0;
};

TimingStats SummarizeSamples(std::vector<double> samples_us) {
	TimingStats stats;
	Require(!samples_us.empty(), "no timing samples");
	for (const auto sample : samples_us) {
		stats.mean_us += sample;
	}
	stats.mean_us /= static_cast<double>(samples_us.size());
	std::sort(samples_us.begin(), samples_us.end());
	const auto middle = samples_us.size() / 2;
	stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
	                                              : samples_us[middle];
	return stats;
}

TimingStats BenchmarkQuery(duckdb::Connection &connection, const std::string &sql, int warmup_iterations,
                           int measured_iterations) {
	for (int i = 0; i < warmup_iterations; i++) {
		ExecuteAndConsume(connection, sql);
	}
	std::vector<double> samples_us;
	samples_us.reserve(measured_iterations);
	for (int i = 0; i < measured_iterations; i++) {
		const auto start = Clock::now();
		ExecuteAndConsume(connection, sql);
		const auto end = Clock::now();
		samples_us.push_back(
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
	}
	return SummarizeSamples(std::move(samples_us));
}

template <class Callback>
void WithTransaction(duckdb::Connection &connection, Callback &&callback) {
	const bool started_transaction = !connection.HasActiveTransaction();
	if (started_transaction) {
		connection.BeginTransaction();
	}
	try {
		callback(*connection.context);
		if (started_transaction) {
			connection.Commit();
		}
	} catch (...) {
		if (started_transaction && connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
}

struct PhaseProfile {
	accelerator_codegen::SecEdgarQ2ProfileCounters counters;
	double pre_scan_us = 0.0;
	double pre_consume_us = 0.0;
	double finalize_us = 0.0;
	double total_us = 0.0;
};

PhaseProfile ProfileSecEdgarQ2Execution(duckdb::Connection &connection,
                                        const accelerator_codegen::SecEdgarQ2Parameters &parameters) {
	PhaseProfile profile;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		const auto total_start = Clock::now();
		auto scan_bind = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateSecEdgarQ2NumScanSpec());
		auto kernel = accelerator_codegen::CreateSecEdgarQ2FusedProfilingKernel(parameters, profile.counters);
		auto &fused_kernel = dynamic_cast<accelerator_codegen::SecEdgarQ2FusedKernel &>(*kernel);
		fused_kernel.RunFusedPreScan(context, *scan_bind);
		profile.pre_scan_us = static_cast<double>(profile.counters.pre_scan_us);
		profile.pre_consume_us = static_cast<double>(profile.counters.pre_consume_us);
		const auto finalize_start = Clock::now();
		kernel->Finalize();
		duckdb::DataChunk output;
		output.Initialize(context, {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DOUBLE});
		while (kernel->ProduceOutput(output)) {
			output.Reset();
		}
		profile.finalize_us =
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - finalize_start).count());
		profile.total_us =
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - total_start).count());
	});
	return profile;
}

} // namespace

int main() {
	try {
		const auto db_path = accelerator_codegen::test_support::SecEdgarDbPath();
		const int warmup = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int iters = GetEnvInt("ACCEL_BENCH_ITERS", 10);
		const bool profile_mode = GetEnvBool("ACCEL_BENCH_PROFILE");
		const auto reference_sql = ReadFile(kReferenceSqlPath);
		const std::string accelerator_sql = "SELECT * FROM accel_sec_edgar_q2()";

		duckdb::DuckDB database(db_path);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		Require(connection.Query("PRAGMA threads=1") != nullptr, "failed to set PRAGMA threads=1");

		const auto reference_stats = BenchmarkQuery(connection, reference_sql, warmup, iters);
		const auto accelerator_stats = BenchmarkQuery(connection, accelerator_sql, warmup, iters);
		const auto speedup_mean = reference_stats.mean_us / accelerator_stats.mean_us;
		const auto speedup_median = reference_stats.median_us / accelerator_stats.median_us;

		std::cout << "label,reference_mean_us,reference_median_us,accelerator_mean_us,accelerator_median_us,speedup_mean,speedup_median\n";
		std::cout << std::fixed << std::setprecision(2) << "q2," << reference_stats.mean_us << ','
		          << reference_stats.median_us << ',' << accelerator_stats.mean_us << ',' << accelerator_stats.median_us
		          << ',' << speedup_mean << ',' << speedup_median << "\n";
		std::cout << "summary_avg_median," << reference_stats.median_us << ',' << reference_stats.median_us << ','
		          << accelerator_stats.median_us << ',' << accelerator_stats.median_us << ',' << speedup_median << ','
		          << speedup_median << "\n";

		if (profile_mode) {
			accelerator_codegen::SecEdgarQ2Parameters parameters;
			WithTransaction(connection, [&](duckdb::ClientContext &context) {
				parameters.row_estimate = accelerator_codegen::GetNativeScanTableRowCount(
				    context, accelerator_codegen::CreateSecEdgarQ2NumScanSpec());
			});
			const auto profile = ProfileSecEdgarQ2Execution(connection, parameters);
			const auto &c = profile.counters;
			std::cout << "label,reference_median_us,accelerator_median_us,speedup_median,pre_scan_us,sub_scan_us,num_max_us,"
			             "num_collect_us,pre_consume_us,finalize_us,qualifying_rows,pre_rows,generic_rows,flat_chunks,dict_chunks,"
			             "generic_chunks,distinct_inserts,active_groups,candidate_rows\n";
			std::cout << "profile," << reference_stats.median_us << ',' << accelerator_stats.median_us << ','
			          << speedup_median << ',' << profile.pre_scan_us << ',' << c.sub_scan_us << ',' << c.num_max_us << ','
			          << c.num_collect_us << ',' << profile.pre_consume_us << ',' << profile.finalize_us << ','
			          << c.qualifying_rows << ',' << c.pre_rows << ',' << c.generic_fallback_rows << ',' << c.flat_chunks
			          << ',' << c.dict_chunks << ',' << c.generic_chunks << ',' << c.distinct_inserts << ',' << c.active_groups
			          << ',' << c.candidate_rows << "\n";
		}

		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_sec_edgar_q2 failed: " << ex.what() << '\n';
		return 1;
	}
}
