#pragma once

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string GetBenchmarkDbPath(const char *env_var, const char *default_path) {
    const char *env_path = std::getenv(env_var);
    if (env_path && *env_path) {
        return env_path;
    }
    return default_path;
}

// ctest SKIP_RETURN_CODE for tests whose database is not provisioned.
inline constexpr int kSkipReturnCode = 77;

// Read-only config: a smoke test never writes its database, and read-only
// opens of a shared file do not lock out concurrent runs.
inline void MakeReadOnly(duckdb::DBConfig &config) {
    config.options.access_mode = duckdb::AccessMode::READ_ONLY;
}

// Exits with kSkipReturnCode and a clear message when `db_path` is missing or
// lacks one of `tables` (e.g. an empty placeholder file), instead of failing
// later with an opaque "Table ... does not exist".
inline void SkipUnlessDbHasTables(const std::string &db_path, std::initializer_list<const char *> tables,
                                  const char *env_hint) {
    if (!std::filesystem::exists(db_path)) {
        std::cerr << "SKIP: database " << db_path << " does not exist (set " << env_hint << ")\n";
        std::exit(kSkipReturnCode);
    }
    std::string missing;
    {
        duckdb::DBConfig config;
        MakeReadOnly(config);
        duckdb::DuckDB database(db_path, &config);
        duckdb::Connection connection(database);
        for (const char *table : tables) {
            auto result = connection.Query(std::string("SELECT 1 FROM duckdb_tables() WHERE table_name = '") + table + "'");
            if (result->HasError() || result->RowCount() == 0) {
                missing += missing.empty() ? table : std::string(", ") + table;
            }
        }
    }
    if (!missing.empty()) {
        std::cerr << "SKIP: database " << db_path << " lacks table(s) " << missing
                  << " (not loaded? set " << env_hint << " to a populated database)\n";
        std::exit(kSkipReturnCode);
    }
}

inline void Require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::string ReadFile(const std::string &path) {
    std::ifstream input(path);
    Require(input.good(), "failed to open file: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

inline std::vector<std::string> MaterializeOrderedRows(duckdb::MaterializedQueryResult &result) {
    std::vector<std::string> rows;
    rows.reserve(result.RowCount());
    for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
        std::string encoded;
        for (duckdb::idx_t col = 0; col < result.ColumnCount(); col++) {
            if (col != 0) {
                encoded += " | ";
            }
            encoded += result.GetValue(col, row).ToString();
        }
        rows.push_back(std::move(encoded));
    }
    return rows;
}

inline bool ValuesEqualCell(const duckdb::Value &expected, const duckdb::Value &actual) {
    if (expected.type().id() == duckdb::LogicalTypeId::DOUBLE && actual.type().id() == duckdb::LogicalTypeId::DOUBLE) {
        const double expected_value = expected.GetValue<double>();
        const double actual_value = actual.GetValue<double>();
        const double scale = std::max({1.0, std::abs(expected_value), std::abs(actual_value)});
        return std::abs(expected_value - actual_value) <= scale * 1e-9;
    }
    return expected.ToString() == actual.ToString();
}

inline void RequireResultsEqualOrdered(duckdb::Connection &connection,
                                       const std::string &label,
                                       const std::string &reference_sql,
                                       const std::string &accelerator_sql) {
    auto expected = connection.Query(reference_sql);
    auto actual = connection.Query(accelerator_sql);
    Require(expected != nullptr, label + ": reference query returned null");
    Require(!expected->HasError(), label + ": reference failed: " + expected->GetError());
    Require(actual != nullptr, label + ": accelerator query returned null");
    Require(!actual->HasError(), label + ": accelerator failed: " + actual->GetError());

    auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
    auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
    Require(expected_m.ColumnCount() == actual_m.ColumnCount(), label + ": column count differs");
    Require(expected_m.RowCount() == actual_m.RowCount(), label + ": row count differs");

    for (duckdb::idx_t row = 0; row < expected_m.RowCount(); row++) {
        for (duckdb::idx_t col = 0; col < expected_m.ColumnCount(); col++) {
            const auto &expected_value = expected_m.GetValue(col, row);
            const auto &actual_value = actual_m.GetValue(col, row);
            Require(ValuesEqualCell(expected_value, actual_value),
                    label + ": row " + std::to_string(row) + " column " + std::to_string(col) + " differs");
        }
    }
}

} // namespace accelerator_codegen::test_support
