//! DuckDB (matched threads) vs accel_tpch_q5_parallel across manifest seeds.

#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/benchmark_paired_timing.hpp"
#include "tpch/q5/tests/q5_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_KERNELS
#error "benchmark_q5_parallel requires ACCEL_ENABLE_PARALLEL_KERNELS"
#endif

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
	const char *env = std::getenv(name);
	if (!env || !*env) {
		return fallback;
	}
	return std::stoi(env);
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

double Geomean(const std::vector<double> &values) {
	double log_sum = 0.0;
	for (const auto v : values) {
		Require(v > 0.0, "non-positive speedup");
		log_sum += std::log(v);
	}
	return std::exp(log_sum / static_cast<double>(values.size()));
}

std::string AcceleratorQ5ParallelSql(const accelerator_codegen::test_support::Q5ManifestEntry &entry) {
	return "SELECT * FROM accel_tpch_q5_parallel('" + EscapeLiteral(entry.region_name) + "', DATE '" +
	       entry.start_date + "')";
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

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		Require(!connection.Query("PRAGMA threads=" + std::to_string(threads))->HasError(), "set threads");

		std::cout << "query,seed,threads,duckdb_median_us,accel_median_us,speedup\n";
		std::vector<double> speedups;

		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ5Manifest.size());
		     i++) {
			const auto &entry = accelerator_codegen::test_support::kQ5Manifest[i];
			const auto duck_sql = ReadFile(entry.sql_path);
			const auto accel_sql = AcceleratorQ5ParallelSql(entry);
			const auto timing =
			    accelerator_codegen::tpch_bench::BenchmarkPair(connection, duck_sql, accel_sql, warmup, measured);
			const auto duck_us = timing.duckdb_median_us;
			const auto accel_us = timing.accel_median_us;
			const auto speedup = duck_us / accel_us;
			std::cout << "Q5," << entry.label << ',' << threads << ',' << duck_us << ',' << accel_us << ','
			          << speedup << '\n';
			speedups.push_back(speedup);
		}

		if (!speedups.empty()) {
			std::cout << "summary,Q5,geomean," << threads << ",,," << Geomean(speedups) << '\n';
		}
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q5_parallel failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
