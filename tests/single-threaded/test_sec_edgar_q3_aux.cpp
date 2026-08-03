#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q3_aux.hpp"
#include "test_support/sec_edgar_q3_aux_helpers.hpp"

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
	auto result = connection.Query("SELECT * FROM accel_sec_edgar_q3_aux_build()");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

} // namespace

int main() {
	try {
		const char *db_env = std::getenv("ACCEL_SEC_EDGAR_DB");
		const bool run_sf3 = std::getenv("ACCEL_SEC_EDGAR_SecEdgarQ3_AUX_SKIP_SF3") == nullptr && db_env != nullptr;
		if (run_sf3) {
			duckdb::DuckDB file_database(GetSecEdgarDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			accelerator_codegen::test_support::RequireEqualSecEdgarQ3Results(connection, "sf3_before_build");
			BuildAuxState(connection);
			accelerator_codegen::test_support::RequireEqualSecEdgarQ3Results(connection, "sf3_after_build");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			Require(connection.Query("CREATE TABLE sub(adsh VARCHAR, name VARCHAR, cik INTEGER, fy INTEGER)") != nullptr,
			        "create sub");
			Require(connection.Query("CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, uom VARCHAR, value DOUBLE)") !=
			            nullptr,
			        "create num");
			Require(connection.Query("INSERT INTO sub VALUES "
			                         "('0000000001-01-000001', 'Acme', 1001, 2022), "
			                         "('0000000002-01-000001', 'Beta', 1002, 2022)") != nullptr,
			        "insert sub");
			Require(connection.Query(
			            "INSERT INTO num VALUES "
			            "('0000000001-01-000001', 'Revenue', '1', 'USD', 100.0), "
			            "('0000000002-01-000001', 'Revenue', '1', 'USD', 10.0)") != nullptr,
			        "insert num");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);
			accelerator_codegen::test_support::RequireEqualSecEdgarQ3Results(connection, "synthetic");
		}

		std::cout << "test_sec_edgar_q3_aux passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_sec_edgar_q3_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
