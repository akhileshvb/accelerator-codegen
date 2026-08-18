#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "test_support/result_compare.hpp"

#include <iostream>
#include <string>

#include "duckdb.hpp"

namespace {

constexpr const char *kDefaultDb = "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";
constexpr const char *kReferenceSqlPath = "benchmarks/sec_edgar/queries/q24.sql";

} // namespace

int main() {
	try {
		const auto db_path = accelerator_codegen::test_support::GetBenchmarkDbPath("ACCEL_SEC_EDGAR_DB", kDefaultDb);
		duckdb::DuckDB database(db_path);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const auto reference_sql = accelerator_codegen::test_support::ReadFile(kReferenceSqlPath);
		accelerator_codegen::test_support::RequireResultsEqualOrdered(
		    connection,
		    "sec_edgar_q24_manifest",
		    reference_sql,
		    "SELECT * FROM accel_sec_edgar_q24()");
		std::cout << "sec_edgar_q24 smoke test passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "sec_edgar_q24 smoke test failed: " << ex.what() << '\n';
		return 1;
	}
}
