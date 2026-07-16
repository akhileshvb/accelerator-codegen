#include "accelerator_codegen/register.hpp"
#include "test_support/q6_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
    const char *env_path = std::getenv("ACCEL_TPCH_DB");
    if (env_path && *env_path) {
        return env_path;
    }
    return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

int GetEnvInt(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return std::stoi(value);
}

std::string GetEnvString(const char *name) {
    const char *value = std::getenv(name);
    if (!value) {
        return "";
    }
    return value;
}

void Require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string ReadFile(const std::string &path) {
    std::ifstream input(path);
    Require(input.good(), "failed to open file: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> SplitCommaSeparated(const std::string &input) {
    std::vector<std::string> result;
    std::stringstream stream(input);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

bool MatchesFilter(const std::string &label, const std::vector<std::string> &filters) {
    if (filters.empty()) {
        return true;
    }
    return std::find(filters.begin(), filters.end(), label) != filters.end();
}

std::string AcceleratorQ6Sql(const accelerator_codegen::test_support::Q6ManifestEntry &entry) {
    return std::string("SELECT revenue FROM accel_tpch_q6(DATE '") + entry.start_date + "', " + entry.discount + ", " +
           entry.quantity + ")";
}

void ExecuteAndValidate(duckdb::Connection &connection, const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result != nullptr, "query returned null result");
    Require(!result->HasError(), result->GetError());
    auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
    Require(materialized.RowCount() == 1, "expected one-row Q6 result");
    (void)materialized.GetValue(0, 0);
}

double BenchmarkQuery(duckdb::Connection &connection,
                      const std::string &sql,
                      int warmup_iterations,
                      int measured_iterations) {
    for (int i = 0; i < warmup_iterations; i++) {
        ExecuteAndValidate(connection, sql);
    }

    const auto start = Clock::now();
    for (int i = 0; i < measured_iterations; i++) {
        ExecuteAndValidate(connection, sql);
    }
    const auto end = Clock::now();

    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    return static_cast<double>(total_us) / static_cast<double>(measured_iterations);
}

struct BenchmarkAggregate {
    int rows = 0;
    double total_reference_us = 0.0;
    double total_accelerator_us = 0.0;
};

} // namespace

int main() {
    try {
        const int warmup_iterations = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
        const int measured_iterations = GetEnvInt("ACCEL_BENCH_ITERS", 10);
        const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));

        duckdb::DuckDB database(GetTpchDbPath().c_str());
        duckdb::Connection connection(database);
        accelerator_codegen::RegisterAcceleratorFunctions(connection);
        connection.Query("PRAGMA threads=1");

        BenchmarkAggregate aggregate;
        std::cout << "label,reference_us,accelerator_us,speedup\n";
        for (const auto &entry : accelerator_codegen::test_support::kQ6Manifest) {
            const std::string label = entry.label;
            if (!MatchesFilter(label, filters)) {
                continue;
            }

            const auto reference_sql = ReadFile(entry.sql_path);
            const auto accelerator_sql = AcceleratorQ6Sql(entry);
            const auto reference_us = BenchmarkQuery(connection, reference_sql, warmup_iterations, measured_iterations);
            const auto accelerator_us = BenchmarkQuery(connection, accelerator_sql, warmup_iterations, measured_iterations);
            const auto speedup = reference_us / accelerator_us;

            aggregate.rows += 1;
            aggregate.total_reference_us += reference_us;
            aggregate.total_accelerator_us += accelerator_us;

            std::cout << label << ',' << std::fixed << std::setprecision(2) << reference_us << ',' << accelerator_us << ','
                      << speedup << "\n";
        }

        Require(aggregate.rows > 0, "benchmark filter selected no Q6 instances");
        const auto average_reference_us = aggregate.total_reference_us / static_cast<double>(aggregate.rows);
        const auto average_accelerator_us = aggregate.total_accelerator_us / static_cast<double>(aggregate.rows);
        const auto average_speedup = average_reference_us / average_accelerator_us;
        std::cout << "summary_avg," << std::fixed << std::setprecision(2) << average_reference_us << ','
                  << average_accelerator_us << ',' << average_speedup << "\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "benchmark_q6 failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
