#include "accelerator_codegen/register.hpp"
#include "test_support/q5_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
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

std::string ReadFile(const std::string &path) {
	std::ifstream input(path);
	Require(input.good(), "failed to open file: " + path);
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

std::unique_ptr<duckdb::QueryResult> RunQuery(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	return result;
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

std::string AcceleratorQ5Sql(const std::string &region_name, const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q5('" + region_name + "', DATE '" + start_date + "')";
}

void RequireEqualQ5Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                           const std::string &accelerator_sql) {
	auto expected = RunQuery(connection, reference_sql);
	auto actual = RunQuery(connection, accelerator_sql);
	Require(expected->names == actual->names, label + ": column names differ");
	Require(expected->types == actual->types, label + ": column types differ");
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_m.RowCount() == actual_m.RowCount(), label + ": row count differs");
	const auto expected_rows = MaterializeOrderedRows(expected_m);
	const auto actual_rows = MaterializeOrderedRows(actual_m);
	for (duckdb::idx_t row = 0; row < expected_rows.size(); row++) {
		Require(expected_rows[row] == actual_rows[row], label + ": row " + std::to_string(row) + " differs");
	}
}

void RequireQueryErrorContains(duckdb::Connection &connection, const std::string &label, const std::string &sql,
                               const std::string &needle) {
	auto result = connection.Query(sql);
	Require(result != nullptr, label + ": query returned null result");
	Require(result->HasError(), label + ": expected query to fail");
	Require(result->GetError().find(needle) != std::string::npos,
	        label + ": expected error to contain '" + needle + "' but saw: " + result->GetError());
}

