#pragma once

#include <string>
#include <vector>

#include "duckdb.hpp"

namespace accelerator_codegen::test_support {

struct InterferenceCalibration {
	double isolated_query_p50_us = 0.0;
	double concurrent_query_p50_us = 0.0;
	double isolated_rebuild_us = 0.0;
	double concurrent_rebuild_us = 0.0;
	int target_reader_count = 3;
	int background_reader_count = 2;
	double interference_delta_per_query_us = 0.0;
	double interference_invalidate_per_query_us = 0.0;
	double interference_rebuild_per_query_us = 0.0;
	double interference_rebuild_scan_byte_us = 0.0;
	double interference_query_slowdown_factor = 0.0;
};

std::vector<int> MultiConnectionTestQueries();

InterferenceCalibration CalibrateInterference(duckdb::DuckDB &database, int target_query,
                                              const std::vector<int> &all_queries);

void RunMultiConnectionStress(duckdb::DuckDB &database, int target_query, const std::vector<int> &all_queries,
                              int duration_sec = 5);

} // namespace accelerator_codegen::test_support
