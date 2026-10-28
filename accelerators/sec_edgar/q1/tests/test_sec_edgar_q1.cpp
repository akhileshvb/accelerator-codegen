#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/result_compare.hpp"

#include <iostream>
#include <string>

#include "duckdb.hpp"

namespace {

// The base kernel's fixed query: Q1 over every non-NULL stmt (the aux accelerator takes stmt as $1).
constexpr const char *kReferenceSql =
    "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, AVG(line) AS avg_line_num "
    "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC";

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

		accelerator_codegen::test_support::RequireResultsEqualOrdered(
		    connection, "sec_edgar_q1_manifest", kReferenceSql, "SELECT * FROM accel_sec_edgar_q1()");
		std::cout << "sec_edgar_q1 smoke test passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "sec_edgar_q1 smoke test failed: " << ex.what() << '\n';
		return 1;
	}
}
