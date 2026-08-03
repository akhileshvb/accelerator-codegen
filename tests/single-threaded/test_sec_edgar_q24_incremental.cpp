#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q24_write_policy.hpp"
#include "test_support/sec_edgar_q24_aux_helpers.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void EnableHybridMaintenance() {
	setenv("ACCEL_AUX_MAINTENANCE_SELECTOR", "hybrid", 1);
}

void CreateSchema(duckdb::Connection &connection) {
	Require(connection.Query("CREATE TABLE pre(adsh VARCHAR, tag VARCHAR, version VARCHAR)") != nullptr, "create pre");
	Require(connection.Query("CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, uom VARCHAR, value DOUBLE, ddate INTEGER)") !=
	            nullptr,
	        "create num");
	Require(connection.Query("INSERT INTO pre VALUES ('0000000001-01-000001', 'Revenue', '1')") != nullptr, "insert pre");
	std::ostringstream num_inserts;
	num_inserts << "INSERT INTO num VALUES ";
	for (int i = 0; i < 12; i++) {
		if (i > 0) {
			num_inserts << ", ";
		}
		num_inserts << "('0000000002-01-00000" << (i + 1) << "', 'Revenue', '1', 'USD', " << (10.0 + i) << ", 20230601)";
	}
	Require(connection.Query(num_inserts.str()) != nullptr, "insert num");
}

} // namespace

int main() {
	try {
		EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		CreateSchema(connection);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		Require(connection.Query("SELECT * FROM accel_sec_edgar_q24_aux_build()") != nullptr, "build");
		accelerator_codegen::test_support::RequireEqualSecEdgarQ24Results(connection, "baseline");

		connection.BeginTransaction();
		Require(connection.Query("INSERT INTO num VALUES ('0000000002-01-000099', 'Revenue', '1', 'USD', 5.0, 20230601)") !=
		            nullptr,
		        "value_append insert");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualSecEdgarQ24Results(connection, "after_value_append");
		accelerator_codegen::test_support::RequireNoIncrementalFailures("after_value_append");

		std::cout << "test_sec_edgar_q24_incremental passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_sec_edgar_q24_incremental failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
