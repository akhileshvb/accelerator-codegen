#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::tpch_bench {

inline int GetEnvInt(const char *name, int fallback) {
	const char *env = std::getenv(name);
	if (!env || !*env) {
		return fallback;
	}
	return std::stoi(env);
}

inline void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

inline std::string ReadFile(const std::string &path) {
	std::ifstream input(path);
	Require(input.good(), "failed to open file: " + path);
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

inline void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr && !result->HasError(), result ? result->GetError() : "null");
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			(void)materialized.GetValue(col, row).ToString();
		}
	}
}

inline double MedianOf(std::vector<double> samples) {
	Require(!samples.empty(), "no timing samples");
	std::sort(samples.begin(), samples.end());
	const auto middle = samples.size() / 2;
	return samples.size() % 2 == 0 ? (samples[middle - 1] + samples[middle]) / 2.0 : samples[middle];
}

struct PairedMedians {
	double duckdb_median_us = 0.0;
	double accel_median_us = 0.0;
};

//! Alternating warmups and measured iterations (same order policy as benchmark_tpch_validated).
inline PairedMedians BenchmarkPair(duckdb::Connection &connection, const std::string &duckdb_sql,
                                   const std::string &accel_sql, int warmup_iterations, int measured_iterations) {
	using Clock = std::chrono::steady_clock;
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
	PairedMedians timing;
	timing.duckdb_median_us = MedianOf(std::move(duck_samples));
	timing.accel_median_us = MedianOf(std::move(accel_samples));
	return timing;
}

} // namespace accelerator_codegen::tpch_bench
