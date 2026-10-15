#include "eval/tests/test_support/db_paths.hpp"
#include "calibrate_aux_cost_model_common.hpp"

#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/interference_calibration.hpp"
#include "eval/tests/test_support/cross_accel_helpers.hpp"

#include <fstream>
#include <iostream>
#include <string>

#include "duckdb.hpp"
#include "catalog/query_catalog.hpp"

namespace accelerator_codegen::calibration {
namespace {

void WriteCostModel(const std::string &out_path,
                    const test_support::InterferenceCalibration &interference, double aux_hit_us,
                    double fallback_us, double partition_rebuild_us, double full_rebuild_us) {
	std::ofstream out(out_path);
	out << "{\n"
	    << "  \"version\": 1,\n"
	    << "  \"aux_hit_us\": " << aux_hit_us << ",\n"
	    << "  \"fallback_us\": " << fallback_us << ",\n"
	    << "  \"partition_rebuild_us\": " << partition_rebuild_us << ",\n"
	    << "  \"full_rebuild_us\": " << full_rebuild_us << ",\n"
	    << "  \"first_query_blocking_us\": 0.0,\n"
	    << "  \"apply_delta_overhead_us\": 100.0,\n"
	    << "  \"apply_delta_base_us\": 0.0,\n"
	    << "  \"apply_delta_per_row_us\": 0.5,\n"
	    << "  \"apply_delta_max_rows\": 1048576,\n"
	    << "  \"apply_delta_max_part_fanout\": 512,\n"
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
}

} // namespace

int CalibrateAndWriteCostModel(int query_num, int argc, char **argv) {
	const std::string db_path = argc > 1 ? std::string(argv[1]) : accelerator_codegen::test_support::TpchDbPath();
	const std::string out_path =
	    argc > 2 ? argv[2] : accelerator_codegen::aux_state::core::AuxCostModelDefaultPath(
	                       accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch", query_num).cost_model);
	duckdb::DuckDB database(db_path);
	auto all_queries = test_support::MultiConnectionTestQueries();
	if (std::find(all_queries.begin(), all_queries.end(), query_num) == all_queries.end()) {
		all_queries.push_back(query_num);
	}
	const auto interference = test_support::CalibrateInterference(database, query_num, all_queries);
	WriteCostModel(out_path, interference, 500.0, 50000.0, 60000.0, 250000.0);
	std::cout << "calibrate_q" << query_num << "_aux_cost_model wrote " << out_path << std::endl;
	return EXIT_SUCCESS;
}

} // namespace accelerator_codegen::calibration
