#include "backends/duckdb/api/register.hpp"
#include "tpch/q12/tests/q12_manifest.hpp"

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

std::string EscapeSqlLiteral(const std::string &value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (char c : value) {
		if (c == '\'') {
			escaped += "''";
		} else {
			escaped += c;
		}
	}
	return escaped;
}

std::string ReferenceQ12Sql(const std::string &shipmode1, const std::string &shipmode2, const std::string &start_date) {
	return "SELECT l_shipmode, "
	       "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) AS "
	       "high_line_count, "
	       "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) AS "
	       "low_line_count "
	       "FROM orders, lineitem "
	       "WHERE o_orderkey = l_orderkey "
	       "AND l_shipmode IN ('" +
	       EscapeSqlLiteral(shipmode1) + "', '" + EscapeSqlLiteral(shipmode2) +
	       "') "
	       "AND l_commitdate < l_receiptdate "
	       "AND l_shipdate < l_commitdate "
	       "AND l_receiptdate >= DATE '" +
	       start_date + "' AND l_receiptdate < DATE '" + start_date +
	       "' + INTERVAL '1' year "
	       "GROUP BY l_shipmode ORDER BY l_shipmode";
}

std::string AcceleratorQ12Sql(const std::string &shipmode1, const std::string &shipmode2,
                              const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q12('" + EscapeSqlLiteral(shipmode1) + "', '" + EscapeSqlLiteral(shipmode2) +
	       "', DATE '" + start_date + "')";
}

void RequireEqualQ12Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
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

void RequireExpectedQ12Schema(duckdb::Connection &connection) {
	auto result = RunQuery(connection, AcceleratorQ12Sql("MAIL", "SHIP", "1995-01-01"));
	const duckdb::vector<duckdb::string> expected_names = {"l_shipmode", "high_line_count", "low_line_count"};
	const duckdb::vector<duckdb::LogicalType> expected_types = {
	    duckdb::LogicalType::VARCHAR, duckdb::LogicalType::HUGEINT, duckdb::LogicalType::HUGEINT};
	Require(result->names == expected_names, "q12 schema names differ");
	Require(result->types == expected_types, "q12 schema types differ");
}

void CreateQ12Schema(duckdb::Connection &connection, const std::string &orderpriority_type = "VARCHAR",
                     bool nullable_priority = false, bool nullable_lineitem = false) {
	const std::string priority_null = nullable_priority ? "" : " NOT NULL";
	const std::string line_null = nullable_lineitem ? "" : " NOT NULL";
	auto result = connection.Query(
	    "CREATE TABLE orders("
	    "o_orderkey BIGINT NOT NULL, "
	    "o_custkey BIGINT NOT NULL, "
	    "o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, "
	    "o_orderdate DATE NOT NULL, "
	    "o_orderpriority " +
	    orderpriority_type + priority_null +
	    ", "
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
	    "l_shipdate DATE" +
	    line_null +
	    ", "
	    "l_commitdate DATE" +
	    line_null +
	    ", "
	    "l_receiptdate DATE" +
	    line_null +
	    ", "
	    "l_shipinstruct VARCHAR NOT NULL, "
	    "l_shipmode VARCHAR" +
	    line_null +
	    ", "
	    "l_comment VARCHAR NOT NULL);");
	Require(result != nullptr, "failed to create q12 schema");
	Require(!result->HasError(), result->GetError());
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, const std::string &priority) {
	std::string priority_sql = priority == "NULL" ? "NULL" : ("'" + EscapeSqlLiteral(priority) + "'");
	auto result = connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) +
	                               ", 1, 'O', 100.00, DATE '1995-01-01', " + priority_sql +
	                               ", 'Clerk#000000001', 0, 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert order: " + result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t linenumber, const std::string &shipmode,
                    const std::string &shipdate, const std::string &commitdate, const std::string &receiptdate) {
	std::string mode_sql = shipmode == "NULL" ? "NULL" : ("'" + EscapeSqlLiteral(shipmode) + "'");
	std::string ship_sql = shipdate == "NULL" ? "NULL" : ("DATE '" + shipdate + "'");
	std::string commit_sql = commitdate == "NULL" ? "NULL" : ("DATE '" + commitdate + "'");
	std::string receipt_sql = receiptdate == "NULL" ? "NULL" : ("DATE '" + receiptdate + "'");
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, 1, " + std::to_string(linenumber) +
	    ", 1.00, 10.00, 0.00, 0.00, 'N', 'O', " + ship_sql + ", " + commit_sql + ", " + receipt_sql + ", 'NONE', " +
	    mode_sql + ", 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

void CompareParams(duckdb::Connection &connection, const std::string &label, const std::string &shipmode1,
                   const std::string &shipmode2, const std::string &start_date) {
	RequireEqualQ12Results(connection, label, ReferenceQ12Sql(shipmode1, shipmode2, start_date),
	                       AcceleratorQ12Sql(shipmode1, shipmode2, start_date));
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ12Schema(connection, "VARCHAR", true, true);

	// Priorities across high/low/null.
	InsertOrder(connection, 1, "1-URGENT");
	InsertOrder(connection, 2, "2-HIGH");
	InsertOrder(connection, 3, "3-MEDIUM");
	InsertOrder(connection, 4, "4-NOT SPECIFIED");
	InsertOrder(connection, 5, "5-LOW");
	InsertOrder(connection, 6, "NULL");
	InsertOrder(connection, 7, "1-URGENT"); // multi-lineitem order

	// Qualifying baseline rows for MAIL / SHIP in 1995 window [1995-01-01, 1996-01-01).
	InsertLineitem(connection, 1, 1, "MAIL", "1995-01-01", "1995-01-10", "1995-01-20"); // high
	InsertLineitem(connection, 2, 1, "SHIP", "1995-01-01", "1995-01-10", "1995-01-20"); // high
	InsertLineitem(connection, 3, 1, "MAIL", "1995-01-01", "1995-01-10", "1995-01-20"); // low
	InsertLineitem(connection, 4, 1, "SHIP", "1995-01-01", "1995-01-10", "1995-01-20"); // low
	InsertLineitem(connection, 5, 1, "MAIL", "1995-01-01", "1995-01-10", "1995-01-20"); // low
	InsertLineitem(connection, 6, 1, "SHIP", "1995-01-01", "1995-01-10", "1995-01-20"); // null priority → neither
	InsertLineitem(connection, 7, 1, "MAIL", "1995-01-01", "1995-01-10", "1995-01-20"); // high
	InsertLineitem(connection, 7, 2, "MAIL", "1995-01-02", "1995-01-11", "1995-01-21"); // high second line

	// Only first / second ship mode and non-matching mode.
	InsertLineitem(connection, 1, 2, "TRUCK", "1995-01-01", "1995-01-10", "1995-01-20"); // not in (MAIL,SHIP)
	InsertLineitem(connection, 2, 2, "AIR", "1995-01-01", "1995-01-10", "1995-01-20");   // not matching

	// Strict inequality boundaries (should NOT qualify).
	InsertLineitem(connection, 1, 3, "MAIL", "1995-01-01", "1995-01-20", "1995-01-20"); // commit == receipt
	InsertLineitem(connection, 1, 4, "MAIL", "1995-01-10", "1995-01-10", "1995-01-20"); // ship == commit

	// Receipt window bounds: lower inclusive, upper exclusive.
	InsertLineitem(connection, 3, 2, "SHIP", "1994-12-01", "1994-12-10", "1995-01-01"); // lower bound inclusive
	InsertLineitem(connection, 3, 3, "SHIP", "1995-11-01", "1995-11-10", "1996-01-01"); // upper bound exclusive
	InsertLineitem(connection, 3, 4, "SHIP", "1994-11-01", "1994-11-10", "1994-12-31"); // just outside lower
	InsertLineitem(connection, 3, 5, "SHIP", "1995-11-20", "1995-12-01", "1995-12-31"); // just inside upper

	// Missing matching order.
	InsertLineitem(connection, 999, 1, "MAIL", "1995-01-01", "1995-01-10", "1995-01-20");

	// Null shipmode / dates.
	InsertLineitem(connection, 4, 2, "NULL", "1995-01-01", "1995-01-10", "1995-01-20");
	InsertLineitem(connection, 4, 3, "MAIL", "NULL", "1995-01-10", "1995-01-20");
	InsertLineitem(connection, 4, 4, "MAIL", "1995-01-01", "NULL", "1995-01-20");
	InsertLineitem(connection, 4, 5, "MAIL", "1995-01-01", "1995-01-10", "NULL");
}

void SetupEmptyFixture(duckdb::Connection &connection) {
	CreateQ12Schema(connection);
	InsertOrder(connection, 1, "1-URGENT");
	InsertLineitem(connection, 1, 1, "TRUCK", "1995-01-01", "1995-01-10", "1995-01-20");
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ12Schema(connection);
	InsertOrder(connection, 10, "1-URGENT");
	InsertLineitem(connection, 10, 1, "MAIL", "1995-01-01", "1995-01-10", "1995-01-20");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			auto pragma = connection.Query("PRAGMA threads=1");
			Require(pragma != nullptr && !pragma->HasError(), "failed to set PRAGMA threads=1");

			RequireExpectedQ12Schema(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ12Manifest) {
				RequireEqualQ12Results(connection, std::string(entry.label) + "_seed_file", ReadFile(entry.sql_path),
				                       AcceleratorQ12Sql(entry.shipmode1, entry.shipmode2, entry.start_date));
				RequireEqualQ12Results(connection, std::string(entry.label) + "_inline_sql",
				                       ReferenceQ12Sql(entry.shipmode1, entry.shipmode2, entry.start_date),
				                       AcceleratorQ12Sql(entry.shipmode1, entry.shipmode2, entry.start_date));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			CompareParams(connection, "synthetic_both_modes", "MAIL", "SHIP", "1995-01-01");
			CompareParams(connection, "synthetic_only_first", "MAIL", "TRUCK", "1995-01-01");
			CompareParams(connection, "synthetic_only_second", "AIR", "SHIP", "1995-01-01");
			CompareParams(connection, "synthetic_no_matching_modes", "RAIL", "FOB", "1995-01-01");
			CompareParams(connection, "synthetic_same_mode_twice", "MAIL", "MAIL", "1995-01-01");
			CompareParams(connection, "synthetic_empty_window", "MAIL", "SHIP", "1990-01-01");

			auto result = RunQuery(connection, AcceleratorQ12Sql("MAIL", "SHIP", "1995-01-01"));
			auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
			Require(materialized.RowCount() == 2, "synthetic: expected MAIL and SHIP groups");
			Require(materialized.GetValue(0, 0).ToString() == "MAIL", "synthetic: first shipmode");
			Require(materialized.GetValue(0, 1).ToString() == "SHIP", "synthetic: second shipmode");
			// MAIL: orders 1(high),3(low),5(low),7x2(high) = high 3, low 2
			Require(materialized.GetValue(1, 0).GetValue<duckdb::hugeint_t>() == 3, "synthetic: MAIL high");
			Require(materialized.GetValue(2, 0).GetValue<duckdb::hugeint_t>() == 2, "synthetic: MAIL low");
			// SHIP: orders 2(high),4(low),3 lower-bound(low),3 inside-upper(low); null priority adds 0
			// = high 1, low 3
			Require(materialized.GetValue(1, 1).GetValue<duckdb::hugeint_t>() == 1, "synthetic: SHIP high");
			Require(materialized.GetValue(2, 1).GetValue<duckdb::hugeint_t>() == 3, "synthetic: SHIP low");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupEmptyFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			CompareParams(connection, "empty_result", "MAIL", "SHIP", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			CompareParams(connection, "transaction_baseline", "MAIL", "SHIP", "1995-01-01");

			connection.BeginTransaction();

			InsertOrder(connection, 20, "2-HIGH");
			InsertLineitem(connection, 20, 1, "SHIP", "1995-01-01", "1995-01-10", "1995-01-20");
			CompareParams(connection, "after_insert_high_priority_order", "MAIL", "SHIP", "1995-01-01");

			InsertOrder(connection, 21, "3-MEDIUM");
			InsertLineitem(connection, 21, 1, "MAIL", "1995-01-01", "1995-01-10", "1995-01-20");
			CompareParams(connection, "after_insert_low_priority_order", "MAIL", "SHIP", "1995-01-01");

			auto priority_flip =
			    connection.Query("UPDATE orders SET o_orderpriority = '5-LOW' WHERE o_orderkey = 20");
			Require(priority_flip != nullptr && !priority_flip->HasError(), "failed priority high→low");
			CompareParams(connection, "after_priority_high_to_low", "MAIL", "SHIP", "1995-01-01");

			auto priority_flip_back =
			    connection.Query("UPDATE orders SET o_orderpriority = '1-URGENT' WHERE o_orderkey = 20");
			Require(priority_flip_back != nullptr && !priority_flip_back->HasError(), "failed priority low→high");
			CompareParams(connection, "after_priority_low_to_high", "MAIL", "SHIP", "1995-01-01");

			auto leave_shipmode =
			    connection.Query("UPDATE lineitem SET l_shipmode = 'TRUCK' WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(leave_shipmode != nullptr && !leave_shipmode->HasError(), "failed leave shipmode");
			CompareParams(connection, "after_lineitem_leave_shipmode", "MAIL", "SHIP", "1995-01-01");

			auto enter_shipmode =
			    connection.Query("UPDATE lineitem SET l_shipmode = 'MAIL' WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(enter_shipmode != nullptr && !enter_shipmode->HasError(), "failed enter shipmode");
			CompareParams(connection, "after_lineitem_enter_shipmode", "MAIL", "SHIP", "1995-01-01");

			auto break_commit_receipt =
			    connection.Query("UPDATE lineitem SET l_commitdate = DATE '1995-02-01', l_receiptdate = DATE '1995-01-15' "
			                     "WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(break_commit_receipt != nullptr && !break_commit_receipt->HasError(),
			        "failed break commit<receipt");
			CompareParams(connection, "after_leave_commit_lt_receipt", "MAIL", "SHIP", "1995-01-01");

			auto restore_commit_receipt =
			    connection.Query("UPDATE lineitem SET l_commitdate = DATE '1995-01-10', l_receiptdate = DATE '1995-01-20', "
			                     "l_shipdate = DATE '1995-01-01' WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(restore_commit_receipt != nullptr && !restore_commit_receipt->HasError(),
			        "failed restore commit/receipt");
			CompareParams(connection, "after_enter_commit_lt_receipt", "MAIL", "SHIP", "1995-01-01");

			auto break_ship_commit =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-01-15' WHERE l_orderkey = 10 AND "
			                     "l_linenumber = 1");
			Require(break_ship_commit != nullptr && !break_ship_commit->HasError(), "failed break ship<commit");
			CompareParams(connection, "after_leave_ship_lt_commit", "MAIL", "SHIP", "1995-01-01");

			auto restore_ship_commit =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-01-01' WHERE l_orderkey = 10 AND "
			                     "l_linenumber = 1");
			Require(restore_ship_commit != nullptr && !restore_ship_commit->HasError(), "failed restore shipdate");
			CompareParams(connection, "after_enter_ship_lt_commit", "MAIL", "SHIP", "1995-01-01");

			auto leave_window =
			    connection.Query("UPDATE lineitem SET l_receiptdate = DATE '1996-02-01' WHERE l_orderkey = 10 AND "
			                     "l_linenumber = 1");
			Require(leave_window != nullptr && !leave_window->HasError(), "failed leave receipt window");
			CompareParams(connection, "after_leave_receipt_window", "MAIL", "SHIP", "1995-01-01");

			auto enter_window =
			    connection.Query("UPDATE lineitem SET l_receiptdate = DATE '1995-01-20', l_commitdate = DATE '1995-01-10' "
			                     "WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(enter_window != nullptr && !enter_window->HasError(), "failed enter receipt window");
			CompareParams(connection, "after_enter_receipt_window", "MAIL", "SHIP", "1995-01-01");

			auto rekey =
			    connection.Query("UPDATE lineitem SET l_orderkey = 21 WHERE l_orderkey = 10 AND l_linenumber = 1");
			Require(rekey != nullptr && !rekey->HasError(), "failed rekey lineitem");
			CompareParams(connection, "after_lineitem_rekey_to_low_priority", "MAIL", "SHIP", "1995-01-01");

			auto delete_line = connection.Query("DELETE FROM lineitem WHERE l_orderkey = 21 AND l_linenumber = 1");
			Require(delete_line != nullptr && !delete_line->HasError(), "failed delete rekeyed lineitem");
			CompareParams(connection, "after_delete_lineitem", "MAIL", "SHIP", "1995-01-01");

			auto delete_order = connection.Query("DELETE FROM orders WHERE o_orderkey = 20");
			Require(delete_order != nullptr && !delete_order->HasError(), "failed delete order");
			CompareParams(connection, "after_delete_order", "MAIL", "SHIP", "1995-01-01");

			connection.Rollback();
			CompareParams(connection, "after_rollback", "MAIL", "SHIP", "1995-01-01");

			connection.BeginTransaction();
			InsertOrder(connection, 30, "2-HIGH");
			InsertLineitem(connection, 30, 1, "SHIP", "1995-01-01", "1995-01-10", "1995-01-20");
			connection.Commit();
			CompareParams(connection, "after_commit_visible", "MAIL", "SHIP", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ12Schema(connection, "INTEGER");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ12Sql("MAIL", "SHIP", "1995-01-01"),
			                          "Unsupported schema");
		}

		std::cout << "test_tpch_q12 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q12 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
