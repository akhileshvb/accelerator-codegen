#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/cross_accel_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

int main(int argc, char **argv) {
	try {
		if (argc == 2 && std::string(argv[1]) == "--help") {
			std::cout << "usage: test_duckdb_cross_accel   (database: ACCEL_TPCH_DB)\n";
			return EXIT_SUCCESS;
		}
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

		std::cout << "test_duckdb_cross_accel passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_duckdb_cross_accel failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
