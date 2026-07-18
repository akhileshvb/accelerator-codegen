#include "accelerator_codegen/register.hpp"
#include "test_support/q6_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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

duckdb::Value QuerySingleValue(duckdb::Connection &connection, const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result != nullptr, "query returned null result");
    Require(!result->HasError(), result->GetError());
    auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
    Require(materialized.RowCount() == 1, "expected a single-row result");
    return materialized.GetValue(0, 0);
}

void RequireEqualRevenue(duckdb::Connection &connection,
                         const std::string &label,
                         const std::string &reference_sql,
                         const std::string &accelerator_sql) {
    auto expected = QuerySingleValue(connection, reference_sql);
    auto actual = QuerySingleValue(connection, accelerator_sql);

    Require(expected.type() == actual.type(), label + ": result types differ");
    Require(expected.IsNull() == actual.IsNull(), label + ": nullability differs");
    if (!expected.IsNull()) {
        Require(expected.GetValueUnsafe<duckdb::hugeint_t>() == actual.GetValueUnsafe<duckdb::hugeint_t>(),
                label + ": decimal payload differs");
    }
}


void RequireEqualSingleQuery(duckdb::Connection &connection,
                             const std::string &label,
                             const std::string &lhs_sql,
                             const std::string &rhs_sql) {
    auto lhs = QuerySingleValue(connection, lhs_sql);
    auto rhs = QuerySingleValue(connection, rhs_sql);
    Require(lhs.type() == rhs.type(), label + ": result types differ");
    Require(lhs.IsNull() == rhs.IsNull(), label + ": nullability differs");
    if (!lhs.IsNull()) {
        Require(lhs.GetValueUnsafe<duckdb::hugeint_t>() == rhs.GetValueUnsafe<duckdb::hugeint_t>(),
                label + ": decimal payload differs");
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

void SetupBoundaryTestLineitem(duckdb::Connection &connection) {
    auto result = connection.Query(R"SQL(
        CREATE TABLE lineitem(
            l_orderkey BIGINT NOT NULL,
            l_partkey BIGINT NOT NULL,
            l_suppkey BIGINT NOT NULL,
            l_linenumber BIGINT NOT NULL,
            l_quantity DECIMAL(15,2) NOT NULL,
            l_extendedprice DECIMAL(15,2) NOT NULL,
            l_discount DECIMAL(15,2) NOT NULL,
            l_tax DECIMAL(15,2) NOT NULL,
            l_returnflag VARCHAR NOT NULL,
            l_linestatus VARCHAR NOT NULL,
            l_shipdate DATE NOT NULL,
            l_commitdate DATE NOT NULL,
            l_receiptdate DATE NOT NULL,
            l_shipinstruct VARCHAR NOT NULL,
            l_shipmode VARCHAR NOT NULL,
            l_comment VARCHAR NOT NULL
        );
        INSERT INTO lineitem VALUES
            (1, 10, 100, 1, 10.00, 100.00, 0.05, 0.01, 'N', 'O', DATE '1992-02-29', DATE '1992-03-01', DATE '1992-03-02', 'DELIVER', 'MAIL', 'lower_discount'),
            (2, 20, 200, 1, 23.99, 200.00, 0.07, 0.02, 'N', 'O', DATE '1993-02-27', DATE '1993-02-28', DATE '1993-03-01', 'DELIVER', 'MAIL', 'upper_discount'),
            (3, 30, 300, 1, 23.00, 300.00, 0.04, 0.03, 'N', 'O', DATE '1992-06-01', DATE '1992-06-02', DATE '1992-06-03', 'DELIVER', 'MAIL', 'below_discount'),
            (4, 40, 400, 1, 24.00, 400.00, 0.06, 0.04, 'N', 'O', DATE '1992-06-01', DATE '1992-06-02', DATE '1992-06-03', 'DELIVER', 'MAIL', 'quantity_boundary'),
            (5, 50, 500, 1, 20.00, 500.00, 0.06, 0.05, 'N', 'O', DATE '1993-02-28', DATE '1993-03-01', DATE '1993-03-02', 'DELIVER', 'MAIL', 'exclusive_end_date');
    )SQL");
    Require(result != nullptr, "failed to set up boundary test lineitem");
    Require(!result->HasError(), result->GetError());
}

void SetupTransactionTestLineitem(duckdb::Connection &connection) {
    auto result = connection.Query(R"SQL(
        CREATE TABLE lineitem(
            l_orderkey BIGINT NOT NULL,
            l_partkey BIGINT NOT NULL,
            l_suppkey BIGINT NOT NULL,
            l_linenumber BIGINT NOT NULL,
            l_quantity DECIMAL(15,2) NOT NULL,
            l_extendedprice DECIMAL(15,2) NOT NULL,
            l_discount DECIMAL(15,2) NOT NULL,
            l_tax DECIMAL(15,2) NOT NULL,
            l_returnflag VARCHAR NOT NULL,
            l_linestatus VARCHAR NOT NULL,
            l_shipdate DATE NOT NULL,
            l_commitdate DATE NOT NULL,
            l_receiptdate DATE NOT NULL,
            l_shipinstruct VARCHAR NOT NULL,
            l_shipmode VARCHAR NOT NULL,
            l_comment VARCHAR NOT NULL
        );
        INSERT INTO lineitem VALUES
            (1, 10, 100, 1, 10.00, 100.00, 0.05, 0.01, 'N', 'O', DATE '1992-02-29', DATE '1992-03-01', DATE '1992-03-02', 'DELIVER', 'MAIL', 'base_lower_discount'),
            (2, 20, 200, 1, 23.99, 200.00, 0.07, 0.02, 'N', 'O', DATE '1993-02-27', DATE '1993-02-28', DATE '1993-03-01', 'DELIVER', 'MAIL', 'base_upper_discount'),
            (3, 30, 300, 1, 23.00, 300.00, 0.04, 0.03, 'N', 'O', DATE '1992-06-01', DATE '1992-06-02', DATE '1992-06-03', 'DELIVER', 'MAIL', 'below_discount'),
            (4, 40, 400, 1, 24.00, 400.00, 0.06, 0.04, 'N', 'O', DATE '1992-06-01', DATE '1992-06-02', DATE '1992-06-03', 'DELIVER', 'MAIL', 'quantity_boundary'),
            (5, 50, 500, 1, 20.00, 500.00, 0.06, 0.05, 'N', 'O', DATE '1993-02-28', DATE '1993-03-01', DATE '1993-03-02', 'DELIVER', 'MAIL', 'exclusive_end_date');
    )SQL");
    Require(result != nullptr, "failed to set up transaction test lineitem");
    Require(!result->HasError(), result->GetError());
}

void SetupSchemaMismatchLineitem(duckdb::Connection &connection) {
    auto result = connection.Query(R"SQL(
        CREATE TABLE lineitem(
            l_orderkey BIGINT NOT NULL,
            l_partkey BIGINT NOT NULL,
            l_suppkey BIGINT NOT NULL,
            l_linenumber BIGINT NOT NULL,
            l_quantity DECIMAL(15,2) NOT NULL,
            l_extendedprice DECIMAL(15,2) NOT NULL,
            l_discount DOUBLE NOT NULL,
            l_tax DECIMAL(15,2) NOT NULL,
            l_returnflag VARCHAR NOT NULL,
            l_linestatus VARCHAR NOT NULL,
            l_shipdate DATE NOT NULL,
            l_commitdate DATE NOT NULL,
            l_receiptdate DATE NOT NULL,
            l_shipinstruct VARCHAR NOT NULL,
            l_shipmode VARCHAR NOT NULL,
            l_comment VARCHAR NOT NULL
        );
    )SQL");
    Require(result != nullptr, "failed to set up schema mismatch lineitem");
    Require(!result->HasError(), result->GetError());
}

std::string ReferenceQ6Sql(const std::string &start_date, const std::string &discount, const std::string &quantity) {
    return "SELECT SUM(l_extendedprice * l_discount) AS revenue FROM lineitem WHERE l_shipdate >= DATE '" + start_date +
           "' AND l_shipdate < DATE '" + start_date + "' + INTERVAL '1 year' AND l_discount BETWEEN " + discount +
           " - 0.01 AND " + discount + " + 0.01 AND l_quantity < " + quantity;
}

std::string AcceleratorQ6Sql(const std::string &start_date, const std::string &discount, const std::string &quantity) {
    return "SELECT revenue FROM accel_tpch_q6(DATE '" + start_date + "', " + discount + ", " + quantity + ")";
}

} // namespace

int main() {
    try {
        {
            duckdb::DuckDB file_database(GetTpchDbPath().c_str());
            duckdb::Connection connection(file_database);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);

            for (const auto &entry : accelerator_codegen::test_support::kQ6Manifest) {
                RequireEqualRevenue(connection, entry.label, ReadFile(entry.sql_path),
                                    AcceleratorQ6Sql(entry.start_date, entry.discount, entry.quantity));
            }

            RequireEqualSingleQuery(
                connection,
                "literal_overload_matches_decimal_cast",
                "SELECT revenue FROM accel_tpch_q6(DATE '1994-01-01', 0.06, 24)",
                "SELECT revenue FROM accel_tpch_q6(DATE '1994-01-01', CAST(0.06 AS DECIMAL(15,2)), CAST(24 AS DECIMAL(15,2)))");
        }

        {
            duckdb::DuckDB memory_database(nullptr);
            duckdb::Connection connection(memory_database);
            SetupBoundaryTestLineitem(connection);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);

            RequireEqualRevenue(connection, "leap_year_and_boundaries",
                                ReferenceQ6Sql("1992-02-29", "0.06", "24"),
                                AcceleratorQ6Sql("1992-02-29", "0.06", "24"));
            RequireEqualRevenue(connection, "empty_result",
                                ReferenceQ6Sql("1994-01-01", "0.02", "1"),
                                AcceleratorQ6Sql("1994-01-01", "0.02", "1"));
        }

        {
            duckdb::DuckDB memory_database(nullptr);
            duckdb::Connection connection(memory_database);
            SetupTransactionTestLineitem(connection);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);

            RequireEqualRevenue(connection, "transaction_baseline",
                                ReferenceQ6Sql("1992-02-29", "0.06", "24"),
                                AcceleratorQ6Sql("1992-02-29", "0.06", "24"));

            connection.BeginTransaction();

            auto update_aggregate =
                connection.Query("UPDATE lineitem SET l_extendedprice = 150.00 WHERE l_orderkey = 1");
            Require(update_aggregate != nullptr && !update_aggregate->HasError(), "q6 aggregate update failed");
            RequireEqualRevenue(connection, "after_aggregate_update",
                                ReferenceQ6Sql("1992-02-29", "0.06", "24"),
                                AcceleratorQ6Sql("1992-02-29", "0.06", "24"));

            auto update_into_range =
                connection.Query("UPDATE lineitem SET l_shipdate = DATE '1993-02-27' WHERE l_orderkey = 5");
            Require(update_into_range != nullptr && !update_into_range->HasError(), "q6 into-range update failed");
            RequireEqualRevenue(connection, "after_into_range_update",
                                ReferenceQ6Sql("1992-02-29", "0.06", "24"),
                                AcceleratorQ6Sql("1992-02-29", "0.06", "24"));

            auto update_out_of_range =
                connection.Query("UPDATE lineitem SET l_quantity = 24.00 WHERE l_orderkey = 2");
            Require(update_out_of_range != nullptr && !update_out_of_range->HasError(), "q6 out-of-range update failed");
            RequireEqualRevenue(connection, "after_out_of_range_update",
                                ReferenceQ6Sql("1992-02-29", "0.06", "24"),
                                AcceleratorQ6Sql("1992-02-29", "0.06", "24"));

            auto delete_result = connection.Query("DELETE FROM lineitem WHERE l_orderkey = 1");
            Require(delete_result != nullptr && !delete_result->HasError(), "q6 delete failed");
            RequireEqualRevenue(connection, "after_delete",
                                ReferenceQ6Sql("1992-02-29", "0.06", "24"),
                                AcceleratorQ6Sql("1992-02-29", "0.06", "24"));

            connection.Rollback();
            RequireEqualRevenue(connection, "after_rollback",
                                ReferenceQ6Sql("1992-02-29", "0.06", "24"),
                                AcceleratorQ6Sql("1992-02-29", "0.06", "24"));
        }

        {
            duckdb::DuckDB mismatch_database(nullptr);
            duckdb::Connection connection(mismatch_database);
            SetupSchemaMismatchLineitem(connection);
            accelerator_codegen::RegisterAcceleratorFunctions(connection);
            RequireQueryErrorContains(connection, "schema_mismatch",
                                      AcceleratorQ6Sql("1994-01-01", "0.06", "24"),
                                      "Unsupported schema");
        }

        std::cout << "tpch_q6_smoke_test passed" << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "tpch_q6_smoke_test failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
