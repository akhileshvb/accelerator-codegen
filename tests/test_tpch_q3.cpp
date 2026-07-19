#include "accelerator_codegen/register.hpp"
#include "test_support/q3_manifest.hpp"

#include <cctype>
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

std::string ReferenceQ3Sql(const std::string &market_segment, const std::string &cutoff_date) {
	// Canonicalize remaining ties with l_orderkey, o_shippriority so differential order
	// matches the accelerator's deterministic BetterRow (DuckDB leaves those ties unspecified).
	return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
	       "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority "
	       "FROM customer, orders, lineitem "
	       "WHERE c_mktsegment = '" +
	       market_segment +
	       "' "
	       "AND c_custkey = o_custkey "
	       "AND l_orderkey = o_orderkey "
	       "AND o_orderdate < DATE '" +
	       cutoff_date +
	       "' "
	       "AND l_shipdate > DATE '" +
	       cutoff_date +
	       "' "
	       "GROUP BY l_orderkey, o_orderdate, o_shippriority "
	       "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority "
	       "LIMIT 10"
	       ")";
}

std::string AcceleratorQ3Sql(const std::string &market_segment, const std::string &cutoff_date) {
	return "SELECT * FROM accel_tpch_q3('" + market_segment + "', DATE '" + cutoff_date + "')";
}

