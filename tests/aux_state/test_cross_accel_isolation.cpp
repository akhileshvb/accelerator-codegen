#include "accelerator_codegen/register.hpp"
#include "test_support/cross_accel_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

namespace {

void RequireAllCorrect(duckdb::Connection &connection, const std::vector<int> &queries, const std::string &label) {
	for (int query_num : queries) {
		accelerator_codegen::test_support::RequireCrossAccelQueryCorrectness(
		    connection, query_num, label + "_Q" + std::to_string(query_num));
	}
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database("/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=1");

		const auto queries = accelerator_codegen::test_support::CrossAccelTestQueries();
		for (int query_num : queries) {
			accelerator_codegen::test_support::BuildCrossAccelAux(connection, query_num);
		}
		RequireAllCorrect(connection, queries, "baseline");

		for (int writer_query : queries) {
			accelerator_codegen::test_support::ApplyInvalidatingWriterSql(connection, writer_query);
			for (int reader_query : queries) {
				accelerator_codegen::test_support::RequireCrossAccelQueryCorrectness(
				    connection, reader_query,
				    "after_Q" + std::to_string(writer_query) + "_write_check_Q" + std::to_string(reader_query));
			}
		}

		std::cout << "test_cross_accel_isolation passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_cross_accel_isolation failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
