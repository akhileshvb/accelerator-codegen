// Template for SEC-EDGAR base accelerator tests.
// Copy to accelerators/sec_edgar/q{{query_number}}/tests/test_sec_edgar_q{{query_number}}.cpp and customize.

#include "backends/duckdb/api/register.hpp"
#include "test_support/result_compare.hpp"

#include <iostream>
#include <string>

#include "duckdb.hpp"

namespace {

constexpr const char *kDefaultDb = "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";
constexpr const char *kReferenceSqlPath = "eval/bench/sec_edgar/queries/q{{query_number}}.sql";

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
            "sec_edgar_q{{query_number}}_manifest",
            reference_sql,
            "SELECT * FROM {{function_name}}()");
        std::cout << "sec_edgar_q{{query_number}} smoke test passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "sec_edgar_q{{query_number}} smoke test failed: " << ex.what() << '\n';
        return 1;
    }
}
