#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q38_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string ReferenceQ18Sql(int64_t threshold) {
	return "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity) "
	       "FROM customer, orders, lineitem "
	       "WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > " +
	       std::to_string(threshold) + ") "
	       "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
	       "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
	       "ORDER BY o_totalprice DESC, o_orderdate";
}

std::string AcceleratorQ18Sql(int64_t threshold) {
	return "SELECT * FROM accel_tpch_q18(" + std::to_string(threshold) + ")";
}

std::string AcceleratorQ18ParallelSql(int64_t threshold) {
	return "SELECT * FROM accel_tpch_q18_parallel(" + std::to_string(threshold) + ")";
}

std::vector<std::string> MaterializeRows(duckdb::MaterializedQueryResult &result) {
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

void CreateSchema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
	    "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
	    "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL);"
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
}

void InsertCustomer(duckdb::Connection &connection, int64_t custkey, const std::string &name) {
	connection.Query("INSERT INTO customer VALUES (" + std::to_string(custkey) + ", '" + name +
	                 "', 'a', 1, 'p', 0, 's', 'c')");
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, int64_t custkey, const char *totalprice,
                 const char *orderdate) {
	connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) + ", " + std::to_string(custkey) +
	                 ", 'O', " + totalprice + ", DATE '" + orderdate + "', '1', 'c', 0, 'c')");
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t lineno, const char *quantity) {
	connection.Query("INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, 1, " + std::to_string(lineno) +
	                 ", " + quantity + ", 10.00, 0.00, 0.00, 'N', 'O', DATE '1993-01-01', DATE '1993-01-01', DATE "
	                 "'1993-01-01', 'N', 'A', 'c')");
}

struct SemanticFixture {
	std::string label;
	int64_t threshold = 0;
	std::function<void(duckdb::Connection &)> setup;
};

void RunSemanticCase(duckdb::Connection &connection, const SemanticFixture &fixture) {
	connection.Query("DROP TABLE IF EXISTS lineitem");
	connection.Query("DROP TABLE IF EXISTS orders");
	connection.Query("DROP TABLE IF EXISTS customer");
	CreateSchema(connection);
	fixture.setup(connection);
	const auto reference = ReferenceQ18Sql(fixture.threshold);
	RequireEqualResults(connection, fixture.label + "/serial", reference, AcceleratorQ18Sql(fixture.threshold));
	RequireEqualResults(connection, fixture.label + "/parallel", reference, AcceleratorQ18ParallelSql(fixture.threshold));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=4");

		const std::vector<SemanticFixture> fixtures = {
		    {"single_lineitem", 100, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "100.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "150.00");
		     }},
		    {"multi_lineitem_sum", 300, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "200.00");
			     InsertLineitem(c, 100, 2, "150.00");
		     }},
		    {"exact_threshold_not_qualifying", 350, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "200.00");
			     InsertLineitem(c, 100, 2, "150.00");
		     }},
		    {"just_above_threshold", 349, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "200.00");
			     InsertLineitem(c, 100, 2, "150.00");
		     }},
		    {"just_below_threshold", 351, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "200.00");
			     InsertLineitem(c, 100, 2, "150.00");
		     }},
		    {"multiple_qualifying_orders", 100, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertCustomer(c, 2, "cust_b");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertOrder(c, 200, 2, "400.00", "1993-02-01");
			     InsertLineitem(c, 100, 1, "200.00");
			     InsertLineitem(c, 200, 1, "150.00");
		     }},
		    {"mixed_qualifying_and_nonqualifying", 200, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertOrder(c, 200, 1, "300.00", "1993-02-01");
			     InsertLineitem(c, 100, 1, "250.00");
			     InsertLineitem(c, 200, 1, "100.00");
		     }},
		    {"duplicate_lineitems", 100, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "120.00");
			     InsertLineitem(c, 100, 2, "120.00");
		     }},
		    {"equal_totalprice_tiebreak_by_date", 100, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertCustomer(c, 2, "cust_b");
			     InsertOrder(c, 100, 1, "500.00", "1993-02-01");
			     InsertOrder(c, 200, 2, "500.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "200.00");
			     InsertLineitem(c, 200, 1, "200.00");
		     }},
		    {"zero_quantity", 0, [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "cust_a");
			     InsertOrder(c, 100, 1, "500.00", "1993-01-01");
			     InsertLineitem(c, 100, 1, "0.00");
		     }},
		};

		for (const auto &fixture : fixtures) {
			RunSemanticCase(connection, fixture);
		}

		std::cout << "test_tpch_q38_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q38_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
