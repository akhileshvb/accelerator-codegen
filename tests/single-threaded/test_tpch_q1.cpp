#include "accelerator_codegen/register.hpp"
#include "test_support/q1_manifest.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

std::string GetTpchDbPath() {
    const char *env_path = std::getenv("ACCEL_TPCH_DB");
    if (env_path && *env_path) {
        return env_path;
    }
    return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
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

std::unique_ptr<duckdb::QueryResult> RunQuery(duckdb::Connection &connection, const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result != nullptr, "query returned null result");
    Require(!result->HasError(), result->GetError());
    return result;
}

std::string RenderResult(duckdb::MaterializedQueryResult &result) {
    std::ostringstream out;
    for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
        out << "[";
        for (duckdb::idx_t col = 0; col < result.ColumnCount(); col++) {
            if (col > 0) {
                out << ", ";
            }
            out << result.GetValue(col, row).ToString();
        }
        out << "]";
        if (row + 1 < result.RowCount()) {
            out << "\n";
        }
    }
    return out.str();
}

std::string ReferenceQ1Sql(int day_offset) {
    return "SELECT l_returnflag, l_linestatus, SUM(l_quantity) AS sum_qty, "
           "SUM(l_extendedprice) AS sum_base_price, "
           "SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
           "SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
           "AVG(l_quantity) AS avg_qty, AVG(l_extendedprice) AS avg_price, AVG(l_discount) AS avg_disc, "
           "COUNT(*) AS count_order FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - " +
           std::to_string(day_offset) + " GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";
}

std::string AcceleratorQ1Sql(int day_offset) {
    return "SELECT * FROM accel_tpch_q1(" + std::to_string(day_offset) + ")";
}

void RequireDoubleClose(double expected, double actual, const std::string &label) {
    const double diff = std::fabs(expected - actual);
    const double scale = std::max(1.0, std::fabs(expected));
    Require(diff <= 1e-12 * scale, label + ": double values differ");
}

void RequireEqualQ1Results(duckdb::Connection &connection,
                           const std::string &label,
                           const std::string &reference_sql,
                           const std::string &accelerator_sql) {
    auto expected = RunQuery(connection, reference_sql);
    auto actual = RunQuery(connection, accelerator_sql);

    auto &expected_materialized = expected->Cast<duckdb::MaterializedQueryResult>();
    auto &actual_materialized = actual->Cast<duckdb::MaterializedQueryResult>();

    Require(expected->names == actual->names, label + ": column names differ");
    Require(expected->types == actual->types, label + ": column types differ");
    Require(expected_materialized.RowCount() == actual_materialized.RowCount(),
            label + ": row counts differ\nexpected:\n" + RenderResult(expected_materialized) + "\nactual:\n" +
                RenderResult(actual_materialized));
    Require(expected_materialized.ColumnCount() == actual_materialized.ColumnCount(), label + ": column counts differ");
    const auto fail = [&](const std::string &message, duckdb::idx_t row, duckdb::idx_t col) {
        throw std::runtime_error(message + " at row " + std::to_string(row) + ", col " + std::to_string(col) +
                                 "\nexpected:\n" + RenderResult(expected_materialized) + "\nactual:\n" +
                                 RenderResult(actual_materialized));
    };

    for (duckdb::idx_t row = 0; row < expected_materialized.RowCount(); row++) {
        for (duckdb::idx_t col = 0; col < expected_materialized.ColumnCount(); col++) {
            const auto expected_value = expected_materialized.GetValue(col, row);
            const auto actual_value = actual_materialized.GetValue(col, row);
            if (expected_value.type() != actual_value.type()) {
                fail(label + ": value types differ", row, col);
            }
            if (expected_value.IsNull() != actual_value.IsNull()) {
                fail(label + ": nullability differs", row, col);
            }
            if (expected_value.IsNull()) {
                continue;
            }
            switch (expected_value.type().id()) {
            case duckdb::LogicalTypeId::VARCHAR:
                if (expected_value.GetValue<std::string>() != actual_value.GetValue<std::string>()) {
                    fail(label + ": varchar payload differs", row, col);
                }
                break;
            case duckdb::LogicalTypeId::DECIMAL:
                if (expected_value.GetValueUnsafe<duckdb::hugeint_t>() !=
                    actual_value.GetValueUnsafe<duckdb::hugeint_t>()) {
                    fail(label + ": decimal payload differs", row, col);
                }
                break;
            case duckdb::LogicalTypeId::DOUBLE:
                try {
                    RequireDoubleClose(expected_value.GetValue<double>(), actual_value.GetValue<double>(),
                                       label + ": double payload differs");
                } catch (const std::exception &) {
                    fail(label + ": double payload differs", row, col);
                }
                break;
            case duckdb::LogicalTypeId::BIGINT:
                if (expected_value.GetValue<int64_t>() != actual_value.GetValue<int64_t>()) {
                    fail(label + ": bigint payload differs", row, col);
                }
                break;
            default:
                throw std::runtime_error(label + ": unsupported Q1 comparison type " + expected_value.type().ToString());
            }
        }
    }
}

