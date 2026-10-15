//! DuckDB (matched threads) vs accel_tpch_q17_parallel across manifest seeds.

#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q17/tests/q17_manifest.hpp"
#include "tpch/q17/tests/q17_nonzero_manifest.hpp"

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

#ifndef ACCEL_ENABLE_PARALLEL_KERNELS
#error "benchmark_q17_parallel requires ACCEL_ENABLE_PARALLEL_KERNELS"
#endif

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

bool EnvEnabled(const char *name) {
	const auto value = GetEnvString(name);
	return value == "1" || value == "true" || value == "TRUE";
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

std::string EscapeLiteral(const std::string &value) {
	std::string out;
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
	Require(result != nullptr && !result->HasError(), result ? result->GetError() : "null");
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

struct DualTiming {
	double duckdb_median_us = 0.0;
	double accel_median_us = 0.0;
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
	auto median = [](std::vector<double> samples) {
		std::sort(samples.begin(), samples.end());
		const auto middle = samples.size() / 2;
		return samples.size() % 2 == 0 ? (samples[middle - 1] + samples[middle]) / 2.0 : samples[middle];
	};
	DualTiming timing;
	timing.duckdb_median_us = median(std::move(duck_samples));
	timing.accel_median_us = median(std::move(accel_samples));
	return timing;
}

double Geomean(const std::vector<double> &values) {
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
		int threads = GetEnvInt("ACCEL_BENCH_THREADS", 1);
		for (int i = 1; i < argc; i++) {
			if (std::string(argv[i]) == "--threads" && i + 1 < argc) {
				threads = std::stoi(argv[++i]);
			}
		}
		const int warmup = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int measured = GetEnvInt("ACCEL_BENCH_ITERS", 5);
		const int seed_limit = GetEnvInt("ACCEL_BENCH_SEED_LIMIT", 20);
		const bool nonzero_only = EnvEnabled("ACCEL_BENCH_Q17_NONZERO");
		Require(warmup >= 3, "ACCEL_BENCH_WARMUP must be >= 3");
		Require(measured >= 5, "ACCEL_BENCH_ITERS must be >= 5");

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		Require(!connection.Query("PRAGMA threads=" + std::to_string(threads))->HasError(), "set threads");

		std::cout << "seed,threads,duckdb_median_us,accel_median_us,speedup\n";
		std::vector<double> speedups;

		const auto run_entry = [&](const accelerator_codegen::test_support::Q17ManifestEntry &entry) {
			const auto duck_sql = entry.sql_path != nullptr
			                          ? ReadFile(entry.sql_path)
			                          : accelerator_codegen::test_support::Q17ReferenceSql(entry.brand, entry.container);
			const auto accel_sql = std::string("SELECT * FROM accel_tpch_q17_parallel('") + EscapeLiteral(entry.brand) +
			                       "', '" + EscapeLiteral(entry.container) + "')";
			RequireEqualScalar(connection, entry.label, duck_sql, accel_sql);
			const auto timing = BenchmarkPair(connection, duck_sql, accel_sql, warmup, measured);
			const auto duck_us = timing.duckdb_median_us;
			const auto accel_us = timing.accel_median_us;
			const auto speedup = duck_us / accel_us;
			speedups.push_back(speedup);
			std::cout << entry.label << ',' << threads << ',' << std::fixed << std::setprecision(2) << duck_us << ','
			          << accel_us << ',' << speedup << '\n';
		};

		if (nonzero_only) {
			for (const auto &entry : accelerator_codegen::test_support::kQ37NonzeroManifest) {
				run_entry(entry);
			}
		} else {
			for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ17Manifest.size());
			     i++) {
				run_entry(accelerator_codegen::test_support::kQ17Manifest[i]);
			}
		}

		Require(!speedups.empty(), "no seeds benchmarked");
		std::cout << "summary_geomean_speedup," << threads << ",,," << std::fixed << std::setprecision(4)
		          << Geomean(speedups) << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q17_parallel failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
