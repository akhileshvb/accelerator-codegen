#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_maintenance.hpp"
#include "test_support/sec_edgar_q1_aux_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		connection.Query(
		    "CREATE TABLE pre(stmt VARCHAR, rfile VARCHAR, line INTEGER, adsh VARCHAR, plabel VARCHAR, negating BOOLEAN, "
		    "inpth BOOLEAN, drlbl BOOLEAN, durp BOOLEAN, datp BOOLEAN, uom VARCHAR, footlen INTEGER, footnote VARCHAR)");
		connection.Query("INSERT INTO pre VALUES ('BS','H',1,'0000000001-01-000001',NULL,false,false,false,false,false,NULL,0,NULL)");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_sec_edgar_q1_aux_build()");
		accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "baseline");

		connection.BeginTransaction();
		connection.Query("INSERT INTO pre VALUES ('BS','H',2,'0000000001-01-000002',NULL,false,false,false,false,false,NULL,0,NULL)");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "hybrid_filing_append");

		const auto &log = accelerator_codegen::GetSecEdgarQ1MaintenanceDecisionLog();
		if (log.empty()) {
			throw std::runtime_error("expected at least one maintenance decision");
		}
		std::cout << "test_sec_edgar_q1_hybrid_policy passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_sec_edgar_q1_hybrid_policy failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
