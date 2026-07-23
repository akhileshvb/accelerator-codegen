//! DuckDB (matched threads) vs accel_tpch_q{10,18,20,21}_parallel for Wave 5 queries.

#include "accelerator_codegen/register.hpp"
#include "test_support/q10_manifest.hpp"
#include "test_support/q18_manifest.hpp"
#include "test_support/q20_manifest.hpp"
#include "test_support/q21_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
	return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf10.duckdb";
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return std::stoi(value);
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

struct TimingStats {
	double median_us = 0.0;
};

TimingStats SummarizeMedian(std::vector<double> samples_us) {
	std::sort(samples_us.begin(), samples_us.end());
	const auto middle = samples_us.size() / 2;
	TimingStats stats;
	stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
	                                               : samples_us[middle];
	return stats;
}

double BenchmarkMedian(duckdb::Connection &connection, const std::string &sql, int warmup, int measured) {
	for (int i = 0; i < warmup; i++) {
		ExecuteAndConsume(connection, sql);
	}
	std::vector<double> samples;
	samples.reserve(measured);
	for (int i = 0; i < measured; i++) {
		const auto t0 = Clock::now();
		ExecuteAndConsume(connection, sql);
		const auto t1 = Clock::now();
		samples.push_back(
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
	}
	return SummarizeMedian(std::move(samples)).median_us;
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
#ifndef ACCEL_ENABLE_PARALLEL_TPCH
		std::cerr << "requires ACCEL_ENABLE_PARALLEL_TPCH build\n";
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
		const int seed_limit = GetEnvInt("ACCEL_BENCH_SEED_LIMIT", 5);

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		Require(!connection.Query("PRAGMA threads=" + std::to_string(threads))->HasError(), "set threads");

		std::cout << "query,seed,threads,duckdb_median_us,accel_median_us,speedup\n";
		std::vector<double> q10s, q18s, q20s, q21s;

		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ10Manifest.size());
		     i++) {
			const auto &e = accelerator_codegen::test_support::kQ10Manifest[i];
			const auto duck_sql = ReadFile(e.sql_path);
			const auto accel_sql = std::string("SELECT * FROM accel_tpch_q10_parallel(DATE '") + e.start_date + "')";
			const auto duck_us = BenchmarkMedian(connection, duck_sql, warmup, measured);
			const auto accel_us = BenchmarkMedian(connection, accel_sql, warmup, measured);
			const auto speedup = duck_us / accel_us;
			std::cout << "Q10," << e.label << ',' << threads << ',' << duck_us << ',' << accel_us << ',' << speedup
			          << '\n';
			q10s.push_back(speedup);
		}
		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ18Manifest.size());
		     i++) {
			const auto &e = accelerator_codegen::test_support::kQ18Manifest[i];
			const auto duck_sql = ReadFile(e.sql_path);
			const auto accel_sql =
			    std::string("SELECT * FROM accel_tpch_q18_parallel(") + std::to_string(e.quantity_threshold) + ")";
			const auto duck_us = BenchmarkMedian(connection, duck_sql, warmup, measured);
			const auto accel_us = BenchmarkMedian(connection, accel_sql, warmup, measured);
			const auto speedup = duck_us / accel_us;
			std::cout << "Q18," << e.label << ',' << threads << ',' << duck_us << ',' << accel_us << ',' << speedup
			          << '\n';
			q18s.push_back(speedup);
		}
		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ20Manifest.size());
		     i++) {
			const auto &e = accelerator_codegen::test_support::kQ20Manifest[i];
			const auto duck_sql = ReadFile(e.sql_path);
			const auto accel_sql = std::string("SELECT * FROM accel_tpch_q20_parallel(DATE '") + e.start_date + "', '" +
			                       EscapeLiteral(e.part_name_pattern) + "', '" + EscapeLiteral(e.nation_name) + "')";
			const auto duck_us = BenchmarkMedian(connection, duck_sql, warmup, measured);
			const auto accel_us = BenchmarkMedian(connection, accel_sql, warmup, measured);
			const auto speedup = duck_us / accel_us;
			std::cout << "Q20," << e.label << ',' << threads << ',' << duck_us << ',' << accel_us << ',' << speedup
			          << '\n';
			q20s.push_back(speedup);
		}
		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ21Manifest.size());
		     i++) {
			const auto &e = accelerator_codegen::test_support::kQ21Manifest[i];
			const auto duck_sql = ReadFile(e.sql_path);
			const auto accel_sql =
			    std::string("SELECT * FROM accel_tpch_q21_parallel('") + EscapeLiteral(e.nation_name) + "')";
			const auto duck_us = BenchmarkMedian(connection, duck_sql, warmup, measured);
			const auto accel_us = BenchmarkMedian(connection, accel_sql, warmup, measured);
			const auto speedup = duck_us / accel_us;
			std::cout << "Q21," << e.label << ',' << threads << ',' << duck_us << ',' << accel_us << ',' << speedup
			          << '\n';
			q21s.push_back(speedup);
		}

		if (!q10s.empty()) {
			std::cout << "summary,Q10,geomean," << threads << ",,," << Geomean(q10s) << '\n';
		}
		if (!q18s.empty()) {
			std::cout << "summary,Q18,geomean," << threads << ",,," << Geomean(q18s) << '\n';
		}
		if (!q20s.empty()) {
			std::cout << "summary,Q20,geomean," << threads << ",,," << Geomean(q20s) << '\n';
		}
		if (!q21s.empty()) {
			std::cout << "summary,Q21,geomean," << threads << ",,," << Geomean(q21s) << '\n';
		}
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_wave5_parallel failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
