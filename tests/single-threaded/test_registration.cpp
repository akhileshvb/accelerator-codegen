#include "accelerator_codegen/register.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		auto result = connection.Query(
		    "SELECT DISTINCT function_name FROM duckdb_functions() "
		    "WHERE function_name IN ("
		    "'accel_tpch_q3','accel_tpch_q3','accel_tpch_q4','accel_tpch_q6',"
		    "'accel_tpch_q9','accel_tpch_q32','accel_tpch_q14','accel_tpch_q37','accel_tpch_q39','accel_tpch_q5','accel_tpch_q7',"
		    "'accel_tpch_q30','accel_tpch_q31','accel_tpch_q33','accel_tpch_q36','accel_tpch_q38','accel_tpch_q22',"
		    "'accel_tpch_q35','accel_tpch_q21','accel_tpch_q20','accel_tpch_q8','accel_tpch_q2'"
		    ") ORDER BY 1");
		Require(result != nullptr, "registration query returned null result");
		Require(!result->HasError(), result->GetError());
		auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
		Require(materialized.RowCount() == 22, "expected all twenty-two accel_tpch_* functions registered");

		std::cout << "registration_smoke_test passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "registration_smoke_test failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