void CreateQ5Schema(duckdb::Connection &connection, const std::string &orderkey_type = "BIGINT") {
	auto result = connection.Query(
	    "CREATE TABLE region(r_regionkey INTEGER NOT NULL, r_name VARCHAR NOT NULL, r_comment VARCHAR NOT NULL);"
	    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
	    "n_comment VARCHAR NOT NULL);"
	    "CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
	    "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
	    "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL);"
	    "CREATE TABLE supplier(s_suppkey BIGINT NOT NULL, s_name VARCHAR NOT NULL, s_address VARCHAR NOT NULL, "
	    "s_nationkey INTEGER NOT NULL, s_phone VARCHAR NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL, "
	    "s_comment VARCHAR NOT NULL);"
	    "CREATE TABLE orders(o_orderkey " +
	    orderkey_type +
	    " NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, o_totalprice DECIMAL(15,2) NOT NULL, "
	    "o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, o_clerk VARCHAR NOT NULL, "
	    "o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), "failed to create q5 schema: " + result->GetError());
}

void InsertRegion(duckdb::Connection &connection, int64_t regionkey, const std::string &name) {
	auto result = connection.Query("INSERT INTO region VALUES (" + std::to_string(regionkey) + ", '" + name +
	                               "', 'comment')");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void InsertNation(duckdb::Connection &connection, int64_t nationkey, const std::string &name, int64_t regionkey) {
	auto result = connection.Query("INSERT INTO nation VALUES (" + std::to_string(nationkey) + ", '" + name + "', " +
	                               std::to_string(regionkey) + ", 'comment')");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void InsertCustomer(duckdb::Connection &connection, int64_t custkey, int64_t nationkey) {
	auto result = connection.Query("INSERT INTO customer VALUES (" + std::to_string(custkey) +
	                               ", 'cust', 'addr', " + std::to_string(nationkey) +
	                               ", 'phone', 0.00, 'SEG', 'comment')");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void InsertSupplier(duckdb::Connection &connection, int64_t suppkey, int64_t nationkey) {
	auto result = connection.Query("INSERT INTO supplier VALUES (" + std::to_string(suppkey) +
	                               ", 'supp', 'addr', " + std::to_string(nationkey) +
	                               ", 'phone', 0.00, 'comment')");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, int64_t custkey, const std::string &orderdate) {
	auto result = connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) + ", " +
	                               std::to_string(custkey) +
	                               ", 'O', 100.00, DATE '" + orderdate +
	                               "', '1-URGENT', 'clerk', 0, 'comment')");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t suppkey, int64_t linenumber,
                    const std::string &extendedprice, const std::string &discount) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, " + std::to_string(suppkey) + ", " +
	    std::to_string(linenumber) + ", 1.00, " + extendedprice + ", " + discount +
	    ", 0.00, 'N', 'O', DATE '1996-06-01', DATE '1996-06-01', DATE '1996-06-01', 'NONE', 'AIR', 'comment')");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ5Schema(connection);
	InsertRegion(connection, 1, "ASIA");
	InsertNation(connection, 10, "INDIA", 1);
	InsertNation(connection, 11, "CHINA", 1);
	InsertCustomer(connection, 100, 10);
	InsertCustomer(connection, 101, 11);
	InsertSupplier(connection, 200, 10);
	InsertSupplier(connection, 201, 11);

	InsertOrder(connection, 1000, 100, "1996-03-01");
	InsertOrder(connection, 1001, 101, "1996-08-01");
	InsertOrder(connection, 1002, 100, "1995-12-31"); // outside window
	InsertOrder(connection, 1003, 100, "1997-01-01"); // upper bound exclusive

	InsertLineitem(connection, 1000, 200, 1, "100.00", "0.10"); // INDIA revenue 90
	InsertLineitem(connection, 1001, 201, 1, "50.00", "0.00");  // CHINA revenue 50
	InsertLineitem(connection, 1000, 201, 2, "999.00", "0.00");  // nation mismatch
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ5Schema(connection);
	InsertRegion(connection, 1, "ASIA");
	InsertNation(connection, 10, "INDIA", 1);
	InsertCustomer(connection, 100, 10);
	InsertSupplier(connection, 200, 10);
	InsertOrder(connection, 1000, 100, "1996-03-01");
	InsertLineitem(connection, 1000, 200, 1, "100.00", "0.00");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			auto pragma = connection.Query("PRAGMA threads=1");
			Require(pragma != nullptr && !pragma->HasError(), "failed PRAGMA threads=1");

			for (const auto &entry : accelerator_codegen::test_support::kQ5Manifest) {
				RequireEqualQ5Results(connection, std::string(entry.label) + "_seed_file", ReadFile(entry.sql_path),
				                      AcceleratorQ5Sql(entry.region_name, entry.start_date));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "SELECT n_name, SUM(l_extendedprice *(1 - l_discount)) AS revenue "
			    "FROM customer, orders, lineitem, supplier, nation, region "
			    "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey "
			    "AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
			    "AND r_name = 'ASIA' AND o_orderdate >= DATE '1996-01-01' "
			    "AND o_orderdate < DATE '1996-01-01' + interval '1' year "
			    "GROUP BY n_name ORDER BY revenue DESC, n_name";
			RequireEqualQ5Results(connection, "synthetic_mixed", reference, AcceleratorQ5Sql("ASIA", "1996-01-01"));
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "SELECT n_name, SUM(l_extendedprice *(1 - l_discount)) AS revenue "
			    "FROM customer, orders, lineitem, supplier, nation, region "
			    "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey "
			    "AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
			    "AND r_name = 'ASIA' AND o_orderdate >= DATE '1996-01-01' "
			    "AND o_orderdate < DATE '1996-01-01' + interval '1' year "
			    "GROUP BY n_name ORDER BY revenue DESC, n_name";
			const std::string accel = AcceleratorQ5Sql("ASIA", "1996-01-01");

			RequireEqualQ5Results(connection, "transaction_baseline", reference, accel);
			connection.BeginTransaction();

			InsertLineitem(connection, 1000, 200, 2, "25.00", "0.00");
			RequireEqualQ5Results(connection, "after_insert", reference, accel);

			auto date_update = connection.Query("UPDATE orders SET o_orderdate = DATE '1995-01-01' WHERE o_orderkey = 1000");
			Require(date_update != nullptr && !date_update->HasError(), date_update->GetError());
			RequireEqualQ5Results(connection, "after_date_leave_window", reference, accel);

			auto nation_update = connection.Query("UPDATE customer SET c_nationkey = 99 WHERE c_custkey = 100");
			Require(nation_update != nullptr && !nation_update->HasError(), nation_update->GetError());
			RequireEqualQ5Results(connection, "after_customer_nation_change", reference, accel);

			connection.Rollback();
			RequireEqualQ5Results(connection, "after_rollback", reference, accel);
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			auto result = connection.Query(
			    "CREATE TABLE region(r_regionkey INTEGER NOT NULL, r_name VARCHAR NOT NULL, r_comment VARCHAR NOT NULL);"
			    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
			    "n_comment VARCHAR NOT NULL);"
			    "CREATE TABLE customer(c_custkey INTEGER NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
			    "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
			    "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL);"
			    "CREATE TABLE supplier(s_suppkey BIGINT NOT NULL, s_name VARCHAR NOT NULL, s_address VARCHAR NOT NULL, "
			    "s_nationkey INTEGER NOT NULL, s_phone VARCHAR NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL, "
			    "s_comment VARCHAR NOT NULL);"
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
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ5Sql("ASIA", "1996-01-01"),
			                          "Unsupported schema");
		}

		std::cout << "test_tpch_q5 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q5 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
