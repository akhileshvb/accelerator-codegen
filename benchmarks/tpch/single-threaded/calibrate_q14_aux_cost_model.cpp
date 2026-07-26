#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "test_support/q14_aux_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

#include "duckdb.hpp"

namespace {
using Clock = std::chrono::steady_clock;

double MedianUs(std::vector<double> samples) {
	if (samples.empty()) {
		return 0.0;
	}
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

double TimeQueryUs(duckdb::Connection &connection, const std::string &sql) {
	const auto start = Clock::now();
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		return 0.0;
	}
	return static_cast<double>(
	    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

} // namespace

int main(int argc, char **argv) {
	const char *db_path = argc > 1 ? argv[1] : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
	const char *out_path = argc > 2 ? argv[2] : "config/q14_aux_cost_model.json";
	try {
		duckdb::DuckDB database(db_path);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_q14_aux_build()");

		const std::string month = "1995-06-01";
		std::vector<double> aux_hits;
		std::vector<double> fallbacks;
		for (int i = 0; i < 50; i++) {
			aux_hits.push_back(TimeQueryUs(connection, accelerator_codegen::test_support::AcceleratorQ14AuxSql(month)));
		}
		connection.Query("SELECT * FROM accel_q14_aux_rebuild_partition(DATE '" + month + "')");
		connection.Query("UPDATE lineitem SET l_comment = l_comment WHERE l_orderkey % 100000 = 0");
		for (int i = 0; i < 50; i++) {
			fallbacks.push_back(TimeQueryUs(connection, accelerator_codegen::test_support::AcceleratorQ14AuxSql(month)));
		}

		const auto partition_rebuild_start = Clock::now();
		connection.Query("SELECT * FROM accel_q14_aux_rebuild_partition(DATE '" + month + "')");
		const auto partition_rebuild_us = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                                          Clock::now() - partition_rebuild_start)
		                                                          .count());

		connection.Query("UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0000001 "
		                 "WHERE l_shipdate >= DATE '" +
		                 month + "' AND l_shipdate < DATE '" + month + "' + INTERVAL '1' month");
		const auto first_query_start = Clock::now();
		TimeQueryUs(connection, accelerator_codegen::test_support::AcceleratorQ14AuxSql(month));
		const auto first_query_total_us = static_cast<double>(
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - first_query_start).count());
		const auto aux_hit_median = MedianUs(aux_hits);
		const auto first_query_blocking_us =
		    std::max(0.0, first_query_total_us - partition_rebuild_us - aux_hit_median);

		const auto full_rebuild_start = Clock::now();
		connection.Query("SELECT * FROM accel_q14_aux_build()");
		const auto full_rebuild_us = static_cast<double>(
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - full_rebuild_start).count());

		std::ofstream out(out_path);
		out << "{\n"
		    << "  \"version\": 1,\n"
		    << "  \"aux_hit_us\": " << MedianUs(aux_hits) << ",\n"
		    << "  \"fallback_us\": " << MedianUs(fallbacks) << ",\n"
		    << "  \"partition_rebuild_us\": " << partition_rebuild_us << ",\n"
		    << "  \"full_rebuild_us\": " << full_rebuild_us << ",\n"
		    << "  \"first_query_blocking_us\": " << first_query_blocking_us << ",\n"
		    << "  \"apply_delta_overhead_us\": 100.0,\n"
		    << "  \"invalidation_overhead_us\": 10.0,\n"
		    << "  \"decision_margin_us\": 1000.0,\n"
		    << "  \"min_samples\": 8,\n"
		    << "  \"incremental_failure_prob\": 0.01\n"
		    << "}\n";
		std::cout << "calibrate_q14_aux_cost_model wrote " << out_path << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "calibrate_q14_aux_cost_model failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
