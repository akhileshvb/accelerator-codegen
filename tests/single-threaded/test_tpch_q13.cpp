#include "accelerator_codegen/register.hpp"
#include "test_support/q13_manifest.hpp"

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

std::string AcceleratorQ13Sql(const accelerator_codegen::test_support::Q13ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q33('") + entry.comment_pattern + "')";
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
	const auto expected_rows = MaterializeOrderedRows(expected_m);
	const auto actual_rows = MaterializeOrderedRows(actual_m);
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
	    "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL);");
	connection.Query("INSERT INTO customer VALUES (1, 'c1', 'a', 1, 'p', 0, 's', 'c')");
	connection.Query("INSERT INTO customer VALUES (2, 'c2', 'a', 1, 'p', 0, 's', 'c')");
	connection.Query("INSERT INTO customer VALUES (3, 'c3', 'a', 1, 'p', 0, 's', 'c')");
	connection.Query("INSERT INTO orders VALUES (10, 1, 'O', 1, DATE '1993-01-01', '1', 'c', 0, 'usual account')");
	connection.Query("INSERT INTO orders VALUES (11, 1, 'O', 1, DATE '1993-01-01', '1', 'c', 0, 'normal order')");
	connection.Query("INSERT INTO orders VALUES (12, 2, 'O', 1, DATE '1993-01-01', '1', 'c', 0, 'normal order')");
	connection.Query("INSERT INTO orders VALUES (13, 3, 'O', 1, DATE '1993-01-01', '1', 'c', 0, 'usual account')");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (const auto &entry : accelerator_codegen::test_support::kQ33Manifest) {
				RequireEqualResults(connection, entry.label, ReadFile(entry.sql_path), AcceleratorQ13Sql(entry));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "SELECT c_count, COUNT(*) AS custdist FROM ("
			    "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
			    "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE '%usual%account%' "
			    "GROUP BY c_custkey) AS c_orders(c_custkey, c_count) "
			    "GROUP BY c_count ORDER BY custdist DESC, c_count DESC";
			RequireEqualResults(connection, "synthetic_left_join_counts", reference,
			                    "SELECT * FROM accel_tpch_q33('%usual%account%')");
		}

		std::cout << "test_tpch_q33 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q33 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
