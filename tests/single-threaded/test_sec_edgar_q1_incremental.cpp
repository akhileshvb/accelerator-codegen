#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q1_write_policy.hpp"
#include "test_support/sec_edgar_q1_aux_helpers.hpp"

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

void CreatePreSchema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE pre(stmt VARCHAR, rfile VARCHAR, line INTEGER, adsh VARCHAR, plabel VARCHAR, negating BOOLEAN, "
	    "inpth BOOLEAN, drlbl BOOLEAN, durp BOOLEAN, datp BOOLEAN, uom VARCHAR, footlen INTEGER, footnote VARCHAR)");
	Require(result != nullptr && !result->HasError(), "failed to create pre schema: " + result->GetError());
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		CreatePreSchema(connection);
		Require(connection.Query("INSERT INTO pre VALUES ('BS','H',1,'0000000001-01-000001',NULL,false,false,false,false,false,NULL,0,NULL)") !=
		            nullptr,
		        "insert1");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		Require(connection.Query("SELECT * FROM accel_sec_edgar_q1_aux_build()") != nullptr, "build");

		accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "baseline");

		connection.BeginTransaction();
		Require(connection.Query("INSERT INTO pre VALUES ('BS','H',2,'0000000001-01-000002',NULL,false,false,false,false,false,NULL,0,NULL)") !=
		            nullptr,
		        "insert2");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "after_insert");
		accelerator_codegen::test_support::RequireNoIncrementalFailures("after_insert");

		connection.BeginTransaction();
		Require(connection.Query("UPDATE pre SET line = 5 WHERE adsh = '0000000001-01-000002'") != nullptr, "update line");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "after_line_update");

		std::cout << "test_sec_edgar_q1_incremental passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_sec_edgar_q1_incremental failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
