#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "eval/harness/float_order_ties.hpp"
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

// A positional mismatch may be a reorder inside a floating-point ORDER BY tie
// (eval/harness/float_order_ties.hpp): empty when it is.
inline std::string TieAwareOrderedMismatch(duckdb::MaterializedQueryResult &expected,
                                           duckdb::MaterializedQueryResult &actual,
                                           const std::string &reference_sql) {
    namespace ties = accelerator_codegen::float_order_ties;
    std::vector<bool> is_float;
    for (const auto &type : expected.types) {
        is_float.push_back(type.id() == duckdb::LogicalTypeId::DOUBLE || type.id() == duckdb::LogicalTypeId::FLOAT);
    }
    const auto key = ties::FindFloatOrderKey(reference_sql, expected.names, is_float);
    if (!key.present) {
        return "no floating-point leading ORDER BY key";
    }
    const auto column = static_cast<duckdb::idx_t>(key.column);
    const auto key_of = [column](duckdb::MaterializedQueryResult &result) {
        return [&result, column](std::size_t row) -> std::optional<double> {
            const auto value = result.GetValue(column, row);
            if (value.IsNull()) return std::nullopt;
            return value.GetValue<double>();
        };
    };
    const auto cells_of = [](duckdb::MaterializedQueryResult &result) {
        return [&result](std::size_t row) {
            std::vector<std::string> cells;
            for (duckdb::idx_t c = 0; c < result.ColumnCount(); c++) {
                cells.push_back(result.GetValue(c, row).ToString());
            }
            return cells;
        };
    };
    const auto cells_equal = [&](std::size_t c, const std::string &e, const std::string &a) {
        if (e == a) return true;
        if (!is_float[c]) return false;
        try {
            return ties::DoublesClose(std::stod(e), std::stod(a), 1e-9);
        } catch (...) {
            return false;
        }
    };
    return ties::TieAwareMismatch(expected.RowCount(), key, 1e-9, is_float, key_of(expected), key_of(actual),
                                  cells_of(expected), cells_of(actual), cells_equal, &std::cerr);
}

// The result columns of the outermost ORDER BY, when every item is a bare
// (possibly qualified) column name with optional ASC/DESC/NULLS FIRST|LAST;
// empty otherwise. Also the outermost literal LIMIT, else -1.
inline std::vector<std::size_t> OrderByColumns(const std::string &sql, const duckdb::vector<std::string> &names,
                                               long long &limit) {
    namespace ties = accelerator_codegen::float_order_ties;
    limit = -1;
    const auto upper = ties::UpperAscii(sql);
    const auto order = ties::TopLevelKeyword(upper, "ORDER");
    if (order.empty()) return {};
    std::size_t end = upper.size();
    for (const auto at : ties::TopLevelKeyword(upper, "LIMIT")) {
        if (at > order.back()) {
            end = std::min(end, at);
            limit = std::strtoll(upper.c_str() + at + 5, nullptr, 10);
        }
    }
    std::istringstream items(upper.substr(order.back() + 5, end - order.back() - 5));
    std::string word;
    if (!(items >> word) || word != "BY") return {};
    std::string rest((std::istreambuf_iterator<char>(items)), std::istreambuf_iterator<char>());
    while (!rest.empty() && (rest.back() == ';' || std::isspace(static_cast<unsigned char>(rest.back())))) {
        rest.pop_back();
    }
    std::vector<std::size_t> columns;
    std::istringstream list(rest);
    std::string item;
    while (std::getline(list, item, ',')) {
        std::istringstream tokens(item);
        std::string name;
        if (!(tokens >> name)) return {};
        if (const auto dot = name.rfind('.'); dot != std::string::npos) name = name.substr(dot + 1);
        for (std::string modifier; tokens >> modifier;) {
            if (modifier != "ASC" && modifier != "DESC" && modifier != "NULLS" && modifier != "FIRST" &&
                modifier != "LAST") {
                return {};  // an expression
            }
        }
        std::size_t column = names.size();
        for (std::size_t c = 0; c < names.size(); ++c) {
            if (ties::UpperAscii(names[c]) == name) column = c;
        }
        if (column == names.size()) return {};
        columns.push_back(column);
    }
    return columns;
}

// A positional mismatch may be a reorder of rows whose ORDER BY keys are all
// exactly equal (SQL leaves their order unspecified): empty when it is. Inside
// such a run the keys must match positionally and the rows as a multiset; a run
// cut by the LIMIT may hold different rows, so only its keys are checked.
inline std::string ExactOrderTieMismatch(duckdb::MaterializedQueryResult &expected,
                                         duckdb::MaterializedQueryResult &actual,
                                         const std::string &reference_sql) {
    long long limit = -1;
    const auto columns = OrderByColumns(reference_sql, expected.names, limit);
    if (columns.empty()) return "ORDER BY is not a list of result columns";
    const auto rows = expected.RowCount();
    const auto key = [&](duckdb::MaterializedQueryResult &result, duckdb::idx_t row) {
        std::string out;
        for (const auto c : columns) out += result.GetValue(c, row).ToString() + '\x1f';
        return out;
    };
    const auto cells = [](duckdb::MaterializedQueryResult &result, duckdb::idx_t row) {
        std::string out;
        for (duckdb::idx_t c = 0; c < result.ColumnCount(); c++) out += result.GetValue(c, row).ToString() + '\x1f';
        return out;
    };
    for (duckdb::idx_t first = 0; first < rows;) {
        const auto anchor = key(expected, first);
        auto end = first + 1;
        while (end < rows && key(expected, end) == anchor) ++end;
        std::vector<std::string> want;
        std::vector<std::string> got;
        for (auto row = first; row < end; ++row) {
            if (key(actual, row) != anchor) return "order keys differ at row " + std::to_string(row);
            want.push_back(cells(expected, row));
            got.push_back(cells(actual, row));
        }
        const bool cut_by_limit = end == rows && limit >= 0 && static_cast<long long>(rows) == limit;
        std::sort(want.begin(), want.end());
        std::sort(got.begin(), got.end());
        if (!cut_by_limit && want != got) {
            return "rows differ in the ORDER BY tie run at rows " + std::to_string(first) + ".." +
                   std::to_string(end - 1);
        }
        first = end;
    }
    return {};
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
            if (ValuesEqualCell(expected_value, actual_value)) {
                continue;
            }
            const std::string message = label + ": row " + std::to_string(row) + " column " + std::to_string(col) +
                                        " differs, expected=" + expected_value.ToString() +
                                        " actual=" + actual_value.ToString();
            auto why = TieAwareOrderedMismatch(expected_m, actual_m, reference_sql);
            if (!why.empty()) {
                const auto exact = ExactOrderTieMismatch(expected_m, actual_m, reference_sql);
                why = exact.empty() ? std::string{} : why + "; " + exact;
            }
            Require(why.empty(), message + " (tie-aware recheck: " + why + ")");
            return;
        }
    }
}

} // namespace accelerator_codegen::test_support
