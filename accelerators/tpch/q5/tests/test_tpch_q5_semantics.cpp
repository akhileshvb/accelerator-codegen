#include "backends/duckdb/api/register.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_KERNELS
#error "test_tpch_q5_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
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
	connection.Query("DROP TABLE IF EXISTS nation");
	connection.Query("DROP TABLE IF EXISTS region");
	Require(!connection.Query(
	                   "CREATE TABLE region(r_regionkey INTEGER, r_name VARCHAR);"
	                   "CREATE TABLE nation(n_nationkey INTEGER, n_name VARCHAR, n_regionkey INTEGER);"
	                   "CREATE TABLE customer(c_custkey BIGINT, c_nationkey INTEGER);"
	                   "CREATE TABLE supplier(s_suppkey BIGINT, s_nationkey INTEGER);"
	                   "CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT, o_orderdate DATE);"
	                   "CREATE TABLE lineitem(l_orderkey BIGINT, l_suppkey BIGINT, l_extendedprice DECIMAL(15,2), "
	                   "l_discount DECIMAL(15,2));")
	             ->HasError(),
	        "create schema");
}

void SeedBase(duckdb::Connection &connection) {
	connection.Query("INSERT INTO region VALUES (1, 'R1'), (2, 'R2')");
	connection.Query("INSERT INTO nation VALUES (10, 'N10', 1), (20, 'N20', 1), (30, 'N30', 2)");
	connection.Query("INSERT INTO customer VALUES (1, 10), (2, 20), (3, 30)");
	connection.Query("INSERT INTO supplier VALUES (100, 10), (200, 20), (300, 30)");
}

std::string ReferenceQ5(const std::string &region, const char *start, const char *end) {
	return "SELECT n.n_name, SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue "
	       "FROM customer c, orders o, lineitem l, supplier s, nation n, region r "
	       "WHERE c.c_custkey = o.o_custkey AND l.l_orderkey = o.o_orderkey AND l.l_suppkey = s.s_suppkey "
	       "AND c.c_nationkey = s.s_nationkey AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey "
	       "AND r.r_name = '" + Escape(region) + "' AND o.o_orderdate >= DATE '" + start +
	       "' AND o.o_orderdate < DATE '" + end + "' GROUP BY n.n_name ORDER BY revenue DESC";
}

std::string ParallelQ5(const std::string &region, const char *start) {
	return "SELECT * FROM accel_tpch_q5_parallel('" + Escape(region) + "', DATE '" + start + "')";
}

void RunCase(duckdb::Connection &connection, const std::string &label, const std::string &region, const char *start,
             const char *end, const std::function<void(duckdb::Connection &)> &setup) {
	ResetTables(connection);
	SeedBase(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ5(region, start, end), ParallelQ5(region, start));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=4");

		RunCase(connection, "same_nation_match", "R1", "1996-01-01", "1997-01-01", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 1, DATE '1996-06-01')");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, 100.00, 0.10)");
		});

		RunCase(connection, "different_nation_same_region", "R1", "1996-01-01", "1997-01-01", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 2, DATE '1996-06-01')");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, 100.00, 0.10)");
		});

		RunCase(connection, "date_lower_boundary", "R1", "1996-01-01", "1997-01-01", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 1, DATE '1996-01-01')");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, 50.00, 0.00)");
		});

		RunCase(connection, "date_upper_boundary_excluded", "R1", "1996-01-01", "1997-01-01", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 1, DATE '1997-01-01')");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, 50.00, 0.00)");
		});

		RunCase(connection, "multiple_lineitems", "R1", "1996-01-01", "1997-01-01", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 1, DATE '1996-06-01')");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, 100.00, 0.10), (1000, 100, 200.00, 0.20)");
		});

		RunCase(connection, "zero_rows", "R2", "1996-01-01", "1997-01-01", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 1, DATE '1996-06-01')");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, 100.00, 0.10)");
		});

		std::cout << "test_tpch_q5_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q5_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
