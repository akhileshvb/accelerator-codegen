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
#error "test_tpch_q3_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string ReferenceQ3Sql(const std::string &segment, const std::string &cutoff_date) {
	return "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority "
	       "FROM customer, orders, lineitem "
	       "WHERE c_mktsegment = '" + segment + "' AND c_custkey = o_custkey AND l_orderkey = o_orderkey "
	       "AND o_orderdate < DATE '" + cutoff_date + "' AND l_shipdate > DATE '" + cutoff_date + "' "
	       "GROUP BY l_orderkey, o_orderdate, o_shippriority "
	       "ORDER BY revenue DESC, o_orderdate LIMIT 10";
}

std::string AcceleratorQ3Sql(const std::string &segment, const std::string &cutoff_date) {
	return "SELECT * FROM accel_tpch_q3('" + segment + "', DATE '" + cutoff_date + "')";
}

std::string AcceleratorQ3ParallelSql(const std::string &segment, const std::string &cutoff_date) {
	return "SELECT * FROM accel_tpch_q3_parallel('" + segment + "', DATE '" + cutoff_date + "')";
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

void InsertCustomer(duckdb::Connection &connection, int64_t custkey, const std::string &segment) {
	connection.Query("INSERT INTO customer VALUES (" + std::to_string(custkey) + ", 'n', 'a', 1, 'p', 0, '" +
	                 segment + "', 'c')");
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, int64_t custkey, const char *orderdate,
                 int32_t shippriority) {
	connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) + ", " + std::to_string(custkey) +
	                 ", 'O', 100.00, DATE '" + orderdate + "', '1', 'c', " + std::to_string(shippriority) + ", 'c')");
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t lineno, const char *shipdate,
                    const char *extendedprice, const char *discount) {
	connection.Query("INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, 1, " + std::to_string(lineno) +
	                 ", 10.00, " + extendedprice + ", " + discount + ", 0.00, 'N', 'O', DATE '" + shipdate +
	                 "', DATE '1993-01-01', DATE '1993-01-01', 'N', 'A', 'c')");
}

struct SemanticFixture {
	std::string label;
	std::string segment;
	std::string cutoff_date;
	std::function<void(duckdb::Connection &)> setup;
};

void RunSemanticCase(duckdb::Connection &connection, const SemanticFixture &fixture) {
	connection.Query("DROP TABLE IF EXISTS lineitem");
	connection.Query("DROP TABLE IF EXISTS orders");
	connection.Query("DROP TABLE IF EXISTS customer");
	CreateSchema(connection);
	fixture.setup(connection);
	const auto reference = ReferenceQ3Sql(fixture.segment, fixture.cutoff_date);
	RequireEqualResults(connection, fixture.label + "/serial", reference, AcceleratorQ3Sql(fixture.segment, fixture.cutoff_date));
	RequireEqualResults(connection, fixture.label + "/parallel", reference,
	                    AcceleratorQ3ParallelSql(fixture.segment, fixture.cutoff_date));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=4");

		const std::vector<SemanticFixture> fixtures = {
		    {"matching_segment", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "BUILDING");
			     InsertOrder(c, 100, 1, "1995-03-01", 0);
			     InsertLineitem(c, 100, 1, "1995-03-20", "100.00", "0.10");
		     }},
		    {"nonmatching_segment", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "FURNITURE");
			     InsertOrder(c, 100, 1, "1995-03-01", 0);
			     InsertLineitem(c, 100, 1, "1995-03-20", "100.00", "0.10");
		     }},
		    {"order_on_boundary", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "BUILDING");
			     InsertOrder(c, 100, 1, "1995-03-15", 0);
			     InsertLineitem(c, 100, 1, "1995-03-20", "100.00", "0.00");
		     }},
		    {"order_before_boundary", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "BUILDING");
			     InsertOrder(c, 100, 1, "1995-03-14", 0);
			     InsertLineitem(c, 100, 1, "1995-03-20", "100.00", "0.00");
		     }},
		    {"ship_on_boundary", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "BUILDING");
			     InsertOrder(c, 100, 1, "1995-03-01", 0);
			     InsertLineitem(c, 100, 1, "1995-03-15", "100.00", "0.00");
		     }},
		    {"ship_after_boundary", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "BUILDING");
			     InsertOrder(c, 100, 1, "1995-03-01", 0);
			     InsertLineitem(c, 100, 1, "1995-03-16", "100.00", "0.00");
		     }},
		    {"multi_lineitem_order", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "BUILDING");
			     InsertOrder(c, 100, 1, "1995-03-01", 0);
			     InsertLineitem(c, 100, 1, "1995-03-20", "100.00", "0.10");
			     InsertLineitem(c, 100, 2, "1995-03-21", "50.00", "0.20");
		     }},
		    {"equal_revenue_tiebreak", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "BUILDING");
			     InsertOrder(c, 100, 1, "1995-03-02", 0);
			     InsertOrder(c, 200, 1, "1995-03-01", 0);
			     InsertLineitem(c, 100, 1, "1995-03-20", "100.00", "0.00");
			     InsertLineitem(c, 200, 1, "1995-03-20", "100.00", "0.00");
		     }},
		    {"zero_results", "BUILDING", "1995-03-15", [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "FURNITURE");
		     }},
		};

		for (const auto &fixture : fixtures) {
			RunSemanticCase(connection, fixture);
		}

		std::cout << "test_tpch_q3_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q3_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
