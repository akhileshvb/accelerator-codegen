//! DuckDB (matched threads) vs accel_tpch_q{15,17,19}_parallel for Wave 1 queries.

#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q15/tests/q15_manifest.hpp"
#include "tpch/q17/tests/q17_manifest.hpp"
#include "tpch/q19/tests/q19_manifest.hpp"

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
	return accelerator_codegen::test_support::EnvOr("ACCEL_TPCH_DB", accelerator_codegen::test_support::TpchSf10DbPath());
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

std::string EscapeLiteral(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for (char c : value) {
		if (c == '\'') {
			out += "''";
		} else {
			out.push_back(c);
		}
	}
	return out;
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

void RequireEqualScalar(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                        const std::string &accelerator_sql, bool decimal_scalar) {
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
		if (decimal_scalar) {
			Require(ev.GetValueUnsafe<duckdb::hugeint_t>() == av.GetValueUnsafe<duckdb::hugeint_t>(), label + ": value");
		} else {
			Require(DoublesMatch(ev.GetValue<double>(), av.GetValue<double>()), label + ": value");
		}
	}
}

void RequireEqualMultiset(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                          const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference failed: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + " accelerator failed: " + actual->GetError());
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_m.RowCount() == actual_m.RowCount(), label + ": row count");
	std::vector<std::string> lhs;
	std::vector<std::string> rhs;
	for (duckdb::idx_t row = 0; row < expected_m.RowCount(); row++) {
		std::string encoded;
		for (duckdb::idx_t col = 0; col < expected_m.ColumnCount(); col++) {
			if (col != 0) {
				encoded += " | ";
			}
			encoded += expected_m.GetValue(col, row).ToString();
		}
		lhs.push_back(encoded);
	}
	for (duckdb::idx_t row = 0; row < actual_m.RowCount(); row++) {
		std::string encoded;
		for (duckdb::idx_t col = 0; col < actual_m.ColumnCount(); col++) {
			if (col != 0) {
				encoded += " | ";
			}
			encoded += actual_m.GetValue(col, row).ToString();
		}
		rhs.push_back(encoded);
	}
	std::sort(lhs.begin(), lhs.end());
	std::sort(rhs.begin(), rhs.end());
	Require(lhs == rhs, label + ": multiset mismatch");
}

struct TimingStats {
	double median_us = 0.0;
};

TimingStats SummarizeMedian(std::vector<double> samples_us) {
	Require(!samples_us.empty(), "no timing samples");
	std::sort(samples_us.begin(), samples_us.end());
	const auto middle = samples_us.size() / 2;
	TimingStats stats;
	stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
	                                               : samples_us[middle];
	return stats;
}

struct DualTiming {
	TimingStats duckdb;
	TimingStats accelerator;
};

DualTiming BenchmarkPair(duckdb::Connection &connection, const std::string &duckdb_sql, const std::string &accel_sql,
                         int warmup_iterations, int measured_iterations) {
	for (int i = 0; i < warmup_iterations; i++) {
		if ((i % 2) == 0) {
			ExecuteAndConsume(connection, duckdb_sql);
			ExecuteAndConsume(connection, accel_sql);
		} else {
			ExecuteAndConsume(connection, accel_sql);
			ExecuteAndConsume(connection, duckdb_sql);
		}
	}
	std::vector<double> duck_samples;
	std::vector<double> accel_samples;
	duck_samples.reserve(measured_iterations);
	accel_samples.reserve(measured_iterations);
	for (int i = 0; i < measured_iterations; i++) {
		if ((i % 2) == 0) {
			const auto d0 = Clock::now();
			ExecuteAndConsume(connection, duckdb_sql);
			const auto d1 = Clock::now();
			duck_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count()));
			const auto a0 = Clock::now();
			ExecuteAndConsume(connection, accel_sql);
			const auto a1 = Clock::now();
			accel_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(a1 - a0).count()));
		} else {
			const auto a0 = Clock::now();
			ExecuteAndConsume(connection, accel_sql);
			const auto a1 = Clock::now();
			accel_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(a1 - a0).count()));
			const auto d0 = Clock::now();
			ExecuteAndConsume(connection, duckdb_sql);
			const auto d1 = Clock::now();
			duck_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count()));
		}
	}
	DualTiming timing;
	timing.duckdb = SummarizeMedian(std::move(duck_samples));
	timing.accelerator = SummarizeMedian(std::move(accel_samples));
	return timing;
}

struct CaseResult {
	std::string query;
	std::string seed;
	double speedup = 0.0;
};