void RequireQueryErrorContains(duckdb::Connection &connection,
                               const std::string &label,
                               const std::string &sql,
                               const std::string &needle) {
    auto result = connection.Query(sql);
    Require(result != nullptr, label + ": query returned null result");
    Require(result->HasError(), label + ": expected query to fail");
    Require(result->GetError().find(needle) != std::string::npos,
            label + ": expected error to contain '" + needle + "' but saw: " + result->GetError());
}

void RequireExpectedQ1Schema(duckdb::Connection &connection) {
    auto result = RunQuery(connection, AcceleratorQ1Sql(103));
    const duckdb::vector<duckdb::string> expected_names = {"l_returnflag", "l_linestatus", "sum_qty", "sum_base_price",
                                                           "sum_disc_price", "sum_charge", "avg_qty", "avg_price",
                                                           "avg_disc", "count_order"};
    const duckdb::vector<duckdb::LogicalType> expected_types = {
        duckdb::LogicalType::VARCHAR,
        duckdb::LogicalType::VARCHAR,
        duckdb::LogicalType::DECIMAL(38, 2),
        duckdb::LogicalType::DECIMAL(38, 2),
        duckdb::LogicalType::DECIMAL(38, 4),
        duckdb::LogicalType::DECIMAL(38, 6),
        duckdb::LogicalType::DOUBLE,
        duckdb::LogicalType::DOUBLE,
        duckdb::LogicalType::DOUBLE,
        duckdb::LogicalType::BIGINT,
    };
    Require(result->names == expected_names, "q1 schema names differ");
    Require(result->types == expected_types, "q1 schema types differ");
}

void CreateLineitemSchema(duckdb::Connection &connection, const std::string &tax_type = "DECIMAL(15,2)") {
    auto result = connection.Query(
        "CREATE TABLE lineitem("
        "l_orderkey BIGINT NOT NULL, "
        "l_partkey BIGINT NOT NULL, "
        "l_suppkey BIGINT NOT NULL, "
        "l_linenumber BIGINT NOT NULL, "
        "l_quantity DECIMAL(15,2) NOT NULL, "
        "l_extendedprice DECIMAL(15,2) NOT NULL, "
        "l_discount DECIMAL(15,2) NOT NULL, "
        "l_tax " + tax_type + " NOT NULL, "
        "l_returnflag VARCHAR NOT NULL, "
        "l_linestatus VARCHAR NOT NULL, "
        "l_shipdate DATE NOT NULL, "
        "l_commitdate DATE NOT NULL, "
        "l_receiptdate DATE NOT NULL, "
        "l_shipinstruct VARCHAR NOT NULL, "
        "l_shipmode VARCHAR NOT NULL, "
        "l_comment VARCHAR NOT NULL)");
    Require(result != nullptr, "failed to create q1 lineitem schema");
    Require(!result->HasError(), result->GetError());
}

void SetupQ1MultiGroupLineitem(duckdb::Connection &connection) {
    CreateLineitemSchema(connection);
    auto result = connection.Query(R"SQL(
        INSERT INTO lineitem VALUES
            (1, 10, 100, 1, 10.00, 100.00, 0.05, 0.02, 'A', 'F', DATE '1998-08-01', DATE '1998-08-02', DATE '1998-08-03', 'DELIVER', 'MAIL', 'row1'),
            (2, 20, 200, 1, 20.00, 200.00, 0.10, 0.03, 'A', 'F', DATE '1998-08-10', DATE '1998-08-11', DATE '1998-08-12', 'DELIVER', 'MAIL', 'row2'),
            (3, 30, 300, 1, 30.00, 300.00, 0.00, 0.05, 'B', 'O', DATE '1998-08-15', DATE '1998-08-16', DATE '1998-08-17', 'DELIVER', 'MAIL', 'row3'),
            (4, 40, 400, 1, 40.00, 400.00, 0.20, 0.01, 'C', 'F', DATE '1998-08-20', DATE '1998-08-21', DATE '1998-08-22', 'DELIVER', 'MAIL', 'row4'),
            (5, 50, 500, 1, 50.00, 500.00, 0.15, 0.04, 'D', 'O', DATE '1998-09-10', DATE '1998-09-11', DATE '1998-09-12', 'DELIVER', 'MAIL', 'row5');
    )SQL");
    Require(result != nullptr, "failed to set up q1 multi-group lineitem");
    Require(!result->HasError(), result->GetError());
}

void SetupQ1EmptyLineitem(duckdb::Connection &connection) {
    CreateLineitemSchema(connection);
    auto result = connection.Query(R"SQL(
        INSERT INTO lineitem VALUES
            (1, 10, 100, 1, 10.00, 100.00, 0.05, 0.02, 'A', 'F', DATE '1998-09-01', DATE '1998-09-02', DATE '1998-09-03', 'DELIVER', 'MAIL', 'late')
    )SQL");
    Require(result != nullptr, "failed to set up q1 empty lineitem");
    Require(!result->HasError(), result->GetError());
}

