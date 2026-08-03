#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q4_write_policy.hpp"
#include "test_support/sec_edgar_q4_aux_helpers.hpp"

#include <cstdlib>
#include <iostream>
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
	Require(connection.Query("CREATE TABLE sub(adsh VARCHAR, cik INTEGER, sic INTEGER)") != nullptr, "create sub");
	Require(connection.Query("CREATE TABLE tag(tag VARCHAR, version VARCHAR, abstract INTEGER, tlabel VARCHAR)") != nullptr,
	        "create tag");
	Require(connection.Query("CREATE TABLE pre(adsh VARCHAR, tag VARCHAR, version VARCHAR, stmt VARCHAR)") != nullptr,
	        "create pre");
	Require(connection.Query("CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, uom VARCHAR, value DOUBLE)") !=
	            nullptr,
	        "create num");
	Require(connection.Query("INSERT INTO tag VALUES ('Revenue', '1', 0, 'Revenues')") != nullptr, "insert tag");
	Require(connection.Query("INSERT INTO sub VALUES "
	                         "('0000000001-01-000001', 1001, 4100), "
	                         "('0000000002-01-000001', 1002, 4100)") != nullptr,
	        "insert sub");
	Require(connection.Query("INSERT INTO pre VALUES "
	                         "('0000000001-01-000001', 'Revenue', '1', 'EQ'), "
	                         "('0000000002-01-000001', 'Revenue', '1', 'EQ')") != nullptr,
	        "insert pre");
	Require(connection.Query("INSERT INTO num VALUES "
	                         "('0000000001-01-000001', 'Revenue', '1', 'USD', 100.0), "
	                         "('0000000002-01-000001', 'Revenue', '1', 'USD', 50.0)") != nullptr,
	        "insert num");
}

} // namespace

int main() {
	try {
		EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		CreateSchema(connection);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		Require(connection.Query("SELECT * FROM accel_sec_edgar_q4_aux_build()") != nullptr, "build");
		accelerator_codegen::test_support::RequireEqualSecEdgarQ4Results(connection, "baseline");

		connection.BeginTransaction();
		Require(connection.Query("INSERT INTO num VALUES ('0000000001-01-000001', 'Revenue', '1', 'USD', 5.0)") != nullptr,
		        "value_append insert");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualSecEdgarQ4Results(connection, "after_value_append");
		accelerator_codegen::test_support::RequireNoIncrementalFailures("after_value_append");

		std::cout << "test_sec_edgar_q4_incremental passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_sec_edgar_q4_incremental failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