std::string CanonicalizeAccelOrderSql(const std::string &accelerator_sql) {
	return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM (" + accelerator_sql +
	       ") ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority";
}

std::string StripTrailingSemicolon(std::string sql) {
	while (!sql.empty() && (std::isspace(static_cast<unsigned char>(sql.back())) || sql.back() == ';')) {
		sql.pop_back();
	}
	return sql;
}

void RequireEqualQ3Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                           const std::string &accelerator_sql) {
	// Seed SQL files already ORDER BY revenue, o_orderdate LIMIT 10; wrap both sides with the
	// same full tie-break so unspecified DuckDB ties do not flake differential checks.
	const bool seed_file = reference_sql.find("accel_tpch_q3") == std::string::npos &&
	                       reference_sql.find("ORDER BY") != std::string::npos;
	const std::string expected_sql =
	    seed_file ? ("SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM (" +
	                 StripTrailingSemicolon(reference_sql) +
	                 ") ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority")
	              : reference_sql;
	const std::string actual_sql = CanonicalizeAccelOrderSql(accelerator_sql);

	auto expected = RunQuery(connection, expected_sql);
	auto actual = RunQuery(connection, actual_sql);

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

void RequireOrderedByRevenueThenDate(duckdb::MaterializedQueryResult &result, const std::string &label) {
	for (duckdb::idx_t row = 1; row < result.RowCount(); row++) {
		const auto prev_revenue = result.GetValue(1, row - 1);
		const auto cur_revenue = result.GetValue(1, row);
		if (prev_revenue != cur_revenue) {
			Require(prev_revenue > cur_revenue, label + ": revenue not descending at row " + std::to_string(row));
			continue;
		}
		const auto prev_date = result.GetValue(2, row - 1).GetValue<duckdb::date_t>();
		const auto cur_date = result.GetValue(2, row).GetValue<duckdb::date_t>();
		Require(!(cur_date < prev_date), label + ": orderdate not ascending for equal revenue at row " +
		                                     std::to_string(row));
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

void RequireExpectedQ3Schema(duckdb::Connection &connection) {
	auto result = RunQuery(connection, AcceleratorQ3Sql("BUILDING", "1995-03-15"));
	const duckdb::vector<duckdb::string> expected_names = {"l_orderkey", "revenue", "o_orderdate", "o_shippriority"};
	const duckdb::vector<duckdb::LogicalType> expected_types = {
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DECIMAL(38, 4),
	    duckdb::LogicalType::DATE,
	    duckdb::LogicalType::INTEGER,
	};
	Require(result->names == expected_names, "q3 schema names differ");
	Require(result->types == expected_types, "q3 schema types differ");
}

void CreateQ3Schema(duckdb::Connection &connection, const std::string &shippriority_type = "INTEGER") {
	auto result = connection.Query(
	    "CREATE TABLE customer("
	    "c_custkey BIGINT NOT NULL, "
	    "c_mktsegment VARCHAR NOT NULL);"
	    "CREATE TABLE orders("
	    "o_orderkey BIGINT NOT NULL, "
	    "o_custkey BIGINT NOT NULL, "
	    "o_orderdate DATE NOT NULL, "
	    "o_shippriority " +
	    shippriority_type +
	    " NOT NULL);"
	    "CREATE TABLE lineitem("
	    "l_orderkey BIGINT NOT NULL, "
	    "l_shipdate DATE NOT NULL, "
	    "l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL);");
	Require(result != nullptr, "failed to create q3 schema");
	Require(!result->HasError(), result->GetError());
}

void SetupBaselineFixture(duckdb::Connection &connection) {
	CreateQ3Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO customer VALUES
			(1, 'BUILDING'),
			(2, 'AUTOMOBILE'),
			(3, 'BUILDING'),
			(4, 'HOUSEHOLD');

		INSERT INTO orders VALUES
			(10, 1, DATE '1995-03-10', 1),
			(11, 1, DATE '1995-03-14', 2),
			(12, 2, DATE '1995-03-10', 3),
			(13, 3, DATE '1995-03-16', 4),
			(14, 3, DATE '1995-03-01', 5);

		INSERT INTO lineitem VALUES
			(10, DATE '1995-03-16', 100.00, 0.05),
			(10, DATE '1995-03-18', 50.00, 0.00),
			(11, DATE '1995-03-20', 60.00, 0.10),
			(12, DATE '1995-03-20', 200.00, 0.02),
			(13, DATE '1995-03-20', 300.00, 0.03),
			(14, DATE '1995-03-10', 80.00, 0.04);
	)SQL");
	Require(result != nullptr, "failed to populate baseline fixture");
	Require(!result->HasError(), result->GetError());
}

// Covers: no matching segment, multiple segments, date boundaries, multi-lineitem aggregation,
// equal-revenue orders, equal revenue+date, fewer than 10 / more than 10 results.
void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ3Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO customer VALUES
			(1, 'BUILDING'),
			(2, 'AUTOMOBILE'),
			(3, 'FURNITURE'),
			(4, 'HOUSEHOLD'),
			(5, 'BUILDING');

		INSERT INTO orders VALUES
			-- BUILDING qualifying orders (cutoff 1995-03-15): orderdate < cutoff
			(100, 1, DATE '1995-03-14', 0),  -- just before cutoff
			(101, 1, DATE '1995-03-15', 0),  -- ON cutoff: must be excluded (strict <)
			(102, 1, DATE '1995-03-10', 0),  -- multi lineitem aggregation
			(103, 5, DATE '1995-03-01', 1),  -- second BUILDING customer
			(104, 5, DATE '1995-03-02', 1),  -- equal revenue vs 105 (different dates)
			(105, 5, DATE '1995-03-03', 1),  -- equal revenue vs 104 (later date → later in ORDER BY)
			(106, 5, DATE '1995-03-04', 2),  -- equal revenue+date vs 107
			(107, 5, DATE '1995-03-04', 3),  -- equal revenue+date vs 106
			(108, 5, DATE '1995-03-05', 0),
			(109, 5, DATE '1995-03-06', 0),
			(110, 5, DATE '1995-03-07', 0),
			(111, 5, DATE '1995-03-08', 0),
			(112, 5, DATE '1995-03-09', 0),  -- 100,102-112 = 12 BUILDING orders with revenue (>10)
			-- AUTOMOBILE order (wrong segment for BUILDING queries)
			(200, 2, DATE '1995-03-10', 0),
			-- FURNITURE order
			(300, 3, DATE '1995-03-10', 0);

		INSERT INTO lineitem VALUES
			-- order 100: qualifies (shipdate > cutoff)
			(100, DATE '1995-03-16', 100.00, 0.00),
			-- order 101: would have revenue but orderdate on boundary → excluded
			(101, DATE '1995-03-20', 999.00, 0.00),
			-- order 102: two qualifying lineitems; one on shipdate boundary (excluded)
			(102, DATE '1995-03-15', 40.00, 0.00),   -- ON cutoff: must be excluded (strict >)
			(102, DATE '1995-03-16', 30.00, 0.10),   -- qualifies: 30 * 0.9 = 27
			(102, DATE '1995-03-20', 20.00, 0.00),   -- qualifies: 20
			-- order 103
			(103, DATE '1995-03-20', 50.00, 0.00),
			-- orders 104/105: equal revenue 80.00, different dates
			(104, DATE '1995-03-20', 80.00, 0.00),
			(105, DATE '1995-03-20', 80.00, 0.00),
			-- orders 106/107: equal revenue 70.00 AND equal orderdate
			(106, DATE '1995-03-20', 70.00, 0.00),
			(107, DATE '1995-03-20', 70.00, 0.00),
			-- filler orders for >10 results (distinct descending revenues)
			(108, DATE '1995-03-20', 60.00, 0.00),
			(109, DATE '1995-03-20', 55.00, 0.00),
			(110, DATE '1995-03-20', 45.00, 0.00),
			(111, DATE '1995-03-20', 35.00, 0.00),
			(112, DATE '1995-03-20', 25.00, 0.00),
			-- non-BUILDING
			(200, DATE '1995-03-20', 500.00, 0.00),
			(300, DATE '1995-03-20', 400.00, 0.00);
	)SQL");
	Require(result != nullptr, "failed to populate synthetic fixture");
	Require(!result->HasError(), result->GetError());
}

