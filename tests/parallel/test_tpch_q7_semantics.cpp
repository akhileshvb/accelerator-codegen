#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q7_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
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
	Require(!connection.Query(
	                   "CREATE TABLE nation(n_nationkey INTEGER, n_name VARCHAR);"
	                   "CREATE TABLE customer(c_custkey BIGINT, c_nationkey INTEGER);"
	                   "CREATE TABLE supplier(s_suppkey BIGINT, s_nationkey INTEGER);"
	                   "CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT);"
	                   "CREATE TABLE lineitem(l_orderkey BIGINT, l_suppkey BIGINT, l_shipdate DATE, "
	                   "l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2));")
	             ->HasError(),
	        "create schema");
}

void SeedBase(duckdb::Connection &connection) {
	connection.Query("INSERT INTO nation VALUES (1, 'N1'), (2, 'N2'), (3, 'N3')");
	connection.Query("INSERT INTO customer VALUES (10, 1), (20, 2), (30, 3)");
	connection.Query("INSERT INTO supplier VALUES (100, 1), (200, 2), (300, 3)");
}

std::string ReferenceQ7(const std::string &nation1, const std::string &nation2) {
	return "WITH shipping AS ("
	       "SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, EXTRACT(year FROM l_shipdate) AS l_year, "
	       "l_extendedprice * (1 - l_discount) AS volume "
	       "FROM supplier, lineitem, orders, customer, nation n1, nation n2 "
	       "WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey AND c_custkey = o_custkey "
	       "AND s_nationkey = n1.n_nationkey AND c_nationkey = n2.n_nationkey "
	       "AND ((n1.n_name = '" + Escape(nation1) + "' AND n2.n_name = '" + Escape(nation2) + "') OR "
	       "(n1.n_name = '" + Escape(nation2) + "' AND n2.n_name = '" + Escape(nation1) + "')) "
	       "AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') "
	       "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM shipping "
	       "GROUP BY supp_nation, cust_nation, l_year ORDER BY supp_nation, cust_nation, l_year";
}

std::string ParallelQ7(const std::string &nation1, const std::string &nation2) {
	return "SELECT * FROM accel_tpch_q7_parallel('" + Escape(nation1) + "', '" + Escape(nation2) + "')";
}

void RunCase(duckdb::Connection &connection, const std::string &label, const std::string &nation1,
             const std::string &nation2, const std::function<void(duckdb::Connection &)> &setup) {
	ResetTables(connection);
	SeedBase(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ7(nation1, nation2), ParallelQ7(nation1, nation2));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=4");

		RunCase(connection, "cross_nation_n1_supp_n2_cust", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10)");
			c.Query("INSERT INTO lineitem VALUES (1000, 200, DATE '1995-06-01', 100.00, 0.10)");
		});

		RunCase(connection, "cross_nation_n2_supp_n1_cust", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 20)");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, DATE '1996-06-01', 200.00, 0.20)");
		});

		RunCase(connection, "same_nation_both_n1", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10)");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, DATE '1995-06-01', 100.00, 0.10)");
		});

		RunCase(connection, "same_nation_both_n2", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 20)");
			c.Query("INSERT INTO lineitem VALUES (1000, 200, DATE '1995-06-01', 100.00, 0.10)");
		});

		RunCase(connection, "customer_outside_selected", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 30)");
			c.Query("INSERT INTO lineitem VALUES (1000, 100, DATE '1995-06-01', 100.00, 0.10)");
		});

		RunCase(connection, "supplier_outside_selected", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10)");
			c.Query("INSERT INTO lineitem VALUES (1000, 300, DATE '1995-06-01', 100.00, 0.10)");
		});

		RunCase(connection, "shipdate_lower_boundary", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10)");
			c.Query("INSERT INTO lineitem VALUES (1000, 200, DATE '1995-01-01', 100.00, 0.00)");
		});

		RunCase(connection, "shipdate_upper_boundary", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10)");
			c.Query("INSERT INTO lineitem VALUES (1000, 200, DATE '1996-12-31', 100.00, 0.00)");
		});

		RunCase(connection, "shipdate_before_interval", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10)");
			c.Query("INSERT INTO lineitem VALUES (1000, 200, DATE '1994-12-31', 100.00, 0.00)");
		});

		RunCase(connection, "shipdate_after_interval", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10)");
			c.Query("INSERT INTO lineitem VALUES (1000, 200, DATE '1997-01-01', 100.00, 0.00)");
		});

		RunCase(connection, "multiple_lineitems_same_order", "N1", "N2", [](duckdb::Connection &c) {
			c.Query("INSERT INTO orders VALUES (1000, 10), (1001, 20)");
			c.Query("INSERT INTO lineitem VALUES (1000, 200, DATE '1995-06-01', 100.00, 0.10), "
			        "(1000, 200, DATE '1995-07-01', 50.00, 0.00), "
			        "(1001, 100, DATE '1996-06-01', 200.00, 0.20)");
		});

		RunCase(connection, "zero_qualifying_rows", "N1", "N2", [](duckdb::Connection &) {
		});

		std::cout << "test_tpch_q7_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q7_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
