#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "test_support/q7_manifest.hpp"

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

std::string AcceleratorQ7Sql(const std::string &nation1, const std::string &nation2) {
	return "SELECT * FROM accel_tpch_q7('" + nation1 + "', '" + nation2 + "')";
}

void RequireEqualQ7Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                           const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + " accelerator: " + actual->GetError());
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

void CreateQ7Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
	    "n_comment VARCHAR NOT NULL);"
	    "CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
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
}

void InsertNation(duckdb::Connection &connection, int nationkey, const std::string &name) {
	auto result = connection.Query("INSERT INTO nation VALUES (" + std::to_string(nationkey) + ", '" + name +
	                               "', 1, 'comment')");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ7Schema(connection);
	InsertNation(connection, 10, "BRAZIL");
	InsertNation(connection, 11, "UNITED KINGDOM");
	connection.Query("INSERT INTO customer VALUES (100, 'c', 'a', 10, 'p', 0, 'S', 'c')");
	connection.Query("INSERT INTO customer VALUES (101, 'c', 'a', 11, 'p', 0, 'S', 'c')");
	connection.Query("INSERT INTO supplier VALUES (200, 's', 'a', 10, 'p', 0, 'c')");
	connection.Query("INSERT INTO supplier VALUES (201, 's', 'a', 11, 'p', 0, 'c')");
	connection.Query("INSERT INTO orders VALUES (1000, 100, 'O', 100, DATE '1995-06-01', '1', 'c', 0, 'c')");
	connection.Query("INSERT INTO orders VALUES (1001, 101, 'O', 100, DATE '1995-06-01', '1', 'c', 0, 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (1000, 1, 200, 1, 1, 100.00, 0.10, 0, 'N', 'O', DATE '1995-06-01', DATE "
	    "'1995-06-01', DATE '1995-06-01', 'N', 'A', 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (1001, 1, 201, 1, 1, 50.00, 0.00, 0, 'N', 'O', DATE '1996-06-01', DATE "
	    "'1996-06-01', DATE '1996-06-01', 'N', 'A', 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (1001, 1, 200, 2, 1, 999.00, 0.00, 0, 'N', 'O', DATE '1994-01-01', DATE "
	    "'1994-01-01', DATE '1994-01-01', 'N', 'A', 'c')");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (const auto &entry : accelerator_codegen::test_support::kQ7Manifest) {
				RequireEqualQ7Results(connection, entry.label, ReadFile(entry.sql_path),
				                      AcceleratorQ7Sql(entry.nation1, entry.nation2));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference = ReadFile(
			    "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/07.sql");
			RequireEqualQ7Results(connection, "synthetic_bidirectional", reference,
			                      AcceleratorQ7Sql("UNITED KINGDOM", "BRAZIL"));
		}

		std::cout << "test_tpch_q7 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q7 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
