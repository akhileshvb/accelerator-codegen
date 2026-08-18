#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "test_support/q14_manifest.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
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

std::string ReferenceQ14Sql(const std::string &start_date) {
	return "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) / "
	       "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
	       "FROM lineitem, part "
	       "WHERE l_partkey = p_partkey "
	       "AND l_shipdate >= DATE '" +
	       start_date + "' AND l_shipdate < DATE '" + start_date + "' + INTERVAL '1' month";
}

std::string AcceleratorQ14Sql(const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q14(DATE '" + start_date + "')";
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
	static_assert(sizeof(double) == sizeof(uint64_t), "unexpected double size");
	std::memcpy(&expected_bits, &expected, sizeof(double));
	std::memcpy(&actual_bits, &actual, sizeof(double));
	return expected_bits == actual_bits;
}

void RequireEqualQ14Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
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
	const auto expected_d = expected_value.GetValue<double>();
	const auto actual_d = actual_value.GetValue<double>();
	if (!DoublesMatch(expected_d, actual_d)) {
		throw std::runtime_error(label + ": promo_revenue differs, expected=" + expected_value.ToString() +
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

void RequireExpectedQ14Schema(duckdb::Connection &connection) {
	auto result = RunQuery(connection, AcceleratorQ14Sql("1995-01-01"));
	const duckdb::vector<duckdb::string> expected_names = {"promo_revenue"};
	const duckdb::vector<duckdb::LogicalType> expected_types = {duckdb::LogicalType::DOUBLE};
	Require(result->names == expected_names, "q14 schema names differ");
	Require(result->types == expected_types, "q14 schema types differ");
}

void CreateQ14Schema(duckdb::Connection &connection, bool nullable_part_type = false,
                     const std::string &partkey_type = "BIGINT") {
	const std::string type_null = nullable_part_type ? "" : " NOT NULL";
	auto result = connection.Query(
	    "CREATE TABLE part("
	    "p_partkey " +
	    partkey_type +
	    " NOT NULL, "
	    "p_name VARCHAR NOT NULL, "
	    "p_mfgr VARCHAR NOT NULL, "
	    "p_brand VARCHAR NOT NULL, "
	    "p_type VARCHAR" +
	    type_null +
	    ", "
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
	Require(result != nullptr && !result->HasError(), "failed to create q14 schema: " + result->GetError());
}

void InsertPart(duckdb::Connection &connection, int64_t partkey, const std::string &type) {
	std::string type_sql = type == "NULL" ? "NULL" : ("'" + type + "'");
	auto result = connection.Query("INSERT INTO part VALUES (" + std::to_string(partkey) +
	                               ", 'name', 'Manufacturer#1', 'Brand#11', " + type_sql +
	                               ", 1, 'SM CASE', 10.00, 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert part: " + result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t partkey, int64_t linenumber,
                    const std::string &extendedprice, const std::string &discount, const std::string &shipdate) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", " + std::to_string(partkey) + ", 1, " +
	    std::to_string(linenumber) + ", 1.00, " + extendedprice + ", " + discount +
	    ", 0.00, 'N', 'O', DATE '" + shipdate + "', DATE '" + shipdate + "', DATE '" + shipdate +
	    "', 'NONE', 'AIR', 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

void CompareParams(duckdb::Connection &connection, const std::string &label, const std::string &start_date) {
	RequireEqualQ14Results(connection, label, ReferenceQ14Sql(start_date), AcceleratorQ14Sql(start_date));
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ14Schema(connection, true);

	InsertPart(connection, 1, "PROMO BURNISHED COPPER");   // promo prefix
	InsertPart(connection, 2, "LARGE BRUSHED BRASS");       // non-promo
	InsertPart(connection, 3, "promo brushed copper");      // lowercase — not PROMO%
	InsertPart(connection, 4, "XPROMO STEEL");              // contains PROMO but not prefix
	InsertPart(connection, 5, "PROMO");                     // exact prefix match
	InsertPart(connection, 6, "NULL");                      // null type → not promo
	InsertPart(connection, 7, "PROMO ANODIZED STEEL");      // multi-lineitem promo

	// Window [1995-01-01, 1995-02-01)
	InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1995-01-01"); // lower bound inclusive, promo
	InsertLineitem(connection, 2, 2, 1, "100.00", "0.00", "1995-01-15"); // non-promo
	InsertLineitem(connection, 3, 3, 1, "100.00", "0.00", "1995-01-15"); // lowercase type
	InsertLineitem(connection, 4, 4, 1, "100.00", "0.00", "1995-01-15"); // non-prefix PROMO
	InsertLineitem(connection, 5, 5, 1, "50.00", "0.10", "1995-01-31");  // last day in window, promo
	InsertLineitem(connection, 6, 1, 2, "100.00", "0.00", "1995-02-01"); // upper bound exclusive
	InsertLineitem(connection, 7, 1, 3, "100.00", "0.00", "1994-12-31"); // just outside lower
	InsertLineitem(connection, 8, 6, 1, "100.00", "0.00", "1995-01-10"); // null type
	InsertLineitem(connection, 9, 7, 1, "200.00", "0.05", "1995-01-20"); // promo multi
	InsertLineitem(connection, 9, 7, 2, "300.00", "0.00", "1995-01-21"); // promo multi
	InsertLineitem(connection, 10, 999, 1, "100.00", "0.00", "1995-01-15"); // missing part
}

void SetupEmptyFixture(duckdb::Connection &connection) {
	CreateQ14Schema(connection);
	InsertPart(connection, 1, "PROMO STEEL");
	InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1994-01-01"); // outside window
}

void SetupZeroRevenueFixture(duckdb::Connection &connection) {
	CreateQ14Schema(connection);
	InsertPart(connection, 1, "PROMO STEEL");
	InsertPart(connection, 2, "LARGE STEEL");
	InsertLineitem(connection, 1, 1, 1, "0.00", "0.00", "1995-01-15");
	InsertLineitem(connection, 2, 2, 1, "0.00", "0.00", "1995-01-15");
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ14Schema(connection);
	InsertPart(connection, 10, "PROMO STEEL");
	InsertLineitem(connection, 10, 10, 1, "100.00", "0.00", "1995-01-15");
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

			RequireExpectedQ14Schema(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ14Manifest) {
				RequireEqualQ14Results(connection, std::string(entry.label) + "_seed_file", ReadFile(entry.sql_path),
				                       AcceleratorQ14Sql(entry.start_date));
				RequireEqualQ14Results(connection, std::string(entry.label) + "_inline_sql",
				                       ReferenceQ14Sql(entry.start_date), AcceleratorQ14Sql(entry.start_date));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			CompareParams(connection, "synthetic_mixed", "1995-01-01");
			CompareParams(connection, "synthetic_empty_window", "1990-01-01");
			CompareParams(connection, "synthetic_promo_only_window_shift", "1995-01-01");

			auto only_promo = connection.Query(
			    "SELECT * FROM accel_tpch_q14(DATE '1995-01-01')");
			Require(only_promo != nullptr && !only_promo->HasError(), only_promo->GetError());
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupEmptyFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			CompareParams(connection, "empty_result_null", "1995-01-01");
			auto result = RunQuery(connection, AcceleratorQ14Sql("1995-01-01"));
			auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
			Require(materialized.GetValue(0, 0).IsNull(), "empty: expected NULL promo_revenue");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupZeroRevenueFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			CompareParams(connection, "zero_discounted_revenue", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			CompareParams(connection, "transaction_baseline", "1995-01-01");
			connection.BeginTransaction();

			InsertPart(connection, 20, "PROMO COPPER");
			InsertLineitem(connection, 20, 20, 1, "200.00", "0.00", "1995-01-15");
			CompareParams(connection, "after_insert_promo_part", "1995-01-01");

			InsertPart(connection, 21, "LARGE BRASS");
			InsertLineitem(connection, 21, 21, 1, "50.00", "0.00", "1995-01-15");
			CompareParams(connection, "after_insert_non_promo_part", "1995-01-01");

			auto leave_promo =
			    connection.Query("UPDATE part SET p_type = 'LARGE STEEL' WHERE p_partkey = 20");
			Require(leave_promo != nullptr && !leave_promo->HasError(), leave_promo->GetError());
			CompareParams(connection, "after_part_leave_promo", "1995-01-01");

			auto enter_promo =
			    connection.Query("UPDATE part SET p_type = 'PROMO STEEL' WHERE p_partkey = 21");
			Require(enter_promo != nullptr && !enter_promo->HasError(), enter_promo->GetError());
			CompareParams(connection, "after_part_enter_promo", "1995-01-01");

			auto leave_window =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-03-01' WHERE l_orderkey = 10");
			Require(leave_window != nullptr && !leave_window->HasError(), leave_window->GetError());
			CompareParams(connection, "after_lineitem_leave_window", "1995-01-01");

			auto enter_window =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-01-15' WHERE l_orderkey = 10");
			Require(enter_window != nullptr && !enter_window->HasError(), enter_window->GetError());
			CompareParams(connection, "after_lineitem_enter_window", "1995-01-01");

			auto lower_bound =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-01-01' WHERE l_orderkey = 10");
			Require(lower_bound != nullptr && !lower_bound->HasError(), lower_bound->GetError());
			CompareParams(connection, "after_lineitem_at_lower_bound", "1995-01-01");

			auto upper_bound =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-02-01' WHERE l_orderkey = 10");
			Require(upper_bound != nullptr && !upper_bound->HasError(), upper_bound->GetError());
			CompareParams(connection, "after_lineitem_at_upper_bound", "1995-01-01");

			auto restore_date =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-01-15' WHERE l_orderkey = 10");
			Require(restore_date != nullptr && !restore_date->HasError(), restore_date->GetError());

			auto rekey =
			    connection.Query("UPDATE lineitem SET l_partkey = 21 WHERE l_orderkey = 10");
			Require(rekey != nullptr && !rekey->HasError(), rekey->GetError());
			CompareParams(connection, "after_lineitem_rekey_to_promo_part", "1995-01-01");

			auto price_update =
			    connection.Query("UPDATE lineitem SET l_extendedprice = 250.00, l_discount = 0.10 WHERE l_orderkey = 20");
			Require(price_update != nullptr && !price_update->HasError(), price_update->GetError());
			CompareParams(connection, "after_price_discount_update", "1995-01-01");

			auto delete_line = connection.Query("DELETE FROM lineitem WHERE l_orderkey = 21");
			Require(delete_line != nullptr && !delete_line->HasError(), delete_line->GetError());
			CompareParams(connection, "after_delete_lineitem", "1995-01-01");

			auto delete_part = connection.Query("DELETE FROM part WHERE p_partkey = 20");
			Require(delete_part != nullptr && !delete_part->HasError(), delete_part->GetError());
			CompareParams(connection, "after_delete_part", "1995-01-01");

			connection.Rollback();
			CompareParams(connection, "after_rollback", "1995-01-01");

			connection.BeginTransaction();
			InsertPart(connection, 30, "PROMO NICKEL");
			InsertLineitem(connection, 30, 30, 1, "75.00", "0.00", "1995-01-15");
			connection.Commit();
			CompareParams(connection, "after_commit_visible", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ14Schema(connection, false, "INTEGER");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ14Sql("1995-01-01"),
			                          "Unsupported schema");
		}

		std::cout << "test_tpch_q14 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q14 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
