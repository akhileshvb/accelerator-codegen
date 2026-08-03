#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "test_support/result_compare.hpp"
#include "test_support/sec_edgar_q1_aux_helpers.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

constexpr const char *kDefaultDb = "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";

std::string GetSecEdgarDbPath() {
	const char *env_path = std::getenv("ACCEL_SEC_EDGAR_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return kDefaultDb;
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void BuildAuxState(duckdb::Connection &connection) {
	auto result = connection.Query("SELECT * FROM accel_sec_edgar_q1_aux_build()");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

} // namespace

int main() {
	try {
		const char *db_env = std::getenv("ACCEL_SEC_EDGAR_DB");
		const bool run_sf3 = std::getenv("ACCEL_SEC_EDGAR_SecEdgarQ1_AUX_SKIP_SF3") == nullptr && db_env != nullptr;
		if (run_sf3) {
			duckdb::DuckDB file_database(GetSecEdgarDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "sf3_before_build");
			BuildAuxState(connection);
			accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "sf3_after_build");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			Require(connection.Query("CREATE TABLE pre(stmt VARCHAR, rfile VARCHAR, line INTEGER, adsh VARCHAR)") !=
			            nullptr,
			        "create pre");
			Require(connection.Query("INSERT INTO pre VALUES ('BS','H',1,'0000000001-01-000001')") != nullptr, "insert1");
			Require(connection.Query("INSERT INTO pre VALUES ('BS','H',2,'0000000001-01-000002')") != nullptr, "insert2");
			Require(connection.Query("INSERT INTO pre VALUES ('IS','H',3,'0000000001-01-000001')") != nullptr, "insert3");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);
			accelerator_codegen::test_support::RequireEqualSecEdgarQ1Results(connection, "synthetic");
		}

		std::cout << "test_sec_edgar_q1_aux passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_sec_edgar_q1_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
