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
#error "test_tpch_q21_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

constexpr const char *kNationName = "TESTNATION";

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

std::string ReferenceQ21Sql() {
	return std::string(
	           "SELECT s_name, COUNT(*) AS numwait "
	           "FROM supplier, lineitem l1, orders, nation "
	           "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F' "
	           "AND l1.l_receiptdate > l1.l_commitdate "
	           "AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey) "
	           "AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey "
	           "AND l3.l_receiptdate > l3.l_commitdate) "
	           "AND s_nationkey = n_nationkey AND n_name = '") +
	       kNationName + "' GROUP BY s_name ORDER BY numwait DESC, s_name";
}

std::string AcceleratorQ21Sql() {
	return std::string("SELECT * FROM accel_tpch_q21('") + kNationName + "')";
}

std::string AcceleratorQ21ParallelSql() {
	return std::string("SELECT * FROM accel_tpch_q21_parallel('") + kNationName + "')";
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

void CreateQ21Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE supplier(s_suppkey BIGINT NOT NULL, s_name VARCHAR NOT NULL, s_address VARCHAR NOT NULL, "
	    "s_nationkey INTEGER NOT NULL, s_phone VARCHAR NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL, s_comment VARCHAR NOT NULL);"
	    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, n_comment VARCHAR NOT NULL);"
	    "CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
	    "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, "
	    "l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), result->GetError());
	connection.Query("INSERT INTO nation VALUES (1, '" + std::string(kNationName) + "', 0, 'c')");
	connection.Query("INSERT INTO nation VALUES (2, 'OTHER', 0, 'c')");
}

void InsertSupplier(duckdb::Connection &connection, int64_t suppkey, const std::string &name, int nationkey) {
	connection.Query("INSERT INTO supplier VALUES (" + std::to_string(suppkey) + ", '" + name + "', 'addr', " +
	                 std::to_string(nationkey) + ", 'phone', 0.00, 'c')");
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, char status = 'F') {
	connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) +
	                 ", 1, '" + std::string(1, status) + "', 1.00, DATE '1995-01-01', '1', 'c', 0, 'c')");
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t suppkey, int64_t lineno,
                    const char *commit_date, const char *receipt_date) {
	connection.Query("INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, " + std::to_string(suppkey) +
	                 ", " + std::to_string(lineno) +
	                 ", 1, 1.00, 0.00, 0, 'N', 'O', DATE '1995-01-01', DATE '" + commit_date + "', DATE '" +
	                 receipt_date + "', 'N', 'A', 'c')");
}

struct SemanticFixture {
	std::string label;
	std::function<void(duckdb::Connection &)> setup;
};

void RunSemanticCase(duckdb::Connection &connection, const SemanticFixture &fixture) {
	connection.Query("DROP TABLE IF EXISTS lineitem");
	connection.Query("DROP TABLE IF EXISTS orders");
	connection.Query("DROP TABLE IF EXISTS supplier");
	connection.Query("DROP TABLE IF EXISTS nation");
	CreateQ21Schema(connection);
	fixture.setup(connection);
	const auto reference = ReferenceQ21Sql();
	RequireEqualResults(connection, fixture.label + "/serial", reference, AcceleratorQ21Sql());
	RequireEqualResults(connection, fixture.label + "/parallel", reference, AcceleratorQ21ParallelSql());
}

} // namespace

int main() {
	try {
		duckdb::DuckDB memory_database(nullptr);
		duckdb::Connection connection(memory_database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const std::vector<SemanticFixture> fixtures = {
		    {"one_supplier_one_late",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
		     }},
		    {"one_supplier_multiple_late",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 1, 2, "1995-01-01", "1995-01-03");
		     }},
		    {"two_suppliers_one_late",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertSupplier(c, 2, "other_b", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 2, 2, "1995-01-02", "1995-01-01");
		     }},
		    {"two_suppliers_both_late",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertSupplier(c, 2, "other_b", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 2, 2, "1995-01-01", "1995-01-02");
		     }},
		    {"three_suppliers_one_late",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertSupplier(c, 2, "other_b", 1);
			     InsertSupplier(c, 3, "other_c", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 2, 2, "1995-01-02", "1995-01-01");
			     InsertLineitem(c, 100, 3, 3, "1995-01-02", "1995-01-01");
		     }},
		    {"several_late_from_sole_late_supplier",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertSupplier(c, 2, "other_b", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 1, 2, "1995-01-01", "1995-01-03");
			     InsertLineitem(c, 100, 1, 3, "1995-01-01", "1995-01-04");
			     InsertLineitem(c, 100, 2, 4, "1995-01-02", "1995-01-01");
		     }},
		    {"on_time_from_other_supplier",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertSupplier(c, 2, "other_b", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 2, 2, "1995-01-01", "1995-01-01");
			     InsertLineitem(c, 100, 2, 3, "1995-01-01", "1995-01-01");
			     InsertLineitem(c, 100, 2, 4, "1995-01-01", "1995-01-01");
		     }},
		    {"duplicate_lineitems_same_supplier",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertSupplier(c, 2, "other_b", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 1, 2, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 2, 3, "1995-01-02", "1995-01-01");
		     }},
		    {"sole_late_supplier_outside_nation",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "foreign_a", 2);
			     InsertSupplier(c, 2, "qual_b", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 2, 2, "1995-01-02", "1995-01-01");
		     }},
		    {"qualifying_supplier_without_other_supplier",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 1, 2, "1995-01-01", "1995-01-03");
		     }},
		    {"non_qualifying_late_blocks_qualifying",
		     [](duckdb::Connection &c) {
			     InsertSupplier(c, 1, "qual_a", 1);
			     InsertSupplier(c, 2, "foreign_b", 2);
			     InsertOrder(c, 100);
			     InsertLineitem(c, 100, 1, 1, "1995-01-01", "1995-01-02");
			     InsertLineitem(c, 100, 2, 2, "1995-01-01", "1995-01-03");
		     }},
		};

		for (const auto &fixture : fixtures) {
			RunSemanticCase(connection, fixture);
		}

		std::cout << "test_tpch_q21_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q21_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
