#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "test_support/interference_calibration.hpp"
#include "test_support/cross_accel_helpers.hpp"
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
	const int target_query = 14;
	try {
		duckdb::DuckDB database(db_path);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=1");
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

		const auto all_queries = accelerator_codegen::test_support::CrossAccelTestQueries();
		const auto interference =
		    accelerator_codegen::test_support::CalibrateInterference(database, target_query, all_queries);

		std::ofstream out(out_path);
		out << "{\n"
		    << "  \"version\": 1,\n"
		    << "  \"aux_hit_us\": " << aux_hit_median << ",\n"
		    << "  \"fallback_us\": " << MedianUs(fallbacks) << ",\n"
		    << "  \"partition_rebuild_us\": " << partition_rebuild_us << ",\n"
		    << "  \"full_rebuild_us\": " << full_rebuild_us << ",\n"
		    << "  \"first_query_blocking_us\": " << first_query_blocking_us << ",\n"
		    << "  \"apply_delta_overhead_us\": 100.0,\n"
		    << "  \"apply_delta_base_us\": 0.0,\n"
		    << "  \"apply_delta_per_row_us\": 0.5,\n"
		    << "  \"apply_delta_part_lineitem_scan_us\": 27000.0,\n"
		    << "  \"apply_delta_max_rows\": 1024,\n"
		    << "  \"apply_delta_max_part_fanout\": 16,\n"
		    << "  \"invalidation_overhead_us\": 10.0,\n"
		    << "  \"decision_margin_us\": 1000.0,\n"
		    << "  \"min_samples\": 8,\n"
		    << "  \"incremental_failure_prob\": 0.01,\n"
		    << "  \"interference_rebuild_per_query_us\": " << interference.interference_rebuild_per_query_us << ",\n"
		    << "  \"interference_rebuild_scan_byte_us\": " << interference.interference_rebuild_scan_byte_us << ",\n"
		    << "  \"interference_delta_per_query_us\": " << interference.interference_delta_per_query_us << ",\n"
		    << "  \"interference_invalidate_per_query_us\": " << interference.interference_invalidate_per_query_us
		    << ",\n"
		    << "  \"interference_query_slowdown_factor\": " << interference.interference_query_slowdown_factor << "\n"
		    << "}\n";
		std::cout << "calibrate_q14_aux_cost_model wrote " << out_path << '\n';
		std::cout << "interference_delta_per_query_us=" << interference.interference_delta_per_query_us << '\n';
		std::cout << "interference_rebuild_per_query_us=" << interference.interference_rebuild_per_query_us << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "calibrate_q14_aux_cost_model failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
