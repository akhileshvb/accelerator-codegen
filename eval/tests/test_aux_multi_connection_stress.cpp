#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/cross_accel_helpers.hpp"
#include "eval/tests/test_support/interference_calibration.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableCrossAccelHybridMaintenance();
		const auto targets = accelerator_codegen::test_support::MultiConnectionTestQueries();
		Require(!targets.empty(), "no target queries configured");
		const int target_query = targets.front();
		auto all_queries = accelerator_codegen::test_support::MultiConnectionTestQueries();
		if (std::find(all_queries.begin(), all_queries.end(), target_query) == all_queries.end()) {
			all_queries.push_back(target_query);
		}

		duckdb::DuckDB database(accelerator_codegen::test_support::CrossAccelMutableDatabasePath());
		const char *duration_env = std::getenv("ACCEL_MULTI_CONN_STRESS_SEC");
		const int duration_sec = duration_env ? std::stoi(duration_env) : 5;
		accelerator_codegen::test_support::RunMultiConnectionStress(database, target_query, all_queries, duration_sec);

		std::cout << "test_aux_multi_connection_stress passed for Q" << target_query << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_aux_multi_connection_stress failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
