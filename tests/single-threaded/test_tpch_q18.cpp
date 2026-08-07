#include "accelerator_codegen/register.hpp"
#include "test_support/q18_manifest.hpp"

#include <algorithm>
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

std::string AcceleratorQ18Sql(int64_t quantity_threshold) {
	return "SELECT * FROM accel_tpch_q18(" + std::to_string(quantity_threshold) + ")";
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

void SetupSyntheticFixture(duckdb::Connection &connection) {
	connection.Query(
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
	connection.Query("INSERT INTO customer VALUES (1, 'cust_a', 'a', 1, 'p', 0, 's', 'c')");
	connection.Query("INSERT INTO orders VALUES (100, 1, 'O', 500.00, DATE '1993-01-01', '1', 'c', 0, 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (100, 1, 1, 1, 200.00, 100, 0.1, 0, 'N', 'O', DATE '1993-01-01', DATE "
	    "'1993-01-01', DATE '1993-01-01', 'N', 'A', 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (100, 1, 1, 2, 150.00, 50, 0.1, 0, 'N', 'O', DATE '1993-01-01', DATE "
	    "'1993-01-01', DATE '1993-01-01', 'N', 'A', 'c')");
}

} // namespace

int main() {
	try {
		{
			for (const auto &entry : accelerator_codegen::test_support::kQ38Manifest) {
				duckdb::DuckDB file_database(GetTpchDbPath().c_str());
				duckdb::Connection connection(file_database);
				accelerator_codegen::RegisterAcceleratorFunctions(connection);
				connection.Query("PRAGMA threads=1");
				RequireEqualResults(connection, entry.label, ReadFile(entry.sql_path),
				                    AcceleratorQ18Sql(entry.quantity_threshold));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity) "
			    "FROM customer, orders, lineitem "
			    "WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > 300) "
			    "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
			    "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
			    "ORDER BY o_totalprice DESC, o_orderdate";
			RequireEqualResults(connection, "synthetic_large_order", reference, AcceleratorQ18Sql(300));
		}

		std::cout << "test_tpch_q38 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q38 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
