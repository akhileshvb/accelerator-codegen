#include "backends/duckdb/api/register.hpp"
#include "tpch/q22/tests/q22_manifest.hpp"

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

std::string AcceleratorQ22Sql(const accelerator_codegen::test_support::Q22ManifestEntry &entry) {
	std::ostringstream stream;
	stream << "SELECT * FROM accel_tpch_q22(";
	for (duckdb::idx_t index = 0; index < entry.country_codes.size(); index++) {
		if (index != 0) {
			stream << ", ";
		}
		stream << "'" << entry.country_codes[index] << "'";
	}
	stream << ")";
	return stream.str();
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
	connection.Query("INSERT INTO customer VALUES (1, 'c1', 'a', 1, '13-111-111-1111', 1000.00, 's', 'c')");
	connection.Query("INSERT INTO customer VALUES (2, 'c2', 'a', 1, '13-222-222-2222', 500.00, 's', 'c')");
	connection.Query("INSERT INTO customer VALUES (3, 'c3', 'a', 1, '30-333-333-3333', 2000.00, 's', 'c')");
	connection.Query("INSERT INTO orders VALUES (10, 1, 'O', 1, DATE '1993-01-01', '1', 'c', 0, 'c')");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (const auto &entry : accelerator_codegen::test_support::kQ22Manifest) {
				RequireEqualResults(connection, entry.label, ReadFile(entry.sql_path), AcceleratorQ22Sql(entry));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "WITH custsale AS ("
			    "SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
			    "FROM customer "
			    "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ('13', '30') "
			    "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
			    "AND SUBSTRING(c_phone FROM 1 FOR 2) IN ('13', '30')) "
			    "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
			    "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
			    "FROM custsale GROUP BY cntrycode ORDER BY cntrycode";
			RequireEqualResults(connection, "synthetic_country_balance", reference,
			                    "SELECT * FROM accel_tpch_q22('13', '30', '99', '98', '97', '96', '95')");
		}

		std::cout << "test_tpch_q22 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q22 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
