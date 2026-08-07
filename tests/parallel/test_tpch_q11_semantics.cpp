#include "accelerator_codegen/register.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q31_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string Escape(const std::string &value) {
	std::string out;
	for (char c : value) {
		if (c == '\'') {
			out += "''";
		} else {
			out.push_back(c);
		}
	}
	return out;
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
	std::sort(expected_rows.begin(), expected_rows.end());
	std::sort(actual_rows.begin(), actual_rows.end());
	Require(expected_rows == actual_rows, label + ": rows differ");
}

void ResetTables(duckdb::Connection &connection) {
	connection.Query("DROP TABLE IF EXISTS partsupp");
	connection.Query("DROP TABLE IF EXISTS supplier");
	connection.Query("DROP TABLE IF EXISTS nation");
	Require(!connection.Query(
	                   "CREATE TABLE nation(n_nationkey INTEGER, n_name VARCHAR);"
	                   "CREATE TABLE supplier(s_suppkey BIGINT, s_nationkey INTEGER);"
	                   "CREATE TABLE partsupp(ps_partkey BIGINT, ps_suppkey BIGINT, ps_availqty BIGINT, "
	                   "ps_supplycost DECIMAL(15,2));")
	             ->HasError(),
	        "create schema");
}

void SeedBase(duckdb::Connection &connection) {
	connection.Query("INSERT INTO nation VALUES (10, 'N_TARGET'), (20, 'N_OTHER')");
	connection.Query("INSERT INTO supplier VALUES (1000, 10), (1001, 10), (2000, 20)");
}

std::string ReferenceQ11(const std::string &nation, double fraction) {
	return "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value FROM partsupp, supplier, nation "
	       "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = '" + Escape(nation) +
	       "' GROUP BY ps_partkey HAVING SUM(ps_supplycost * ps_availqty) > (SELECT SUM(ps_supplycost * ps_availqty) * " +
	       std::to_string(fraction) + " FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND "
	       "s_nationkey = n_nationkey AND n_name = '" + Escape(nation) + "') ORDER BY value DESC";
}

std::string ParallelQ11(const std::string &nation, double fraction) {
	return "SELECT * FROM accel_tpch_q11_parallel('" + Escape(nation) + "', " + std::to_string(fraction) + ")";
}

void RunCase(duckdb::Connection &connection, const std::string &label, const std::string &nation, double fraction,
             const std::function<void(duckdb::Connection &)> &setup) {
	ResetTables(connection);
	SeedBase(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ11(nation, fraction), ParallelQ11(nation, fraction));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		RunCase(connection, "qualifying_supplier", "N_TARGET", 0.5, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
		});

		RunCase(connection, "nonqualifying_supplier", "N_TARGET", 0.5, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 2000, 10, 100.00)");
		});

		RunCase(connection, "one_qualifying_row", "N_TARGET", 0.5, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
		});

		RunCase(connection, "multiple_qualifying_rows_same_part", "N_TARGET", 0.5, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (1, 1001, 5, 200.00)");
		});

		RunCase(connection, "mixed_suppliers_same_part", "N_TARGET", 0.5, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (1, 2000, 100, 100.00)");
		});

		RunCase(connection, "multiple_qualifying_parts", "N_TARGET", 0.1, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		RunCase(connection, "exact_threshold_excluded", "N_TARGET", 0.5, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		RunCase(connection, "just_above_threshold", "N_TARGET", 0.49, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		RunCase(connection, "just_below_threshold", "N_TARGET", 0.51, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		RunCase(connection, "zero_supply_cost", "N_TARGET", 0.0, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 0.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		RunCase(connection, "zero_availqty", "N_TARGET", 0.0, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 0, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		RunCase(connection, "duplicate_partsupp_rows", "N_TARGET", 0.1, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 10.00)");
		});

		RunCase(connection, "large_values", "N_TARGET", 0.1, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 1000000, 999999.99)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 1, 1.00)");
		});

		RunCase(connection, "no_qualifying_suppliers", "N_EMPTY", 0.5, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO nation VALUES (30, 'N_EMPTY')");
		});

		RunCase(connection, "no_qualifying_partsupp_rows", "N_TARGET", 0.5, [](duckdb::Connection &) {
		});

		RunCase(connection, "one_qualifying_part", "N_TARGET", 0.9, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 1, 1.00)");
		});

		RunCase(connection, "equal_output_values", "N_TARGET", 0.1, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		RunCase(connection, "descending_order", "N_TARGET", 0.0, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 1, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 2, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (3, 1000, 3, 100.00)");
		});

		RunCase(connection, "fraction_one_empty", "N_TARGET", 1.0, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO partsupp VALUES (1, 1000, 10, 100.00)");
			conn.Query("INSERT INTO partsupp VALUES (2, 1001, 10, 100.00)");
		});

		std::cout << "test_tpch_q31_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q31_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
