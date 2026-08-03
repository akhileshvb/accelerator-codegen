#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "test_support/sec_edgar_q1_aux_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

namespace {

void CreatePreSchema(duckdb::Connection &connection) {
	connection.Query("CREATE TABLE pre(stmt VARCHAR, rfile VARCHAR, line INTEGER, adsh VARCHAR)");
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		CreatePreSchema(connection);
		connection.Query("INSERT INTO pre VALUES ('BS','H',1,'0000000001-01-000001')");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_sec_edgar_q1_aux_build()");
		accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "partition_state");
		std::cout << "test_sec_edgar_q1_aux_partition_state passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_sec_edgar_q1_aux_partition_state failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