void SetupEmptySegmentFixture(duckdb::Connection &connection) {
	CreateQ3Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO customer VALUES (1, 'BUILDING');
		INSERT INTO orders VALUES (10, 1, DATE '1995-03-10', 0);
		INSERT INTO lineitem VALUES (10, DATE '1995-03-20', 100.00, 0.00);
	)SQL");
	Require(result != nullptr && !result->HasError(), "failed to populate empty-segment fixture");
}

void SetupFewerThanTenFixture(duckdb::Connection &connection) {
	CreateQ3Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO customer VALUES (1, 'BUILDING');
		INSERT INTO orders VALUES
			(10, 1, DATE '1995-03-10', 0),
			(11, 1, DATE '1995-03-11', 0);
		INSERT INTO lineitem VALUES
			(10, DATE '1995-03-20', 100.00, 0.05),
			(11, DATE '1995-03-20', 50.00, 0.00);
	)SQL");
	Require(result != nullptr && !result->HasError(), "failed to populate fewer-than-ten fixture");
}

void SetupEmptyResultFixture(duckdb::Connection &connection) {
	CreateQ3Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO customer VALUES (1, 'BUILDING');
		INSERT INTO orders VALUES (10, 1, DATE '1995-03-20', 0);  -- orderdate >= cutoff
		INSERT INTO lineitem VALUES (10, DATE '1995-03-10', 100.00, 0.00);  -- shipdate <= cutoff
	)SQL");
	Require(result != nullptr && !result->HasError(), "failed to populate empty-result fixture");
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ3Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO customer VALUES
			(1, 'BUILDING'),
			(2, 'AUTOMOBILE');
		INSERT INTO orders VALUES
			(10, 1, DATE '1995-03-10', 1),
			(11, 2, DATE '1995-03-10', 2);
		INSERT INTO lineitem VALUES
			(10, DATE '1995-03-20', 100.00, 0.05),
			(11, DATE '1995-03-20', 200.00, 0.00);
	)SQL");
	Require(result != nullptr && !result->HasError(), "failed to populate transaction fixture");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireExpectedQ3Schema(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ3Manifest) {
				// Use reference SQL with full deterministic tie-break (orderkey, shippriority)
				// so LIMIT 10 matches the accelerator when DuckDB leaves remaining ties unspecified.
				RequireEqualQ3Results(connection, entry.label,
				                      ReferenceQ3Sql(entry.market_segment, entry.cutoff_date),
				                      AcceleratorQ3Sql(entry.market_segment, entry.cutoff_date));
				auto ordered = RunQuery(connection, AcceleratorQ3Sql(entry.market_segment, entry.cutoff_date));
				RequireOrderedByRevenueThenDate(ordered->Cast<duckdb::MaterializedQueryResult>(),
				                                std::string(entry.label) + "_order_check");
				// Also confirm seed-file SQL (TPC-H ORDER BY only) agrees on the row multiset.
				auto seed_result = RunQuery(connection, StripTrailingSemicolon(ReadFile(entry.sql_path)));
				auto accel_result = RunQuery(connection, AcceleratorQ3Sql(entry.market_segment, entry.cutoff_date));
				auto &seed_mat = seed_result->Cast<duckdb::MaterializedQueryResult>();
				auto &accel_mat = accel_result->Cast<duckdb::MaterializedQueryResult>();
				Require(seed_mat.RowCount() == accel_mat.RowCount(),
				        std::string(entry.label) + ": seed file row count differs from accelerator");
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupBaselineFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireEqualQ3Results(connection, "baseline_fixture", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireEqualQ3Results(connection, "synthetic_building", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			auto building = RunQuery(connection, AcceleratorQ3Sql("BUILDING", "1995-03-15"));
			auto &building_mat = building->Cast<duckdb::MaterializedQueryResult>();
			Require(building_mat.RowCount() == 10, "synthetic_building: expected LIMIT 10");
			RequireOrderedByRevenueThenDate(building_mat, "synthetic_building");
			// Highest revenue should be order 100 (100.00), then 104 (80, earlier date), then 105 (80)
			Require(building_mat.GetValue(0, 0).GetValue<int64_t>() == 100, "synthetic: top orderkey");
			Require(building_mat.GetValue(1, 0).ToString() == "100.0000", "synthetic: top revenue DECIMAL");
			// Order 101 (on orderdate boundary) and shipdate-boundary lineitem must not appear
			for (duckdb::idx_t row = 0; row < building_mat.RowCount(); row++) {
				Require(building_mat.GetValue(0, row).GetValue<int64_t>() != 101,
				        "synthetic: orderdate-boundary order 101 leaked");
			}
			// Aggregated multi-lineitem order 102 revenue = 27 + 20 = 47
			bool found_102 = false;
			for (duckdb::idx_t row = 0; row < building_mat.RowCount(); row++) {
				if (building_mat.GetValue(0, row).GetValue<int64_t>() == 102) {
					found_102 = true;
					Require(building_mat.GetValue(1, row).ToString() == "47.0000",
					        "synthetic: multi-lineitem revenue for order 102");
				}
			}
			Require(found_102, "synthetic: expected aggregated order 102 in top-10");

			// No matching segment
			RequireEqualQ3Results(connection, "no_matching_segment", ReferenceQ3Sql("MACHINERY", "1995-03-15"),
			                      AcceleratorQ3Sql("MACHINERY", "1995-03-15"));
			auto none = RunQuery(connection, AcceleratorQ3Sql("MACHINERY", "1995-03-15"));
			Require(none->Cast<duckdb::MaterializedQueryResult>().RowCount() == 0,
			        "no_matching_segment: expected empty");

			// Different segment with results
			RequireEqualQ3Results(connection, "automobile_segment", ReferenceQ3Sql("AUTOMOBILE", "1995-03-15"),
			                      AcceleratorQ3Sql("AUTOMOBILE", "1995-03-15"));
			RequireEqualQ3Results(connection, "furniture_segment", ReferenceQ3Sql("FURNITURE", "1995-03-15"),
			                      AcceleratorQ3Sql("FURNITURE", "1995-03-15"));
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupEmptySegmentFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireEqualQ3Results(connection, "empty_segment_household", ReferenceQ3Sql("HOUSEHOLD", "1995-03-15"),
			                      AcceleratorQ3Sql("HOUSEHOLD", "1995-03-15"));
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupFewerThanTenFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireEqualQ3Results(connection, "fewer_than_ten", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));
			auto result = RunQuery(connection, AcceleratorQ3Sql("BUILDING", "1995-03-15"));
			Require(result->Cast<duckdb::MaterializedQueryResult>().RowCount() == 2, "fewer_than_ten: expected 2 rows");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupEmptyResultFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireEqualQ3Results(connection, "empty_result", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));
			auto result = RunQuery(connection, AcceleratorQ3Sql("BUILDING", "1995-03-15"));
			Require(result->Cast<duckdb::MaterializedQueryResult>().RowCount() == 0, "empty_result: expected 0 rows");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireEqualQ3Results(connection, "transaction_baseline", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			connection.BeginTransaction();

			// Insert customer/order/lineitem into selected segment
			auto insert_customer = connection.Query("INSERT INTO customer VALUES (5, 'BUILDING')");
			Require(insert_customer != nullptr && !insert_customer->HasError(), "customer insert failed");
			auto insert_order = connection.Query("INSERT INTO orders VALUES (15, 5, DATE '1995-03-12', 6)");
			Require(insert_order != nullptr && !insert_order->HasError(), "order insert failed");
			auto insert_lineitem =
			    connection.Query("INSERT INTO lineitem VALUES (15, DATE '1995-03-17', 500.00, 0.05)");
			Require(insert_lineitem != nullptr && !insert_lineitem->HasError(), "lineitem insert failed");
			RequireEqualQ3Results(connection, "after_insert_all_three", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Customer enters selected market segment
			auto enter_segment = connection.Query("UPDATE customer SET c_mktsegment = 'BUILDING' WHERE c_custkey = 2");
			Require(enter_segment != nullptr && !enter_segment->HasError(), "customer enter segment failed");
			RequireEqualQ3Results(connection, "after_customer_enter_segment", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Customer leaves selected market segment
			auto leave_segment =
			    connection.Query("UPDATE customer SET c_mktsegment = 'AUTOMOBILE' WHERE c_custkey = 2");
			Require(leave_segment != nullptr && !leave_segment->HasError(), "customer leave segment failed");
			RequireEqualQ3Results(connection, "after_customer_leave_segment", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Order leaves order-date predicate (date moves onto/after cutoff)
			auto order_leave = connection.Query("UPDATE orders SET o_orderdate = DATE '1995-03-15' WHERE o_orderkey = 10");
			Require(order_leave != nullptr && !order_leave->HasError(), "order leave date failed");
			RequireEqualQ3Results(connection, "after_order_leave_date", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Order enters order-date predicate
			auto order_enter = connection.Query("UPDATE orders SET o_orderdate = DATE '1995-03-10' WHERE o_orderkey = 10");
			Require(order_enter != nullptr && !order_enter->HasError(), "order enter date failed");
			RequireEqualQ3Results(connection, "after_order_enter_date", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Lineitem leaves ship-date predicate
			auto ship_leave =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-03-15' WHERE l_orderkey = 10");
			Require(ship_leave != nullptr && !ship_leave->HasError(), "lineitem leave shipdate failed");
			RequireEqualQ3Results(connection, "after_lineitem_leave_shipdate", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Lineitem enters ship-date predicate
			auto ship_enter =
			    connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-03-20' WHERE l_orderkey = 10");
			Require(ship_enter != nullptr && !ship_enter->HasError(), "lineitem enter shipdate failed");
			RequireEqualQ3Results(connection, "after_lineitem_enter_shipdate", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Updates to extended price and discount
			auto price_update = connection.Query(
			    "UPDATE lineitem SET l_extendedprice = 110.00, l_discount = 0.20 WHERE l_orderkey = 10");
			Require(price_update != nullptr && !price_update->HasError(), "lineitem value update failed");
			RequireEqualQ3Results(connection, "after_price_discount_update", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			// Deletes across tables
			auto delete_lineitem = connection.Query("DELETE FROM lineitem WHERE l_orderkey = 15");
			Require(delete_lineitem != nullptr && !delete_lineitem->HasError(), "lineitem delete failed");
			RequireEqualQ3Results(connection, "after_delete_lineitem", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			auto delete_order = connection.Query("DELETE FROM orders WHERE o_orderkey = 15");
			Require(delete_order != nullptr && !delete_order->HasError(), "order delete failed");
			RequireEqualQ3Results(connection, "after_delete_order", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			auto delete_customer = connection.Query("DELETE FROM customer WHERE c_custkey = 5");
			Require(delete_customer != nullptr && !delete_customer->HasError(), "customer delete failed");
			RequireEqualQ3Results(connection, "after_delete_customer", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));

			connection.Rollback();
			RequireEqualQ3Results(connection, "after_rollback", ReferenceQ3Sql("BUILDING", "1995-03-15"),
			                      AcceleratorQ3Sql("BUILDING", "1995-03-15"));
		}

		{
			duckdb::DuckDB mismatch_database(nullptr);
			duckdb::Connection connection(mismatch_database);
			CreateQ3Schema(connection, "DOUBLE");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ3Sql("BUILDING", "1995-03-15"),
			                          "Unsupported schema");
		}

		std::cout << "tpch_q3_smoke_test passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "tpch_q3_smoke_test failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
