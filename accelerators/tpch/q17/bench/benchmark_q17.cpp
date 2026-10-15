#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q17/tests/q17_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
	return accelerator_codegen::test_support::TpchDbPath();
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return std::stoi(value);
}

std::string GetEnvString(const char *name) {
	const char *value = std::getenv(name);
	return value ? value : "";
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string ReadFile(const std::string &path) {
	std::ifstream input(path);
	Require(input.good(), "failed to open file: " + path);
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
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

bool MatchesFilter(const std::string &label, const std::vector<std::string> &filters) {
	if (filters.empty()) {
		return true;
	}
	return std::find(filters.begin(), filters.end(), label) != filters.end();
}

std::string AcceleratorQ17Sql(const accelerator_codegen::test_support::Q17ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q17('") + entry.brand + "', '" + entry.container + "')";
}

bool DoublesMatch(double expected, double actual) {
	if (std::isnan(expected) && std::isnan(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	if (std::isinf(expected) && std::isinf(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	uint64_t expected_bits = 0;
	uint64_t actual_bits = 0;
	std::memcpy(&expected_bits, &expected, sizeof(double));
	std::memcpy(&actual_bits, &actual, sizeof(double));
	return expected_bits == actual_bits;
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference failed: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + " accelerator failed: " + actual->GetError());
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_m.RowCount() == 1 && actual_m.RowCount() == 1, label + ": row count");
	const auto ev = expected_m.GetValue(0, 0);
	const auto av = actual_m.GetValue(0, 0);
	Require(ev.IsNull() == av.IsNull(), label + ": nullability");
	if (!ev.IsNull()) {
		Require(DoublesMatch(ev.GetValue<double>(), av.GetValue<double>()), label + ": value mismatch");
	}
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			(void)materialized.GetValue(col, row).ToString();
		}
	}
}

struct TimingStats {
	double mean_us = 0.0;
	double median_us = 0.0;
	double mad_us = 0.0;
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
	std::vector<double> abs_dev;
	abs_dev.reserve(samples_us.size());
	for (const auto sample : samples_us) {
		abs_dev.push_back(std::abs(sample - stats.median_us));
	}
	std::sort(abs_dev.begin(), abs_dev.end());
	stats.mad_us = abs_dev.size() % 2 == 0 ? (abs_dev[middle - 1] + abs_dev[middle]) / 2.0 : abs_dev[middle];
	return stats;
}

struct DualTiming {
	TimingStats reference;
	TimingStats accelerator;
};

DualTiming BenchmarkPair(duckdb::Connection &connection, const std::string &reference_sql,
                         const std::string &accelerator_sql, int warmup_iterations, int measured_iterations) {
	for (int i = 0; i < warmup_iterations; i++) {
		if ((i % 2) == 0) {
			ExecuteAndConsume(connection, reference_sql);
			ExecuteAndConsume(connection, accelerator_sql);
		} else {
			ExecuteAndConsume(connection, accelerator_sql);
			ExecuteAndConsume(connection, reference_sql);
		}
	}

	std::vector<double> reference_samples;
	std::vector<double> accelerator_samples;
	reference_samples.reserve(measured_iterations);
	accelerator_samples.reserve(measured_iterations);
	for (int i = 0; i < measured_iterations; i++) {
		if ((i % 2) == 0) {
			const auto ref_start = Clock::now();
			ExecuteAndConsume(connection, reference_sql);
			const auto ref_end = Clock::now();
			reference_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(ref_end - ref_start).count()));
			const auto accel_start = Clock::now();
			ExecuteAndConsume(connection, accelerator_sql);
			const auto accel_end = Clock::now();
			accelerator_samples.push_back(static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(accel_end - accel_start).count()));
		} else {
			const auto accel_start = Clock::now();
			ExecuteAndConsume(connection, accelerator_sql);
			const auto accel_end = Clock::now();
			accelerator_samples.push_back(static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(accel_end - accel_start).count()));
			const auto ref_start = Clock::now();
			ExecuteAndConsume(connection, reference_sql);
			const auto ref_end = Clock::now();
			reference_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(ref_end - ref_start).count()));
		}
	}
	DualTiming timing;
	timing.reference = SummarizeSamples(std::move(reference_samples));
	timing.accelerator = SummarizeSamples(std::move(accelerator_samples));
	return timing;
}

} // namespace

int main() {
	try {
		const int warmup_iterations = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int measured_iterations = GetEnvInt("ACCEL_BENCH_ITERS", 10);
		const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma_result = connection.Query("PRAGMA threads=1");
		Require(pragma_result != nullptr && !pragma_result->HasError(), "failed to set PRAGMA threads=1");

		std::cout << "label,reference_mean_us,reference_median_us,reference_mad_us,"
		             "accelerator_mean_us,accelerator_median_us,accelerator_mad_us,speedup_median\n";

		int rows = 0;
		double total_reference_median_us = 0.0;
		double total_accelerator_median_us = 0.0;

		for (const auto &entry : accelerator_codegen::test_support::kQ17Manifest) {
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}

			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accelerator_sql = AcceleratorQ17Sql(entry);
			RequireEqualResults(connection, entry.label, reference_sql, accelerator_sql);

			const auto timing =
			    BenchmarkPair(connection, reference_sql, accelerator_sql, warmup_iterations, measured_iterations);
			const auto speedup_median = timing.reference.median_us / timing.accelerator.median_us;

			rows += 1;
			total_reference_median_us += timing.reference.median_us;
			total_accelerator_median_us += timing.accelerator.median_us;

			std::cout << entry.label << ',' << std::fixed << std::setprecision(2) << timing.reference.mean_us << ','
			          << timing.reference.median_us << ',' << timing.reference.mad_us << ','
			          << timing.accelerator.mean_us << ',' << timing.accelerator.median_us << ','
			          << timing.accelerator.mad_us << ',' << speedup_median << '\n';
		}

		Require(rows > 0, "benchmark filter selected no Q17 instances");
		const auto average_reference_median_us = total_reference_median_us / static_cast<double>(rows);
		const auto average_accelerator_median_us = total_accelerator_median_us / static_cast<double>(rows);
		const auto average_speedup_median = average_reference_median_us / average_accelerator_median_us;
		std::cout << "summary_avg_median," << std::fixed << std::setprecision(2) << average_reference_median_us
		          << ",,," << average_accelerator_median_us << ",,," << average_speedup_median << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q17 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
