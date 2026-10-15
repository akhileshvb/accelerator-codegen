//! DuckDB (matched threads) vs accel_tpch_q1_parallel across manifest seeds.

#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/benchmark_paired_timing.hpp"
#include "tpch/q1/tests/q1_manifest.hpp"

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
#error "benchmark_q1_parallel requires ACCEL_ENABLE_PARALLEL_KERNELS"
#endif

namespace {
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
	return accelerator_codegen::test_support::TpchDbPath();
}

int GetEnvInt(const char *name, int fallback) {
	const char *env = std::getenv(name);
	if (!env || !*env) {
		return fallback;
	}
	return std::stoi(env);
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

std::string ReferenceQ1Sql(int day_offset) {
	return "SELECT l_returnflag, l_linestatus, SUM(l_quantity) AS sum_qty, "
	       "SUM(l_extendedprice) AS sum_base_price, "
	       "SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
	       "SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
	       "AVG(l_quantity) AS avg_qty, AVG(l_extendedprice) AS avg_price, AVG(l_discount) AS avg_disc, "
	       "COUNT(*) AS count_order FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - " +
	       std::to_string(day_offset) + " GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";
}

std::string AcceleratorQ1ParallelSql(int day_offset) {
	return "SELECT * FROM accel_tpch_q1_parallel(" + std::to_string(day_offset) + ")";
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

		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ1Manifest.size());
		     i++) {
			const auto &entry = accelerator_codegen::test_support::kQ1Manifest[i];
			const auto duck_sql = ReferenceQ1Sql(entry.day_offset);
			const auto accel_sql = AcceleratorQ1ParallelSql(entry.day_offset);
			const auto timing =
			    accelerator_codegen::tpch_bench::BenchmarkPair(connection, duck_sql, accel_sql, warmup, measured);
			const auto duck_us = timing.duckdb_median_us;
			const auto accel_us = timing.accel_median_us;
			const auto speedup = duck_us / accel_us;
			std::cout << "Q1," << entry.label << ',' << threads << ',' << duck_us << ',' << accel_us << ','
			          << speedup << '\n';
			speedups.push_back(speedup);
		}

		if (!speedups.empty()) {
			std::cout << "summary,Q1,geomean," << threads << ",,," << Geomean(speedups) << '\n';
		}
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q1_parallel failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
