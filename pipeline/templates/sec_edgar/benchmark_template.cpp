// Template for SEC-EDGAR base accelerator benchmarks.
// Copy to accelerators/sec_edgar/q{{query_number}}/bench/benchmark_q{{query_number}}.cpp and customize.

#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/result_compare.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "duckdb.hpp"

namespace {
using Clock = std::chrono::steady_clock;

constexpr const char *kReferenceSqlPath = "workloads/sec_edgar/q{{query_number}}.sql";

int GetEnvInt(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return std::stoi(value);
}

double RunTimed(duckdb::Connection &connection, const std::string &sql) {
    const auto start = Clock::now();
    auto result = connection.Query(sql);
    if (result == nullptr || result->HasError()) {
        throw std::runtime_error(result ? result->GetError() : "null result");
    }
    result->Fetch();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

int main() {
    try {
        const auto db_path = accelerator_codegen::test_support::SecEdgarDbPath();
        duckdb::DuckDB database(db_path);
        duckdb::Connection connection(database);
        accelerator_codegen::RegisterAcceleratorFunctions(connection);

        const int warmup = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
        const int iters = GetEnvInt("ACCEL_BENCH_ITERS", 10);
        const auto reference_sql = accelerator_codegen::test_support::ReadFile(kReferenceSqlPath);
        const std::string accelerator_sql = "SELECT * FROM {{function_name}}()";

        for (int i = 0; i < warmup; i++) {
            RunTimed(connection, reference_sql);
            RunTimed(connection, accelerator_sql);
        }

        double ref_ms = 0.0;
        double accel_ms = 0.0;
        for (int i = 0; i < iters; i++) {
            ref_ms += RunTimed(connection, reference_sql);
            accel_ms += RunTimed(connection, accelerator_sql);
        }
        ref_ms /= iters;
        accel_ms /= iters;

        std::cout << "label,reference_ms,accelerator_ms,speedup\n";
        std::cout << "q{{query_number}}," << ref_ms << "," << accel_ms << "," << (ref_ms / accel_ms) << "\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "benchmark_sec_edgar_q{{query_number}} failed: " << ex.what() << '\n';
        return 1;
    }
}