void SetupQ1OneGroupLineitem(duckdb::Connection &connection) {
    CreateLineitemSchema(connection);
    auto result = connection.Query(R"SQL(
        INSERT INTO lineitem VALUES
            (1, 10, 100, 1, 10.00, 100.00, 0.05, 0.02, 'R', 'F', DATE '1998-08-01', DATE '1998-08-02', DATE '1998-08-03', 'DELIVER', 'MAIL', 'row1'),
            (2, 20, 200, 1, 20.00, 200.00, 0.10, 0.03, 'R', 'F', DATE '1998-08-02', DATE '1998-08-03', DATE '1998-08-04', 'DELIVER', 'MAIL', 'row2')
    )SQL");
    Require(result != nullptr, "failed to set up q1 one-group lineitem");
    Require(!result->HasError(), result->GetError());
}

void SetupQ1SchemaMismatchLineitem(duckdb::Connection &connection) {
    CreateLineitemSchema(connection, "DOUBLE");
}

} // namespace

int main() {
    try {
        {
            duckdb::DuckDB file_database(GetTpchDbPath().c_str());
            duckdb::Connection connection(file_database);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);

            RequireExpectedQ1Schema(connection);
            for (const auto &entry : accelerator_codegen::test_support::kQ1Manifest) {
                RequireEqualQ1Results(connection, entry.label, ReadFile(entry.sql_path), AcceleratorQ1Sql(entry.day_offset));
            }
        }

        {
            duckdb::DuckDB memory_database(nullptr);
            duckdb::Connection connection(memory_database);
            SetupQ1EmptyLineitem(connection);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);
            RequireEqualQ1Results(connection, "empty_qualifying_input", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));
        }

        {
            duckdb::DuckDB memory_database(nullptr);
            duckdb::Connection connection(memory_database);
            SetupQ1OneGroupLineitem(connection);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);
            RequireEqualQ1Results(connection, "one_group", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));
        }

        {
            duckdb::DuckDB memory_database(nullptr);
            duckdb::Connection connection(memory_database);
            SetupQ1MultiGroupLineitem(connection);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);

            RequireEqualQ1Results(connection, "multi_group_and_multi_chunk_output", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));

            connection.BeginTransaction();
            auto insert_result = connection.Query(R"SQL(
                INSERT INTO lineitem VALUES
                    (6, 60, 600, 1, 60.00, 600.00, 0.08, 0.02, 'E', 'O', DATE '1998-08-05', DATE '1998-08-06', DATE '1998-08-07', 'DELIVER', 'MAIL', 'inserted')
            )SQL");
            Require(insert_result != nullptr && !insert_result->HasError(), "q1 insert failed");
            RequireEqualQ1Results(connection, "after_insert", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));

            auto update_aggregate =
                connection.Query("UPDATE lineitem SET l_quantity = 11.00, l_extendedprice = 110.00 WHERE l_orderkey = 1");
            Require(update_aggregate != nullptr && !update_aggregate->HasError(), "q1 aggregate update failed");
            RequireEqualQ1Results(connection, "after_aggregate_update", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));

            auto update_group =
                connection.Query("UPDATE lineitem SET l_returnflag = 'A', l_linestatus = 'O' WHERE l_orderkey = 3");
            Require(update_group != nullptr && !update_group->HasError(), "q1 group update failed");
            RequireEqualQ1Results(connection, "after_group_update", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));

            auto update_into_range =
                connection.Query("UPDATE lineitem SET l_shipdate = DATE '1998-08-03' WHERE l_orderkey = 5");
            Require(update_into_range != nullptr && !update_into_range->HasError(), "q1 into-range update failed");
            RequireEqualQ1Results(connection, "after_into_range_update", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));

            auto update_out_of_range =
                connection.Query("UPDATE lineitem SET l_shipdate = DATE '1998-09-15' WHERE l_orderkey = 2");
            Require(update_out_of_range != nullptr && !update_out_of_range->HasError(), "q1 out-of-range update failed");
            RequireEqualQ1Results(connection, "after_out_of_range_update", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));

            auto delete_result = connection.Query("DELETE FROM lineitem WHERE l_orderkey = 4");
            Require(delete_result != nullptr && !delete_result->HasError(), "q1 delete failed");
            RequireEqualQ1Results(connection, "after_delete", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));

            connection.Rollback();
            RequireEqualQ1Results(connection, "after_rollback", ReferenceQ1Sql(100), AcceleratorQ1Sql(100));
        }

        {
            duckdb::DuckDB mismatch_database(nullptr);
            duckdb::Connection connection(mismatch_database);
            SetupQ1SchemaMismatchLineitem(connection);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);
            RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ1Sql(100), "Unsupported schema");
        }

        std::cout << "tpch_q1_smoke_test passed" << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "tpch_q1_smoke_test failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
