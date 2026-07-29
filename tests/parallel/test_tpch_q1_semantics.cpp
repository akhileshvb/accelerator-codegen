#include "accelerator_codegen/register.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q3_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::vector<std::string> MaterializeRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
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

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + " accelerator: " + actual->GetError());
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_m.RowCount() == actual_m.RowCount(), label + ": row count differs");
	auto expected_rows = MaterializeRows(expected_m);
	auto actual_rows = MaterializeRows(actual_m);
	Require(expected_rows == actual_rows, label + ": rows differ");
}

void ResetLineitem(duckdb::Connection &connection) {
	connection.Query("DROP TABLE IF EXISTS lineitem");
	Require(!connection
	             .Query("CREATE TABLE lineitem("
	                    "l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), "
	                    "l_tax DECIMAL(15,2), l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE)")
	             ->HasError(),
	        "create lineitem");
}

std::string ReferenceQ1(int day_offset) {
	return "SELECT l_returnflag, l_linestatus, SUM(l_quantity) AS sum_qty, "
	       "SUM(l_extendedprice) AS sum_base_price, "
	       "SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
	       "SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
	       "AVG(l_quantity) AS avg_qty, AVG(l_extendedprice) AS avg_price, AVG(l_discount) AS avg_disc, "
	       "COUNT(*) AS count_order FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - " +
	       std::to_string(day_offset) + " GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";
}

std::string ParallelQ1(int day_offset) {
	return "SELECT * FROM accel_tpch_q3_parallel(" + std::to_string(day_offset) + ")";
}

void RunCase(duckdb::Connection &connection, const std::string &label, int day_offset,
             const std::function<void(duckdb::Connection &)> &setup) {
	ResetLineitem(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ1(day_offset), ParallelQ1(day_offset));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		constexpr int kDayOffset = 90;
		const std::string cutoff = "DATE '1998-12-01' - 90";

		RunCase(connection, "zero_qualifying_rows", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (1.00, 100.00, 0.05, 0.08, 'N', 'O', " + cutoff + " + 1)");
		});

		RunCase(connection, "one_qualifying_row", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (2.00, 200.00, 0.10, 0.04, 'N', 'F', " + cutoff + ")");
		});

		RunCase(connection, "shipdate_on_cutoff", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (1.00, 50.00, 0.00, 0.00, 'N', 'O', " + cutoff + ")");
		});

		RunCase(connection, "shipdate_before_cutoff", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (1.00, 50.00, 0.00, 0.00, 'N', 'O', " + cutoff + " - 1)");
		});

		RunCase(connection, "shipdate_after_cutoff", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (99.00, 999.00, 0.50, 0.50, 'N', 'O', " + cutoff + " + 1)");
		});

		RunCase(connection, "all_returnflag_linestatus_pairs", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (1.00, 10.00, 0.01, 0.02, 'A', 'F', " + cutoff + ")");
			conn.Query("INSERT INTO lineitem VALUES (1.00, 10.00, 0.01, 0.02, 'A', 'O', " + cutoff + ")");
			conn.Query("INSERT INTO lineitem VALUES (1.00, 10.00, 0.01, 0.02, 'N', 'F', " + cutoff + ")");
			conn.Query("INSERT INTO lineitem VALUES (1.00, 10.00, 0.01, 0.02, 'N', 'O', " + cutoff + ")");
			conn.Query("INSERT INTO lineitem VALUES (1.00, 10.00, 0.01, 0.02, 'R', 'F', " + cutoff + ")");
			conn.Query("INSERT INTO lineitem VALUES (1.00, 10.00, 0.01, 0.02, 'R', 'O', " + cutoff + ")");
		});

		RunCase(connection, "multiple_rows_same_group", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (1.00, 100.00, 0.05, 0.08, 'N', 'O', " + cutoff + ")");
			conn.Query("INSERT INTO lineitem VALUES (3.00, 300.00, 0.15, 0.08, 'N', 'O', " + cutoff + ")");
		});

		RunCase(connection, "zero_discount_and_tax", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (5.00, 500.00, 0.00, 0.00, 'R', 'F', " + cutoff + ")");
		});

		RunCase(connection, "max_representative_discount_tax", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (1.00, 1000.00, 0.10, 0.08, 'A', 'O', " + cutoff + ")");
		});

		RunCase(connection, "large_extended_price", kDayOffset, [&](duckdb::Connection &conn) {
			conn.Query("INSERT INTO lineitem VALUES (1.00, 99999999.99, 0.01, 0.01, 'N', 'F', " + cutoff + ")");
		});

		std::cout << "tpch_q3_semantics_test passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q3_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
