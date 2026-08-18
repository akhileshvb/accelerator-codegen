#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q8_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
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
	Require(MaterializeRows(expected_m) == MaterializeRows(actual_m), label + ": rows differ");
}

void ResetTables(duckdb::Connection &connection) {
	connection.Query("DROP TABLE IF EXISTS lineitem");
	connection.Query("DROP TABLE IF EXISTS orders");
	connection.Query("DROP TABLE IF EXISTS supplier");
	connection.Query("DROP TABLE IF EXISTS customer");
	connection.Query("DROP TABLE IF EXISTS part");
	connection.Query("DROP TABLE IF EXISTS nation");
	connection.Query("DROP TABLE IF EXISTS region");
	Require(!connection.Query(
	                   "CREATE TABLE region(r_regionkey INTEGER, r_name VARCHAR);"
	                   "CREATE TABLE nation(n_nationkey INTEGER, n_name VARCHAR, n_regionkey INTEGER);"
	                   "CREATE TABLE customer(c_custkey BIGINT, c_nationkey INTEGER);"
	                   "CREATE TABLE supplier(s_suppkey BIGINT, s_nationkey INTEGER);"
	                   "CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT, o_orderdate DATE);"
	                   "CREATE TABLE part(p_partkey BIGINT, p_type VARCHAR);"
	                   "CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, "
	                   "l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2));")
	             ->HasError(),
	        "create schema");
}

void SeedBase(duckdb::Connection &connection) {
	connection.Query("INSERT INTO region VALUES (1, 'R1'), (2, 'R2')");
	connection.Query("INSERT INTO nation VALUES (10, 'N_TARGET', 1), (11, 'N_OTHER', 1), (20, 'N_OUT', 2)");
	connection.Query("INSERT INTO customer VALUES (100, 10), (101, 11), (200, 20)");
	connection.Query("INSERT INTO supplier VALUES (1000, 10), (1001, 11), (2000, 20)");
}

std::string ReferenceQ8(const std::string &nation, const std::string &region, const std::string &part_suffix) {
	return "SELECT o_year, SUM(CASE WHEN nation = '" + Escape(nation) +
	       "' THEN volume ELSE 0 END) / SUM(volume) AS mkt_share FROM ("
	       "SELECT EXTRACT(year FROM o_orderdate) AS o_year, l_extendedprice * (1 - l_discount) AS volume, "
	       "n2.n_name AS nation FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
	       "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
	       "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey AND r_name = '" + Escape(region) +
	       "' AND s_nationkey = n2.n_nationkey AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
	       "AND p_type LIKE '%" + Escape(part_suffix) + "') AS all_nations GROUP BY o_year ORDER BY o_year";
}

std::string ParallelQ8(const std::string &nation, const std::string &region, const std::string &part_suffix) {
	return "SELECT * FROM accel_tpch_q8_parallel('" + Escape(nation) + "', '" + Escape(region) + "', '" +
	       Escape(part_suffix) + "')";
}

void RunCase(duckdb::Connection &connection, const std::string &label, const std::string &nation,
             const std::string &region, const std::string &part_suffix,
             const std::function<void(duckdb::Connection &)> &setup) {
	ResetTables(connection);
	SeedBase(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ8(nation, region, part_suffix),
	                    ParallelQ8(nation, region, part_suffix));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=4");

		RunCase(connection, "target_nation_numerator", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL BRASS'), (2, 'STEEL')");
			c.Query("INSERT INTO orders VALUES (5000, 100, DATE '1995-06-01')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1000, 100.00, 0.10)");
		});

		RunCase(connection, "non_target_supplier_denominator", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL BRASS')");
			c.Query("INSERT INTO orders VALUES (5000, 100, DATE '1995-06-01')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1001, 200.00, 0.00)");
		});

		RunCase(connection, "customer_outside_region", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL BRASS')");
			c.Query("INSERT INTO orders VALUES (5000, 200, DATE '1995-06-01')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1000, 100.00, 0.00)");
		});

		RunCase(connection, "nonmatching_part_type", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL STEEL')");
			c.Query("INSERT INTO orders VALUES (5000, 100, DATE '1995-06-01')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1000, 100.00, 0.00)");
		});

		RunCase(connection, "order_date_lower_boundary", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL BRASS')");
			c.Query("INSERT INTO orders VALUES (5000, 100, DATE '1995-01-01')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1000, 100.00, 0.00)");
		});

		RunCase(connection, "order_date_upper_boundary", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL BRASS')");
			c.Query("INSERT INTO orders VALUES (5000, 100, DATE '1996-12-31')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1000, 100.00, 0.00)");
		});

		RunCase(connection, "order_date_outside", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL BRASS')");
			c.Query("INSERT INTO orders VALUES (5000, 100, DATE '1997-01-01')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1000, 100.00, 0.00)");
		});

		RunCase(connection, "two_years_mixed", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &c) {
			c.Query("INSERT INTO part VALUES (1, 'SMALL BRASS')");
			c.Query("INSERT INTO orders VALUES (5000, 100, DATE '1995-06-01'), (5001, 101, DATE '1996-06-01')");
			c.Query("INSERT INTO lineitem VALUES (5000, 1, 1000, 100.00, 0.00), (5001, 1, 1001, 50.00, 0.00)");
		});

		RunCase(connection, "zero_qualifying_rows", "N_TARGET", "R1", "BRASS", [](duckdb::Connection &) {
		});

		std::cout << "test_tpch_q8_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q8_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
