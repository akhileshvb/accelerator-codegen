#include "accelerator_codegen/register.hpp"
#include "test_support/q11_manifest.hpp"

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

std::string AcceleratorQ11Sql(const accelerator_codegen::test_support::Q11ManifestEntry &entry) {
	std::ostringstream stream;
	stream << "SELECT * FROM accel_tpch_q11('" << entry.nation_name << "', " << entry.fraction << ")";
	return stream.str();
}

std::string DuckdbQ11Sql(const accelerator_codegen::test_support::Q11ManifestEntry &entry) {
	std::ostringstream fraction;
	fraction.precision(17);
	fraction << entry.fraction;
	const auto nation = entry.nation_name;
	const auto from_clause = "FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND "
	                         "s_nationkey = n_nationkey AND n_name = '" + std::string(nation) + "'";
	return "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value " + from_clause +
	       " GROUP BY ps_partkey HAVING SUM(ps_supplycost * ps_availqty) > (SELECT "
	       "SUM(ps_supplycost * ps_availqty) * " + fraction.str() + " " + from_clause + ") ORDER BY value DESC";
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

void CreateQ11Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
	    "n_comment VARCHAR NOT NULL);"
	    "CREATE TABLE supplier(s_suppkey BIGINT NOT NULL, s_name VARCHAR NOT NULL, s_address VARCHAR NOT NULL, "
	    "s_nationkey INTEGER NOT NULL, s_phone VARCHAR NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL, "
	    "s_comment VARCHAR NOT NULL);"
	    "CREATE TABLE partsupp(ps_partkey BIGINT NOT NULL, ps_suppkey BIGINT NOT NULL, ps_availqty BIGINT NOT NULL, "
	    "ps_supplycost DECIMAL(15,2) NOT NULL, ps_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ11Schema(connection);
	connection.Query("INSERT INTO nation VALUES (10, 'ROMANIA', 1, 'c')");
	connection.Query("INSERT INTO supplier VALUES (200, 's', 'a', 10, 'p', 0, 'c')");
	connection.Query("INSERT INTO partsupp VALUES (1, 200, 10, 10.00, 'c')");
	connection.Query("INSERT INTO partsupp VALUES (2, 200, 10, 20.00, 'c')");
	connection.Query("INSERT INTO partsupp VALUES (3, 200, 10, 5.00, 'c')");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (const auto &entry : accelerator_codegen::test_support::kQ11Manifest) {
				RequireEqualResults(connection, entry.label, DuckdbQ11Sql(entry), AcceleratorQ11Sql(entry));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value "
			    "FROM partsupp, supplier, nation "
			    "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = 'ROMANIA' "
			    "GROUP BY ps_partkey "
			    "HAVING SUM(ps_supplycost * ps_availqty) > ("
			    "SELECT SUM(ps_supplycost * ps_availqty) * 0.5 "
			    "FROM partsupp, supplier, nation "
			    "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = 'ROMANIA') "
			    "ORDER BY value DESC";
			RequireEqualResults(connection, "synthetic_having_threshold", reference,
			                    "SELECT * FROM accel_tpch_q11('ROMANIA', 0.5)");
		}

		std::cout << "test_tpch_q11 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q11 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
