// Template for SEC-EDGAR base accelerator tests.
// Copy to accelerators/sec_edgar/q{{query_number}}/tests/test_sec_edgar_q{{query_number}}.cpp and customize ({{query_number_2d}}: the query number in two digits).

#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/result_compare.hpp"

#include <iostream>
#include <string>

#include "duckdb.hpp"

namespace {

constexpr const char *kReferenceSqlPath = "workloads/sec_edgar/instances/seed_1/{{query_number_2d}}.sql";

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
