#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/result_compare.hpp"

#include <iostream>
#include <string>

#include "duckdb.hpp"

namespace {

constexpr const char *kReferenceSqlPath = "workloads/sec_edgar/instances/seed_1/24.sql";

} // namespace

int main() {
	try {
		namespace ts = accelerator_codegen::test_support;
		const auto db_path = ts::SecEdgarSmokeDbPath();
		ts::SkipUnlessDbHasTables(db_path, {"sub", "num", "tag", "pre"}, "ACCEL_SEC_EDGAR_DB");
		duckdb::DBConfig db_config;
		ts::MakeReadOnly(db_config);
		duckdb::DuckDB database(db_path, &db_config);
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
