#include "backends/duckdb/api/register.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_KERNELS
#error "test_tpch_q30_semantics requires -DACCEL_ENABLE_PARALLEL_KERNELS"
#endif

namespace {

constexpr const char *kStartDate = "1993-06-01";

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::vector<std::string> MaterializeOrderedRows(duckdb::MaterializedQueryResult &result) {
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

std::string ReferenceQ10Sql() {
	return std::string(
	           "SELECT c_custkey, c_name, SUM(l_extendedprice *(1 - l_discount)) AS revenue, c_acctbal, n_name, "
	           "c_address, c_phone, c_comment "
	           "FROM customer, orders, lineitem, nation "
	           "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND c_nationkey = n_nationkey "
	           "AND o_orderdate >= DATE '") +
	       kStartDate + "' AND o_orderdate < DATE '" + kStartDate +
	       "' + interval '3' month AND l_returnflag = 'R' "
	       "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
	       "ORDER BY revenue DESC, c_custkey";
}

std::string AcceleratorQ10Sql() {
	return std::string("SELECT * FROM accel_tpch_q10(DATE '") + kStartDate + "')";
}

std::string AcceleratorQ10ParallelSql() {
	return std::string("SELECT * FROM accel_tpch_q10_parallel(DATE '") + kStartDate + "')";
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
	const auto expected_rows = MaterializeOrderedRows(expected_m);
	const auto actual_rows = MaterializeOrderedRows(actual_m);
	for (duckdb::idx_t row = 0; row < expected_rows.size(); row++) {
		Require(expected_rows[row] == actual_rows[row], label + ": row " + std::to_string(row) + " differs");
	}
}

void CreateQ30Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
	    "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
	    "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL);"
	    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
	    "n_comment VARCHAR NOT NULL);"
	    "CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
	    "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), result->GetError());
	connection.Query("INSERT INTO nation VALUES (10, 'INDIA', 1, 'c')");
	connection.Query("INSERT INTO nation VALUES (20, 'FRANCE', 1, 'c')");
}

void InsertCustomer(duckdb::Connection &connection, int64_t custkey, const std::string &name, int nationkey,
                    const std::string &acctbal) {
	connection.Query("INSERT INTO customer VALUES (" + std::to_string(custkey) + ", '" + name + "', 'addr_" +
	                 std::to_string(custkey) + "', " + std::to_string(nationkey) + ", 'phone_" +
	                 std::to_string(custkey) + "', " + acctbal + ", 'SEG', 'comment_" + std::to_string(custkey) + "')");
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, int64_t custkey, const char *orderdate) {
	connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) + ", " + std::to_string(custkey) +
	                 ", 'O', 100.00, DATE '" + orderdate + "', '1', 'c', 0, 'c')");
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t lineno, const char *returnflag,
                    const char *extendedprice, const char *discount) {
	connection.Query("INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, 1, " +
	                 std::to_string(lineno) + ", 1, " + extendedprice + ", " + discount +
	                 ", 0, '" + std::string(returnflag) +
	                 "', 'O', DATE '1993-07-01', DATE '1993-07-01', DATE '1993-07-01', 'N', 'A', 'c')");
}

struct SemanticFixture {
	std::string label;
	std::function<void(duckdb::Connection &)> setup;
};

void RunSemanticCase(duckdb::Connection &connection, const SemanticFixture &fixture) {
	connection.Query("DROP TABLE IF EXISTS lineitem");
	connection.Query("DROP TABLE IF EXISTS orders");
	connection.Query("DROP TABLE IF EXISTS customer");
	connection.Query("DROP TABLE IF EXISTS nation");
	CreateQ30Schema(connection);
	fixture.setup(connection);
	const auto reference = ReferenceQ10Sql();
	RequireEqualResults(connection, fixture.label + "/serial", reference, AcceleratorQ10Sql());
	RequireEqualResults(connection, fixture.label + "/parallel", reference, AcceleratorQ10ParallelSql());
}

} // namespace

int main() {
	try {
		duckdb::DuckDB memory_database(nullptr);
		duckdb::Connection connection(memory_database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const std::vector<SemanticFixture> fixtures = {
		    {"lower_date_boundary_inclusive",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-06-01");
			     InsertLineitem(c, 100, 1, "R", "100.00", "0.10");
		     }},
		    {"upper_date_boundary_exclusive",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-08-31");
			     InsertOrder(c, 101, 1, "1993-09-01");
			     InsertLineitem(c, 100, 1, "R", "100.00", "0.00");
			     InsertLineitem(c, 101, 1, "R", "50.00", "0.00");
		     }},
		    {"returned_lineitem_contributes",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "100.00", "0.10");
		     }},
		    {"non_returned_lineitem_ignored",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "N", "100.00", "0.00");
		     }},
		    {"multiple_returned_lineitems_same_order",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "100.00", "0.10");
			     InsertLineitem(c, 100, 2, "R", "50.00", "0.00");
		     }},
		    {"multiple_qualifying_orders_same_customer",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertOrder(c, 101, 1, "1993-08-01");
			     InsertLineitem(c, 100, 1, "R", "100.00", "0.00");
			     InsertLineitem(c, 101, 1, "R", "25.00", "0.00");
		     }},
		    {"equal_revenue_tie_break_by_custkey",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 2, "cust_b", 10, "2000.00");
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertOrder(c, 101, 2, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "90.00", "0.00");
			     InsertLineitem(c, 101, 1, "R", "90.00", "0.00");
		     }},
		    {"qualifying_order_no_returned_lineitems",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "N", "100.00", "0.00");
		     }},
		    {"returned_lineitem_order_outside_interval",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-05-31");
			     InsertLineitem(c, 100, 1, "R", "100.00", "0.00");
		     }},
		    {"decimal_discount_semantics",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "100.00", "0.10");
		     }},
		    {"customer_zero_qualifying_revenue_excluded",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertCustomer(c, 2, "cust_b", 10, "2000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "50.00", "0.00");
		     }},
		    {"nation_lookup_correctness",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 20, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "10.00", "0.00");
		     }},
		    {"duplicate_lineitems_affect_revenue",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "40.00", "0.00");
			     InsertLineitem(c, 100, 2, "R", "40.00", "0.00");
		     }},
		    {"mixed_return_flags_same_order",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a", 10, "1000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "80.00", "0.00");
			     InsertLineitem(c, 100, 2, "N", "200.00", "0.00");
		     }},
		    {"two_customers_different_revenue_ordering",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_low", 10, "1000.00");
			     InsertCustomer(c, 2, "cust_high", 10, "2000.00");
			     InsertOrder(c, 100, 1, "1993-07-01");
			     InsertOrder(c, 101, 2, "1993-07-01");
			     InsertLineitem(c, 100, 1, "R", "10.00", "0.00");
			     InsertLineitem(c, 101, 1, "R", "90.00", "0.00");
		     }},
		};

		for (const auto &fixture : fixtures) {
			RunSemanticCase(connection, fixture);
		}

		std::cout << "test_tpch_q30_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q30_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
