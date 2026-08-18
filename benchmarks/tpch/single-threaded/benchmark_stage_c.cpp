#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "test_support/q15_manifest.hpp"
#include "test_support/q21_manifest.hpp"
#include "test_support/q20_manifest.hpp"
#include "test_support/q8_manifest.hpp"
#include "test_support/q2_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

std::string AcceleratorQ15Sql(const accelerator_codegen::test_support::Q15ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q15(DATE '") + entry.start_date + "')";
}

std::string AcceleratorQ21Sql(const accelerator_codegen::test_support::Q21ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q21('") + entry.nation_name + "')";
}

std::string AcceleratorQ20Sql(const accelerator_codegen::test_support::Q20ManifestEntry &entry) {
	std::ostringstream stream;
	stream << "SELECT * FROM accel_tpch_q20(DATE '" << entry.start_date << "', '" << entry.part_name_pattern
	       << "', '" << entry.nation_name << "')";
	return stream.str();
}

std::string AcceleratorQ8Sql(const accelerator_codegen::test_support::Q8ManifestEntry &entry) {
	std::ostringstream stream;
	stream << "SELECT * FROM accel_tpch_q8('" << entry.nation_name << "', '" << entry.region_name << "', '"
	       << entry.part_type << "')";
	return stream.str();
}

std::string AcceleratorQ2Sql(const accelerator_codegen::test_support::Q2ManifestEntry &entry) {
	std::ostringstream stream;
	stream << "SELECT * FROM accel_tpch_q2(" << entry.part_size << ", '" << entry.part_type_pattern << "', '"
	       << entry.region_name << "')";
	return stream.str();
}

std::vector<std::string> MaterializeOrderedRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
	rows.reserve(result.RowCount());
	for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
		std::string encoded;
		for (duckdb::idx_t col = 0; col < result.ColumnCount(); col++) {
			if (col != 0) {
				encoded += " | ";
			}
			encoded += result.GetValue(col, row).ToString();
		}
		rows.push_back(std::move(encoded));
	}
	return rows;
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + ": reference failed: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + ": accelerator failed: " + actual->GetError());
	Require(expected->names == actual->names, label + ": names differ");
	Require(expected->types == actual->types, label + ": types differ");
	Require(MaterializeOrderedRows(expected->Cast<duckdb::MaterializedQueryResult>()) ==
	            MaterializeOrderedRows(actual->Cast<duckdb::MaterializedQueryResult>()),
	        label + ": rows differ");
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
};

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
	TimingStats stats;
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

struct BenchmarkCase {
	const char *query;
	const char *label;
	std::string reference_sql;
	std::string accelerator_sql;
};

void RunBenchmarkSuite(duckdb::Connection &connection, const std::vector<BenchmarkCase> &cases, int warmup_iterations,
                       int measured_iterations, const std::vector<std::string> &filters) {
	std::cout << "query,label,reference_mean_us,reference_median_us,accelerator_mean_us,accelerator_median_us,"
	             "speedup_median\n";

	int rows = 0;
	double total_reference_median_us = 0.0;
	double total_accelerator_median_us = 0.0;

	for (const auto &entry : cases) {
		if (!MatchesFilter(entry.label, filters)) {
			continue;
		}

		RequireEqualResults(connection, entry.label, entry.reference_sql, entry.accelerator_sql);

		TimingStats reference_stats;
		TimingStats accelerator_stats;
		if ((rows % 2) == 0) {
			reference_stats = BenchmarkQuery(connection, entry.reference_sql, warmup_iterations, measured_iterations);
			accelerator_stats = BenchmarkQuery(connection, entry.accelerator_sql, warmup_iterations, measured_iterations);
		} else {
			accelerator_stats = BenchmarkQuery(connection, entry.accelerator_sql, warmup_iterations, measured_iterations);
			reference_stats = BenchmarkQuery(connection, entry.reference_sql, warmup_iterations, measured_iterations);
		}

		const auto speedup_median = reference_stats.median_us / accelerator_stats.median_us;
		rows += 1;
		total_reference_median_us += reference_stats.median_us;
		total_accelerator_median_us += accelerator_stats.median_us;

		std::cout << entry.query << ',' << entry.label << ',' << std::fixed << std::setprecision(2)
		          << reference_stats.mean_us << ',' << reference_stats.median_us << ',' << accelerator_stats.mean_us
		          << ',' << accelerator_stats.median_us << ',' << speedup_median << '\n';
	}

	Require(rows > 0, "benchmark filter selected no instances");
	const auto average_reference_median_us = total_reference_median_us / static_cast<double>(rows);
	const auto average_accelerator_median_us = total_accelerator_median_us / static_cast<double>(rows);
	const auto average_speedup_median = average_reference_median_us / average_accelerator_median_us;
	std::cout << "summary_avg_median,,," << std::fixed << std::setprecision(2) << average_reference_median_us << ",,"
	          << average_accelerator_median_us << ",," << average_speedup_median << '\n';
}

} // namespace

int main() {
	try {
		const int warmup_iterations = GetEnvInt("ACCEL_BENCH_WARMUP", 2);
		const int measured_iterations = GetEnvInt("ACCEL_BENCH_ITERS", 5);
		const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));
		const auto query_filter = GetEnvString("ACCEL_BENCH_QUERY");

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma_result = connection.Query("PRAGMA threads=1");
		Require(pragma_result != nullptr && !pragma_result->HasError(), "failed to set PRAGMA threads=1");

		std::vector<BenchmarkCase> cases;
		if (query_filter.empty() || query_filter == "Q15") {
			for (const auto &entry : accelerator_codegen::test_support::kQ15Manifest) {
				cases.push_back({"Q15", entry.label, ReadFile(entry.sql_path), AcceleratorQ15Sql(entry)});
			}
		}
		if (query_filter.empty() || query_filter == "Q21") {
			for (const auto &entry : accelerator_codegen::test_support::kQ21Manifest) {
				cases.push_back({"Q21", entry.label, ReadFile(entry.sql_path), AcceleratorQ21Sql(entry)});
			}
		}
		if (query_filter.empty() || query_filter == "Q20") {
			for (const auto &entry : accelerator_codegen::test_support::kQ20Manifest) {
				cases.push_back({"Q20", entry.label, ReadFile(entry.sql_path), AcceleratorQ20Sql(entry)});
			}
		}
		if (query_filter.empty() || query_filter == "Q8") {
			for (const auto &entry : accelerator_codegen::test_support::kQ8Manifest) {
				cases.push_back({"Q8", entry.label, ReadFile(entry.sql_path), AcceleratorQ8Sql(entry)});
			}
		}
		if (query_filter.empty() || query_filter == "Q2") {
			for (const auto &entry : accelerator_codegen::test_support::kQ2Manifest) {
				cases.push_back({"Q2", entry.label, ReadFile(entry.sql_path), AcceleratorQ2Sql(entry)});
			}
		}

		RunBenchmarkSuite(connection, cases, warmup_iterations, measured_iterations, filters);
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_stage_c failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