void RunQueryCases(duckdb::Connection &connection, const std::vector<std::string> &filters, int warmup,
                   int measured, std::vector<CaseResult> &out) {
	for (const auto &entry : accelerator_codegen::test_support::kQ15Manifest) {
		if (!MatchesFilter(entry.label, filters)) {
			continue;
		}
		const auto duck_sql = ReadFile(entry.sql_path);
		const auto accel_sql = std::string("SELECT * FROM accel_tpch_q15_parallel(DATE '") + entry.start_date + "')";
		RequireEqualMultiset(connection, entry.label, duck_sql, accel_sql);
		const auto timing = BenchmarkPair(connection, duck_sql, accel_sql, warmup, measured);
		out.push_back({"Q15", entry.label, timing.duckdb.median_us / timing.accelerator.median_us});
	}
	for (const auto &entry : accelerator_codegen::test_support::kQ17Manifest) {
		if (!MatchesFilter(entry.label, filters)) {
			continue;
		}
		const auto duck_sql = ReadFile(entry.sql_path);
		const auto accel_sql = std::string("SELECT * FROM accel_tpch_q17_parallel('") + EscapeLiteral(entry.brand) +
		                       "', '" + EscapeLiteral(entry.container) + "')";
		RequireEqualScalar(connection, entry.label, duck_sql, accel_sql, false);
		const auto timing = BenchmarkPair(connection, duck_sql, accel_sql, warmup, measured);
		out.push_back({"Q17", entry.label, timing.duckdb.median_us / timing.accelerator.median_us});
	}
	for (const auto &entry : accelerator_codegen::test_support::kQ19Manifest) {
		if (!MatchesFilter(entry.label, filters)) {
			continue;
		}
		const auto duck_sql = ReadFile(entry.sql_path);
		const auto accel_sql = std::string("SELECT * FROM accel_tpch_q19_parallel('") + EscapeLiteral(entry.brand1) +
		                       "', " + std::to_string(entry.qty_low1) + ", '" + EscapeLiteral(entry.brand2) + "', " +
		                       std::to_string(entry.qty_low2) + ", '" + EscapeLiteral(entry.brand3) + "', " +
		                       std::to_string(entry.qty_low3) + ")";
		RequireEqualScalar(connection, entry.label, duck_sql, accel_sql, true);
		const auto timing = BenchmarkPair(connection, duck_sql, accel_sql, warmup, measured);
		out.push_back({"Q19", entry.label, timing.duckdb.median_us / timing.accelerator.median_us});
	}
}

double Geomean(const std::vector<double> &values) {
	Require(!values.empty(), "empty geomean");
	double log_sum = 0.0;
	for (const auto v : values) {
		Require(v > 0.0, "non-positive speedup");
		log_sum += std::log(v);
	}
	return std::exp(log_sum / static_cast<double>(values.size()));
}

} // namespace

int main(int argc, char **argv) {
	try {
#ifndef ACCEL_ENABLE_PARALLEL_KERNELS
		std::cerr << "requires ACCEL_ENABLE_PARALLEL_KERNELS build\n";
		return 1;
#endif
		int threads = GetEnvInt("ACCEL_BENCH_THREADS", 1);
		for (int i = 1; i < argc; i++) {
			if (std::string(argv[i]) == "--threads" && i + 1 < argc) {
				threads = std::stoi(argv[++i]);
			}
		}
		const int warmup = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int measured = GetEnvInt("ACCEL_BENCH_ITERS", 5);
		const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));
		Require(warmup >= 3, "ACCEL_BENCH_WARMUP must be >= 3");
		Require(measured >= 5, "ACCEL_BENCH_ITERS must be >= 5");

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma = connection.Query("PRAGMA threads=" + std::to_string(threads));
		Require(pragma != nullptr && !pragma->HasError(), "failed to set threads");

		std::vector<CaseResult> results;
		RunQueryCases(connection, filters, warmup, measured, results);
		Require(!results.empty(), "no benchmark cases selected");

		std::cout << "query,seed,threads,speedup_vs_duckdb\n";
		std::vector<double> q15, q17, q19;
		for (const auto &r : results) {
			std::cout << r.query << ',' << r.seed << ',' << threads << ',' << std::fixed << std::setprecision(6)
			          << r.speedup << '\n';
			if (r.query == "Q15") {
				q15.push_back(r.speedup);
			} else if (r.query == "Q17") {
				q17.push_back(r.speedup);
			} else {
				q19.push_back(r.speedup);
			}
		}
		if (!q15.empty()) {
			std::cout << "summary,Q15,geomean," << threads << ',' << Geomean(q15) << '\n';
		}
		if (!q17.empty()) {
			std::cout << "summary,Q17,geomean," << threads << ',' << Geomean(q17) << '\n';
		}
		if (!q19.empty()) {
			std::cout << "summary,Q19,geomean," << threads << ',' << Geomean(q19) << '\n';
		}
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_wave1_parallel failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
