#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q2/tests/q2_manifest.hpp"

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

std::string AcceleratorQ2Sql(const accelerator_codegen::test_support::Q2ManifestEntry &entry) {
	return "SELECT * FROM accel_tpch_q2(" + std::to_string(entry.part_size) + ", '" +
	       std::string(entry.part_type_pattern) + "', '" + std::string(entry.region_name) + "')";
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

} // namespace

int main() {
	try {
		// Read-only: the smoke test never writes the shared database.
		duckdb::DBConfig file_database_config;
		file_database_config.options.access_mode = duckdb::AccessMode::READ_ONLY;
		duckdb::DuckDB file_database(GetTpchDbPath(), &file_database_config);
		duckdb::Connection connection(file_database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=1");
		for (const auto &entry : accelerator_codegen::test_support::kQ2Manifest) {
			RequireEqualResults(connection, entry.label, ReadFile(entry.sql_path), AcceleratorQ2Sql(entry));
		}

		std::cout << "test_tpch_q2 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q2 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
