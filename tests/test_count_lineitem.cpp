#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/register.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
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

int64_t QuerySingleBigint(duckdb::Connection &connection, const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result != nullptr, "query returned null result");
    Require(!result->HasError(), result->GetError());
    auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
    Require(materialized.RowCount() == 1, "expected a single-row result");
    return materialized.GetValue(0, 0).GetValue<int64_t>();
}

std::vector<duckdb::date_t> QueryShipdates(duckdb::Connection &connection) {
    auto result = connection.Query("SELECT l_shipdate FROM lineitem ORDER BY l_orderkey");
    Require(result != nullptr, "shipdate query returned null result");
    Require(!result->HasError(), result->GetError());

    std::vector<duckdb::date_t> dates;
    for (auto &row : *result) {
        dates.push_back(row.GetValue<duckdb::date_t>(0));
    }
    return dates;
}

void RequireCountMatches(duckdb::Connection &connection, const std::string &label) {
    auto sql_count = QuerySingleBigint(connection, "SELECT COUNT(*) FROM lineitem");
    auto accel_count = QuerySingleBigint(connection, "SELECT row_count FROM accel_count_lineitem()");
    Require(sql_count == accel_count, label + ": SQL count and accelerator count differ");
}

void RequireShipdatesMatch(duckdb::Connection &connection, const std::string &label) {
    auto sql_dates = QueryShipdates(connection);
    auto accel_dates = accelerator_codegen::CollectLineitemShipdates(connection);
    Require(sql_dates == accel_dates, label + ": SQL shipdates and accelerator shipdates differ");
}

void SetupTestLineitem(duckdb::Connection &connection) {
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
            (1, 10, 100, 1, 1.00, 100.00, 0.06, 0.01, 'N', 'O', DATE '1994-01-01', DATE '1994-01-02', DATE '1994-01-03', 'DELIVER', 'MAIL', 'row1'),
            (2, 20, 200, 1, 2.00, 200.00, 0.05, 0.02, 'N', 'O', DATE '1994-02-01', DATE '1994-02-02', DATE '1994-02-03', 'DELIVER', 'MAIL', 'row2'),
            (3, 30, 300, 1, 3.00, 300.00, 0.04, 0.03, 'N', 'O', DATE '1994-03-01', DATE '1994-03-02', DATE '1994-03-03', 'DELIVER', 'MAIL', 'row3');
    )SQL");
    Require(result != nullptr, "failed to set up test lineitem");
    Require(!result->HasError(), result->GetError());
}

} // namespace

int main() {
    try {
        {
            duckdb::DuckDB file_database(GetTpchDbPath().c_str());
            duckdb::Connection file_connection(file_database);
            accelerator_codegen::RegisterAcceleratorFunctions(file_connection);
            RequireCountMatches(file_connection, "committed_sf1");
        }

        {
            duckdb::DuckDB memory_database(nullptr);
            duckdb::Connection memory_connection(memory_database);
            SetupTestLineitem(memory_connection);
            accelerator_codegen::RegisterAcceleratorFunctions(memory_connection);

            RequireCountMatches(memory_connection, "initial");
            RequireShipdatesMatch(memory_connection, "initial");

            memory_connection.BeginTransaction();
            auto insert_result = memory_connection.Query(R"SQL(
                INSERT INTO lineitem VALUES
                    (4, 40, 400, 1, 4.00, 400.00, 0.03, 0.04, 'N', 'O', DATE '1994-04-01', DATE '1994-04-02', DATE '1994-04-03', 'DELIVER', 'MAIL', 'row4')
            )SQL");
            Require(insert_result != nullptr && !insert_result->HasError(), "insert failed");
            RequireCountMatches(memory_connection, "after_insert");
            RequireShipdatesMatch(memory_connection, "after_insert");

            auto delete_result = memory_connection.Query("DELETE FROM lineitem WHERE l_orderkey = 2");
            Require(delete_result != nullptr && !delete_result->HasError(), "delete failed");
            RequireCountMatches(memory_connection, "after_delete");
            RequireShipdatesMatch(memory_connection, "after_delete");

            auto update_result =
                memory_connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-01-15' WHERE l_orderkey = 1");
            Require(update_result != nullptr && !update_result->HasError(), "update failed");
            RequireCountMatches(memory_connection, "after_update");
            RequireShipdatesMatch(memory_connection, "after_update");
            memory_connection.Rollback();

            RequireCountMatches(memory_connection, "after_rollback");
            RequireShipdatesMatch(memory_connection, "after_rollback");
        }

        std::cout << "count_lineitem_smoke_test passed" << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "count_lineitem_smoke_test failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
