#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q10/tests/q10_manifest.hpp"

#include <algorithm>
#include <cstdlib>
#include <cctype>
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
	return accelerator_codegen::test_support::TpchDbPath();
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

// The generated Q10 instances omit the spec's LIMIT 20; add it to the reference.
std::string ReferenceQ10Sql(const std::string &path) {
	auto sql = ReadFile(path);
	while (!sql.empty() && (sql.back() == ';' || std::isspace(static_cast<unsigned char>(sql.back())))) {
		sql.pop_back();
	}
	return sql + "\nLIMIT 20";
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

std::string AcceleratorQ10Sql(const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q10(DATE '" + start_date + "')";
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
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
	auto expected_rows = MaterializeOrderedRows(expected_m);
	auto actual_rows = MaterializeOrderedRows(actual_m);
	std::sort(expected_rows.begin(), expected_rows.end());
	std::sort(actual_rows.begin(), actual_rows.end());
	for (duckdb::idx_t row = 0; row < expected_rows.size(); row++) {
		Require(expected_rows[row] == actual_rows[row], label + ": row " + std::to_string(row) + " differs");
	}
}

void CreateQ10Schema(duckdb::Connection &connection) {
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
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ10Schema(connection);
	connection.Query("INSERT INTO nation VALUES (10, 'INDIA', 1, 'c')");
	connection.Query(
	    "INSERT INTO customer VALUES (100, 'cust_a', 'addr', 10, 'phone', 1000.00, 'SEG', 'comment_a')");
	connection.Query("INSERT INTO orders VALUES (1000, 100, 'O', 100.00, DATE '1993-07-01', '1', 'c', 0, 'c')");
	connection.Query("INSERT INTO orders VALUES (1001, 100, 'O', 100.00, DATE '1993-03-01', '1', 'c', 0, 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (1000, 1, 1, 1, 1, 100.00, 0.10, 0, 'R', 'O', DATE '1993-07-01', DATE "
	    "'1993-07-01', DATE '1993-07-01', 'N', 'A', 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (1000, 1, 1, 2, 1, 50.00, 0.00, 0, 'N', 'O', DATE '1993-07-01', DATE "
	    "'1993-07-01', DATE '1993-07-01', 'N', 'A', 'c')");
}

} // namespace

int main() {
	try {
		{
			// Read-only: the smoke test never writes the shared database.
			duckdb::DBConfig file_database_config;
			file_database_config.options.access_mode = duckdb::AccessMode::READ_ONLY;
			duckdb::DuckDB file_database(GetTpchDbPath(), &file_database_config);
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (const auto &entry : accelerator_codegen::test_support::kQ10Manifest) {
				RequireEqualResults(connection, entry.label, ReferenceQ10Sql(entry.sql_path),
				                    AcceleratorQ10Sql(entry.start_date));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "SELECT c_custkey, c_name, SUM(l_extendedprice *(1 - l_discount)) AS revenue, c_acctbal, n_name, "
			    "c_address, c_phone, c_comment "
			    "FROM customer, orders, lineitem, nation "
			    "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND c_nationkey = n_nationkey "
			    "AND o_orderdate >= DATE '1993-06-01' AND o_orderdate < DATE '1993-06-01' + interval '3' month "
			    "AND l_returnflag = 'R' "
			    "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
			    "ORDER BY revenue DESC";
			RequireEqualResults(connection, "synthetic_returned_items", reference, AcceleratorQ10Sql("1993-06-01"));
		}

		std::cout << "test_tpch_q10 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q10 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
