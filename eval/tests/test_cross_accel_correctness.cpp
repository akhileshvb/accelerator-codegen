#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/cross_accel_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

int main() {
	try {
		accelerator_codegen::test_support::EnableCrossAccelHybridMaintenance();
		duckdb::DuckDB database(accelerator_codegen::test_support::CrossAccelMutableDatabasePath());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=1");

		const auto queries = accelerator_codegen::test_support::CrossAccelTestQueries();
		for (int query_num : queries) {
			accelerator_codegen::test_support::BuildCrossAccelAux(connection, query_num);
		}

		accelerator_codegen::test_support::RequireCrossAccelCorrectness(connection, queries);

		for (int query_num : queries) {
			accelerator_codegen::test_support::ApplyInvalidatingWriterSql(connection, query_num);
			accelerator_codegen::test_support::RequireCrossAccelQueryCorrectness(
			    connection, query_num, "after_write_Q" + std::to_string(query_num));
		}

		std::cout << "test_cross_accel_correctness passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_cross_accel_correctness failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
