#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "test_support/q4_manifest.hpp"

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

std::unique_ptr<duckdb::QueryResult> RunQuery(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	return result;
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

std::string ReferenceQ4Sql(const std::string &start_date) {
	return "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
	       "FROM orders "
	       "WHERE o_orderdate >= DATE '" +
	       start_date + "' AND o_orderdate < DATE '" + start_date +
	       "' + INTERVAL '3' month AND EXISTS ("
	       "SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate) "
	       "GROUP BY o_orderpriority ORDER BY o_orderpriority";
}

std::string AcceleratorQ4Sql(const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q4(DATE '" + start_date + "')";
}

void RequireEqualQ4Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                           const std::string &accelerator_sql) {
	auto expected = RunQuery(connection, reference_sql);
	auto actual = RunQuery(connection, accelerator_sql);

	auto &expected_materialized = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_materialized = actual->Cast<duckdb::MaterializedQueryResult>();

	Require(expected->names == actual->names, label + ": column names differ");
	Require(expected->types == actual->types, label + ": column types differ");
	Require(expected_materialized.RowCount() == actual_materialized.RowCount(), label + ": row counts differ");
	Require(expected_materialized.ColumnCount() == actual_materialized.ColumnCount(),
	        label + ": column counts differ");

	const auto expected_rows = MaterializeOrderedRows(expected_materialized);
	const auto actual_rows = MaterializeOrderedRows(actual_materialized);
	if (expected_rows != actual_rows) {
		duckdb::idx_t mismatch_index = 0;
		while (mismatch_index < expected_rows.size() && mismatch_index < actual_rows.size() &&
		       expected_rows[mismatch_index] == actual_rows[mismatch_index]) {
			mismatch_index++;
		}
		throw std::runtime_error(
		    label + ": ordered rows differ at index " + std::to_string(mismatch_index) + ", expected=" +
		    (mismatch_index < expected_rows.size() ? expected_rows[mismatch_index] : std::string("<missing>")) +
		    ", actual=" +
		    (mismatch_index < actual_rows.size() ? actual_rows[mismatch_index] : std::string("<missing>")));
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

void RequireExpectedQ4Schema(duckdb::Connection &connection) {
	auto result = RunQuery(connection, AcceleratorQ4Sql("1995-01-01"));
	const duckdb::vector<duckdb::string> expected_names = {"o_orderpriority", "order_count"};
	const duckdb::vector<duckdb::LogicalType> expected_types = {duckdb::LogicalType::VARCHAR,
	                                                            duckdb::LogicalType::BIGINT};
	Require(result->names == expected_names, "q4 schema names differ");
	Require(result->types == expected_types, "q4 schema types differ");
}

void CreateQ4Schema(duckdb::Connection &connection, const std::string &orderpriority_type = "VARCHAR") {
	auto result = connection.Query(
	    "CREATE TABLE orders("
	    "o_orderkey BIGINT NOT NULL, "
	    "o_custkey BIGINT NOT NULL, "
	    "o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, "
	    "o_orderdate DATE NOT NULL, "
	    "o_orderpriority " +
	    orderpriority_type +
	    " NOT NULL, "
	    "o_clerk VARCHAR NOT NULL, "
	    "o_shippriority INTEGER NOT NULL, "
	    "o_comment VARCHAR NOT NULL);"
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
	Require(result != nullptr, "failed to create q4 schema");
	Require(!result->HasError(), result->GetError());
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, const std::string &orderdate,
                 const std::string &priority) {
	auto result = connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) +
	                               ", 1, 'O', 100.00, DATE '" + orderdate + "', '" + priority +
	                               "', 'Clerk#000000001', 0, 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert order: " + result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t linenumber,
                    const std::string &commitdate, const std::string &receiptdate) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, 1, " + std::to_string(linenumber) +
	    ", 1.00, 10.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-01', DATE '" + commitdate + "', DATE '" + receiptdate +
	    "', 'NONE', 'AIR', 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ4Schema(connection);

	// Window: [1995-01-01, 1995-04-01)
	InsertOrder(connection, 1, "1995-01-01", "1-URGENT");   // lower bound inclusive
	InsertOrder(connection, 2, "1995-03-31", "2-HIGH");     // last day in window
	InsertOrder(connection, 3, "1995-04-01", "3-MEDIUM");   // upper bound exclusive
	InsertOrder(connection, 4, "1994-12-31", "4-NOT SPECIFIED"); // before window
	InsertOrder(connection, 5, "1995-02-15", "1-URGENT");   // multi qualifying lineitems
	InsertOrder(connection, 6, "1995-02-15", "5-LOW");      // mixed qualifying/non-qualifying lineitems
	InsertOrder(connection, 7, "1995-02-15", "2-HIGH");     // no qualifying lineitems

	InsertLineitem(connection, 1, 1, "1995-01-10", "1995-01-20"); // qualifying
	InsertLineitem(connection, 2, 1, "1995-02-01", "1995-02-10"); // qualifying
	InsertLineitem(connection, 3, 1, "1995-02-01", "1995-02-10"); // would qualify but order out of window
	InsertLineitem(connection, 4, 1, "1995-02-01", "1995-02-10"); // order out of window
	InsertLineitem(connection, 5, 1, "1995-02-01", "1995-02-10"); // qualifying
	InsertLineitem(connection, 5, 2, "1995-02-02", "1995-02-11"); // second qualifying (EXISTS once)
	InsertLineitem(connection, 6, 1, "1995-02-10", "1995-02-01"); // non-qualifying (commit >= receipt)
	InsertLineitem(connection, 6, 2, "1995-02-01", "1995-02-10"); // qualifying
	InsertLineitem(connection, 7, 1, "1995-02-10", "1995-02-01"); // non-qualifying only
}

void SetupEmptyFixture(duckdb::Connection &connection) {
	CreateQ4Schema(connection);
	InsertOrder(connection, 1, "1995-02-01", "1-URGENT");
	InsertLineitem(connection, 1, 1, "1995-02-10", "1995-02-01"); // non-qualifying
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ4Schema(connection);
	InsertOrder(connection, 10, "1995-02-01", "1-URGENT");
	InsertLineitem(connection, 10, 1, "1995-02-01", "1995-02-10");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireExpectedQ4Schema(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ4Manifest) {
				RequireEqualQ4Results(connection, entry.label, ReadFile(entry.sql_path),
				                      AcceleratorQ4Sql(entry.start_date));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireEqualQ4Results(connection, "synthetic_window_and_exists", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			// Explicit expected shape: orders 1,2,5,6 qualify once each → priorities 1-URGENT:2, 2-HIGH:1, 5-LOW:1
			auto result = RunQuery(connection, AcceleratorQ4Sql("1995-01-01"));
			auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
			Require(materialized.RowCount() == 3, "synthetic: expected 3 priority groups");
			Require(materialized.GetValue(0, 0).ToString() == "1-URGENT", "synthetic: first priority");
			Require(materialized.GetValue(1, 0).GetValue<int64_t>() == 2, "synthetic: 1-URGENT count");
			Require(materialized.GetValue(0, 1).ToString() == "2-HIGH", "synthetic: second priority");
			Require(materialized.GetValue(1, 1).GetValue<int64_t>() == 1, "synthetic: 2-HIGH count");
			Require(materialized.GetValue(0, 2).ToString() == "5-LOW", "synthetic: third priority");
			Require(materialized.GetValue(1, 2).GetValue<int64_t>() == 1, "synthetic: 5-LOW count");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupEmptyFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireEqualQ4Results(connection, "empty_result", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireEqualQ4Results(connection, "transaction_baseline", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			connection.BeginTransaction();

			InsertOrder(connection, 20, "1995-02-10", "2-HIGH");
			InsertLineitem(connection, 20, 1, "1995-02-01", "1995-02-10");
			RequireEqualQ4Results(connection, "after_insert_order_and_qualifying_lineitem", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			InsertLineitem(connection, 20, 2, "1995-02-02", "1995-02-11");
			RequireEqualQ4Results(connection, "after_duplicate_qualifying_lineitems", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			auto flip_true =
			    connection.Query("UPDATE lineitem SET l_commitdate = DATE '1995-03-01', l_receiptdate = DATE '1995-02-01' "
			                     "WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(flip_true != nullptr && !flip_true->HasError(), "failed to flip commit<receipt false");
			RequireEqualQ4Results(connection, "after_lineitem_predicate_false", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			auto flip_false =
			    connection.Query("UPDATE lineitem SET l_commitdate = DATE '1995-02-01', l_receiptdate = DATE '1995-03-01' "
			                     "WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(flip_false != nullptr && !flip_false->HasError(), "failed to flip commit<receipt true");
			RequireEqualQ4Results(connection, "after_lineitem_predicate_true", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			auto into_window =
			    connection.Query("UPDATE orders SET o_orderdate = DATE '1995-02-15' WHERE o_orderkey = 20");
			Require(into_window != nullptr && !into_window->HasError(), "failed into-window update");
			RequireEqualQ4Results(connection, "after_order_into_window", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			auto out_of_window =
			    connection.Query("UPDATE orders SET o_orderdate = DATE '1995-05-01' WHERE o_orderkey = 20");
			Require(out_of_window != nullptr && !out_of_window->HasError(), "failed out-of-window update");
			RequireEqualQ4Results(connection, "after_order_out_of_window", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			auto back_in =
			    connection.Query("UPDATE orders SET o_orderdate = DATE '1995-02-10' WHERE o_orderkey = 20");
			Require(back_in != nullptr && !back_in->HasError(), "failed restore order into window");

			auto priority_update =
			    connection.Query("UPDATE orders SET o_orderpriority = '3-MEDIUM' WHERE o_orderkey = 20");
			Require(priority_update != nullptr && !priority_update->HasError(), "failed priority update");
			RequireEqualQ4Results(connection, "after_priority_update", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			auto delete_lineitem =
			    connection.Query("DELETE FROM lineitem WHERE l_orderkey = 20 AND l_linenumber = 1");
			Require(delete_lineitem != nullptr && !delete_lineitem->HasError(), "failed delete lineitem");
			RequireEqualQ4Results(connection, "after_delete_one_qualifying_lineitem", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			auto delete_remaining =
			    connection.Query("DELETE FROM lineitem WHERE l_orderkey = 20");
			Require(delete_remaining != nullptr && !delete_remaining->HasError(), "failed delete remaining lineitems");
			RequireEqualQ4Results(connection, "after_delete_all_qualifying_lineitems", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			InsertLineitem(connection, 20, 1, "1995-02-01", "1995-02-10");
			auto delete_order = connection.Query("DELETE FROM orders WHERE o_orderkey = 20");
			Require(delete_order != nullptr && !delete_order->HasError(), "failed delete order");
			RequireEqualQ4Results(connection, "after_delete_order", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			connection.Rollback();
			RequireEqualQ4Results(connection, "after_rollback", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ4Schema(connection, "INTEGER");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ4Sql("1995-01-01"),
			                          "Unsupported schema");
		}

		{
			setenv("ACCEL_Q4_LINEITEM_FUSED", "1", 1);
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ4Manifest) {
				RequireEqualQ4Results(connection, std::string(entry.label) + "_fused_lineitem", ReadFile(entry.sql_path),
				                      AcceleratorQ4Sql(entry.start_date));
			}

			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection memory_connection(memory_database);
			SetupSyntheticFixture(memory_connection);
			accelerator_codegen::RegisterAcceleratorFunctions(memory_connection);
			RequireEqualQ4Results(memory_connection, "synthetic_fused_lineitem", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));

			duckdb::DuckDB txn_database(nullptr);
			duckdb::Connection txn_connection(txn_database);
			SetupTransactionFixture(txn_connection);
			accelerator_codegen::RegisterAcceleratorFunctions(txn_connection);
			RequireEqualQ4Results(txn_connection, "fused_transaction_baseline", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));
			txn_connection.BeginTransaction();
			InsertOrder(txn_connection, 20, "1995-02-10", "2-HIGH");
			InsertLineitem(txn_connection, 20, 1, "1995-02-01", "1995-02-10");
			RequireEqualQ4Results(txn_connection, "fused_after_insert", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));
			txn_connection.Rollback();
			RequireEqualQ4Results(txn_connection, "fused_after_rollback", ReferenceQ4Sql("1995-01-01"),
			                      AcceleratorQ4Sql("1995-01-01"));
			unsetenv("ACCEL_Q4_LINEITEM_FUSED");
		}

		std::cout << "test_tpch_q4 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q4 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
