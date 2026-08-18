#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "test_support/q1_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

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

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	volatile std::size_t sink = 0;
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			sink += materialized.GetValue(col, row).IsNull() ? 0 : 1;
		}
	}
	Require(sink != static_cast<std::size_t>(-1), "unreachable consume guard");
}

struct TimingStats {
	double average_us = 0.0;
	double median_us = 0.0;
};

template <class Callback>
TimingStats BenchmarkOperation(int warmup_iterations, int measured_iterations, Callback &&callback) {
	for (int i = 0; i < warmup_iterations; i++) {
		callback();
	}
	std::vector<double> samples_us;
	samples_us.reserve(measured_iterations);
	for (int i = 0; i < measured_iterations; i++) {
		const auto start = Clock::now();
		callback();
		const auto end = Clock::now();
		samples_us.push_back(
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
	}
	TimingStats stats;
	for (const auto sample : samples_us) {
		stats.average_us += sample;
	}
	stats.average_us /= static_cast<double>(samples_us.size());
	std::sort(samples_us.begin(), samples_us.end());
	const auto middle = samples_us.size() / 2;
	stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
	                                              : samples_us[middle];
	return stats;
}

std::string AcceleratorQ1Sql(const accelerator_codegen::test_support::Q1ManifestEntry &entry) {
	return "SELECT * FROM accel_tpch_q1(" + std::to_string(entry.day_offset) + ")";
}

double MedianOfVector(std::vector<double> values) {
	std::sort(values.begin(), values.end());
	const auto middle = values.size() / 2;
	return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
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
		connection.Query("PRAGMA threads=1");

		std::unordered_map<std::string, std::vector<double>> median_by_path;
		std::cout << "label,path,average_us,median_us,speedup_median\n";
		for (const auto &entry : accelerator_codegen::test_support::kQ1Manifest) {
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql = AcceleratorQ1Sql(entry);
			const auto reference =
			    BenchmarkOperation(warmup_iterations, measured_iterations,
			                       [&]() { ExecuteAndConsume(connection, reference_sql); });
			const auto accel = BenchmarkOperation(warmup_iterations, measured_iterations,
			                                      [&]() { ExecuteAndConsume(connection, accel_sql); });
			const auto speedup = reference.median_us / accel.median_us;
			std::cout << entry.label << ",duckdb_reference_q1," << std::fixed << std::setprecision(2)
			          << reference.average_us << ',' << reference.median_us << ",\n";
			std::cout << entry.label << ",accel_fused_q1," << accel.average_us << ',' << accel.median_us << ','
			          << speedup << "\n";
			median_by_path["duckdb_reference_q1"].push_back(reference.median_us);
			median_by_path["accel_fused_q1"].push_back(accel.median_us);
		}
		Require(!median_by_path.empty(), "benchmark filter selected no Q1 instances");
		const auto duck = MedianOfVector(median_by_path["duckdb_reference_q1"]);
		const auto accel = MedianOfVector(median_by_path["accel_fused_q1"]);
		std::cout << "summary_median,duckdb_reference_q1,," << duck << ",\n";
		std::cout << "summary_median,accel_fused_q1,," << accel << ',' << (duck / accel) << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q1 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
