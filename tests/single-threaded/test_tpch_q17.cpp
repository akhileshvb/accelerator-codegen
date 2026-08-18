#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "test_support/q17_manifest.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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

std::string ReferenceQ17Sql(const std::string &brand, const std::string &container) {
	return "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
	       "FROM lineitem, part, "
	       "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
	       "FROM lineitem GROUP BY l_partkey) part_agg "
	       "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
	       "AND p_brand = '" +
	       brand + "' AND p_container = '" + container + "' AND l_quantity < avg_quantity";
}

std::string AcceleratorQ17Sql(const std::string &brand, const std::string &container) {
	return "SELECT * FROM accel_tpch_q17('" + brand + "', '" + container + "')";
}

bool DoublesMatch(double expected, double actual) {
	if (std::isnan(expected) && std::isnan(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	if (std::isinf(expected) && std::isinf(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	uint64_t expected_bits = 0;
	uint64_t actual_bits = 0;
	std::memcpy(&expected_bits, &expected, sizeof(double));
	std::memcpy(&actual_bits, &actual, sizeof(double));
	return expected_bits == actual_bits;
}

void RequireEqualQ17Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                            const std::string &accelerator_sql) {
	auto expected = RunQuery(connection, reference_sql);
	auto actual = RunQuery(connection, accelerator_sql);
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();

	Require(expected->names == actual->names, label + ": column names differ");
	Require(expected->types == actual->types, label + ": column types differ");
	Require(expected_m.RowCount() == 1 && actual_m.RowCount() == 1, label + ": expected single-row results");
	Require(expected_m.ColumnCount() == 1 && actual_m.ColumnCount() == 1, label + ": expected single-column results");

	const auto expected_value = expected_m.GetValue(0, 0);
	const auto actual_value = actual_m.GetValue(0, 0);
	Require(expected_value.IsNull() == actual_value.IsNull(), label + ": nullability differs");
	if (expected_value.IsNull()) {
		return;
	}
	if (!DoublesMatch(expected_value.GetValue<double>(), actual_value.GetValue<double>())) {
		throw std::runtime_error(label + ": avg_yearly differs, expected=" + expected_value.ToString() +
		                         ", actual=" + actual_value.ToString());
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

void RequireExpectedQ17Schema(duckdb::Connection &connection) {
	auto result = RunQuery(connection, AcceleratorQ17Sql("Brand#11", "SM CASE"));
	const duckdb::vector<duckdb::string> expected_names = {"avg_yearly"};
	const duckdb::vector<duckdb::LogicalType> expected_types = {duckdb::LogicalType::DOUBLE};
	Require(result->names == expected_names, "q17 schema names differ");
	Require(result->types == expected_types, "q17 schema types differ");
}

void CreateQ37Schema(duckdb::Connection &connection, const std::string &partkey_type = "BIGINT") {
	auto result = connection.Query(
	    "CREATE TABLE part("
	    "p_partkey " +
	    partkey_type +
	    " NOT NULL, "
	    "p_name VARCHAR NOT NULL, "
	    "p_mfgr VARCHAR NOT NULL, "
	    "p_brand VARCHAR NOT NULL, "
	    "p_type VARCHAR NOT NULL, "
	    "p_size INTEGER NOT NULL, "
	    "p_container VARCHAR NOT NULL, "
	    "p_retailprice DECIMAL(15,2) NOT NULL, "
	    "p_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem("
	    "l_orderkey BIGINT NOT NULL, "
	    "l_partkey BIGINT NOT NULL, "
	    "l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, "
	    "l_quantity DECIMAL(15,2) NOT NULL, "
	    "l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, "
	    "l_tax DECIMAL(15,2) NOT NULL, "
	    "l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, "
	    "l_shipdate DATE NOT NULL, "
	    "l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, "
	    "l_shipinstruct VARCHAR NOT NULL, "
	    "l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), "failed to create q17 schema: " + result->GetError());
}

void InsertPart(duckdb::Connection &connection, int64_t partkey, const std::string &brand,
                const std::string &container) {
	auto result = connection.Query("INSERT INTO part VALUES (" + std::to_string(partkey) +
	                               ", 'name', 'Manufacturer#1', '" + brand + "', 'TYPE', 1, '" + container +
	                               "', 10.00, 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert part: " + result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t partkey, int64_t linenumber,
                    const std::string &quantity, const std::string &extendedprice) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", " + std::to_string(partkey) + ", 1, " +
	    std::to_string(linenumber) + ", " + quantity + ", " + extendedprice +
	    ", 0.00, 0.00, 'N', 'O', DATE '1995-01-01', DATE '1995-01-01', DATE '1995-01-01', "
	    "'NONE', 'AIR', 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

void CompareParams(duckdb::Connection &connection, const std::string &label, const std::string &brand,
                   const std::string &container) {
	RequireEqualQ17Results(connection, label, ReferenceQ17Sql(brand, container), AcceleratorQ17Sql(brand, container));
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ37Schema(connection);
	InsertPart(connection, 1, "Brand#11", "SM CASE");
	InsertPart(connection, 2, "Brand#11", "LG BOX");
	InsertPart(connection, 3, "Brand#22", "SM CASE");

	// Part 1 avg quantity = (10+30+50)/3 = 30, threshold = 6.0
	InsertLineitem(connection, 1, 1, 1, "10.00", "100.00"); // below threshold
	InsertLineitem(connection, 2, 1, 2, "30.00", "200.00"); // above threshold
	InsertLineitem(connection, 3, 1, 3, "50.00", "300.00"); // above threshold

	// Part 2 wrong container
	InsertLineitem(connection, 4, 2, 1, "1.00", "999.00");

	// Part 3 wrong brand
	InsertLineitem(connection, 5, 3, 1, "1.00", "888.00");
}

void SetupEmptyFixture(duckdb::Connection &connection) {
	CreateQ37Schema(connection);
	InsertPart(connection, 1, "Brand#99", "NONE");
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ37Schema(connection);
	InsertPart(connection, 10, "Brand#11", "SM CASE");
	InsertLineitem(connection, 10, 10, 1, "5.00", "70.00");
	InsertLineitem(connection, 11, 10, 2, "15.00", "30.00");
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

			RequireExpectedQ17Schema(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ37Manifest) {
				RequireEqualQ17Results(connection, std::string(entry.label) + "_seed_file", ReadFile(entry.sql_path),
				                       AcceleratorQ17Sql(entry.brand, entry.container));
				RequireEqualQ17Results(connection, std::string(entry.label) + "_inline_sql",
				                       ReferenceQ17Sql(entry.brand, entry.container),
				                       AcceleratorQ17Sql(entry.brand, entry.container));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			CompareParams(connection, "synthetic_mixed", "Brand#11", "SM CASE");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupEmptyFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			CompareParams(connection, "empty_result_null", "Brand#11", "SM CASE");
			auto result = RunQuery(connection, AcceleratorQ17Sql("Brand#11", "SM CASE"));
			auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
			Require(materialized.GetValue(0, 0).IsNull(), "empty: expected NULL avg_yearly");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			CompareParams(connection, "transaction_baseline", "Brand#11", "SM CASE");
			connection.BeginTransaction();

			InsertPart(connection, 20, "Brand#11", "SM CASE");
			InsertLineitem(connection, 20, 20, 1, "2.00", "50.00");
			CompareParams(connection, "after_insert_qualifying", "Brand#11", "SM CASE");

			auto brand_update = connection.Query("UPDATE part SET p_brand = 'Brand#99' WHERE p_partkey = 10");
			Require(brand_update != nullptr && !brand_update->HasError(), brand_update->GetError());
			CompareParams(connection, "after_part_leave_brand", "Brand#11", "SM CASE");

			auto container_update = connection.Query("UPDATE part SET p_container = 'LG BOX' WHERE p_partkey = 20");
			Require(container_update != nullptr && !container_update->HasError(), container_update->GetError());
			CompareParams(connection, "after_part_leave_container", "Brand#11", "SM CASE");

			auto qty_update = connection.Query("UPDATE lineitem SET l_quantity = 1.00 WHERE l_orderkey = 10");
			Require(qty_update != nullptr && !qty_update->HasError(), qty_update->GetError());
			CompareParams(connection, "after_quantity_cross_threshold", "Brand#11", "SM CASE");

			auto price_update = connection.Query("UPDATE lineitem SET l_extendedprice = 99.00 WHERE l_orderkey = 10");
			Require(price_update != nullptr && !price_update->HasError(), price_update->GetError());
			CompareParams(connection, "after_price_update", "Brand#11", "SM CASE");

			auto delete_line = connection.Query("DELETE FROM lineitem WHERE l_orderkey = 11");
			Require(delete_line != nullptr && !delete_line->HasError(), delete_line->GetError());
			CompareParams(connection, "after_delete_lineitem", "Brand#11", "SM CASE");

			connection.Rollback();
			CompareParams(connection, "after_rollback", "Brand#11", "SM CASE");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ37Schema(connection, "INTEGER");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ17Sql("Brand#11", "SM CASE"),
			                          "Unsupported schema");
		}

		std::cout << "test_tpch_q37 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q37 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
