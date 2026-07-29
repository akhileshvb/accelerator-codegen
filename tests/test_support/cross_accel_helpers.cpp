#include "test_support/cross_accel_helpers.hpp"

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {
namespace {

bool ApproxDoublesMatch(double expected, double actual) {
	if (std::isnan(expected) && std::isnan(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	if (std::isinf(expected) && std::isinf(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	const auto scale = std::max({1.0, std::abs(expected), std::abs(actual)});
	return std::abs(expected - actual) <= 1e-9 * scale;
}

bool ValuesMatch(const duckdb::Value &expected, const duckdb::Value &actual) {
	if (expected.IsNull() != actual.IsNull()) {
		return false;
	}
	if (expected.IsNull()) {
		return true;
	}
	if (expected.type().id() == duckdb::LogicalTypeId::DOUBLE) {
		return ApproxDoublesMatch(expected.GetValue<double>(), actual.GetValue<double>());
	}
	return expected.ToString() == actual.ToString();
}

void RequireResultsEqual(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	if (expected == nullptr || expected->HasError()) {
		throw std::runtime_error(label + ": reference failed: " + (expected ? expected->GetError() : "null"));
	}
	if (actual == nullptr || actual->HasError()) {
		throw std::runtime_error(label + ": accelerator failed: " + (actual ? actual->GetError() : "null"));
	}
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	if (expected_m.RowCount() != actual_m.RowCount()) {
		throw std::runtime_error(label + ": row count differs, expected=" + std::to_string(expected_m.RowCount()) +
		                         " actual=" + std::to_string(actual_m.RowCount()));
	}
	for (duckdb::idx_t row = 0; row < expected_m.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < expected_m.ColumnCount(); col++) {
			if (!ValuesMatch(expected_m.GetValue(col, row), actual_m.GetValue(col, row))) {
				throw std::runtime_error(label + ": row " + std::to_string(row) + " col " + std::to_string(col) +
				                         " differs, expected=" + expected_m.GetValue(col, row).ToString() +
				                         " actual=" + actual_m.GetValue(col, row).ToString());
			}
		}
	}
}

std::string ReferenceSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT l_returnflag, l_linestatus, "
		       "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
		       "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
		       "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
		       "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, avg(l_discount) AS avg_disc, "
		       "count(*) AS count_order "
		       "FROM lineitem "
		       "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
		       "GROUP BY l_returnflag, l_linestatus "
		       "ORDER BY l_returnflag, l_linestatus";
	case 3:
		return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
		       "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority "
		       "FROM customer, orders, lineitem "
		       "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey "
		       "AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15' "
		       "GROUP BY l_orderkey, o_orderdate, o_shippriority "
		       "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)";
	case 10:
		return "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')";
	case 12:
		return "SELECT * FROM accel_tpch_q12('MAIL', 'SHIP', DATE '1995-01-01')";
	case 14:
		return "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) / "
		       "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
		       "FROM lineitem, part "
		       "WHERE l_partkey = p_partkey "
		       "AND l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month";
	case 6:
		return "SELECT revenue FROM accel_tpch_q6(DATE '1995-01-01', 0.03, 25)";
	default:
		throw std::runtime_error("unsupported query for reference SQL: " + std::to_string(query_num));
	}
}

std::string AuxSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT * FROM accel_tpch_q1_aux(90)";
	case 3:
		return "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')";
	case 10:
		return "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')";
	case 12:
		return "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')";
	case 14:
		return "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')";
	case 6:
		return "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)";
	default:
		throw std::runtime_error("unsupported query for aux SQL: " + std::to_string(query_num));
	}
}

std::string CanonicalizeSql(int query_num, const std::string &sql) {
	if (query_num == 3) {
		return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM (" + sql +
		       ") ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority";
	}
	return sql;
}

std::string PointLineitemUpdateSql(const std::string &where_clause, int bulk_row_limit) {
	const auto limit = std::max(1, bulk_row_limit);
	return "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0000001 "
	       "WHERE (l_orderkey, l_linenumber) IN ("
	       "SELECT l_orderkey, l_linenumber FROM lineitem WHERE " +
	       where_clause + " ORDER BY l_orderkey, l_linenumber LIMIT " + std::to_string(limit) + ")";
}

} // namespace

std::string CrossAccelAuxSql(int query_num) {
	return AuxSql(query_num);
}

std::vector<int> CrossAccelTestQueries() {
	const char *raw = std::getenv("ACCEL_CROSS_ACCEL_TEST_QUERIES");
	if (raw == nullptr || *raw == '\0') {
		return {1, 3, 6, 10, 12, 14};
	}
	std::vector<int> queries;
	std::stringstream stream(raw);
	std::string token;
	while (std::getline(stream, token, ',')) {
		if (token.empty()) {
			continue;
		}
		queries.push_back(std::stoi(token));
	}
	if (queries.empty()) {
		throw std::runtime_error("ACCEL_CROSS_ACCEL_TEST_QUERIES is empty");
	}
	return queries;
}

std::vector<int> CrossAccelQueriesIncludingTarget(int target_query) {
	auto queries = CrossAccelTestQueries();
	if (std::find(queries.begin(), queries.end(), target_query) == queries.end()) {
		queries.push_back(target_query);
	}
	return queries;
}

void BuildCrossAccelAux(duckdb::Connection &connection, int query_num) {
	switch (query_num) {
	case 1:
		connection.Query("SELECT * FROM accel_q1_aux_build()");
		break;
	case 3:
		connection.Query("SELECT * FROM accel_q3_aux_build()");
		break;
	case 10:
		connection.Query("SELECT * FROM accel_q10_aux_build()");
		break;
	case 12:
		connection.Query("SELECT * FROM accel_q12_aux_build()");
		break;
	case 14:
		connection.Query("SELECT * FROM accel_q14_aux_build()");
		break;
	case 6:
		connection.Query("SELECT * FROM accel_q6_aux_build()");
		break;
	default:
		throw std::runtime_error("unsupported query for aux build: " + std::to_string(query_num));
	}
}

std::string InvalidatingWriterSqlForQuery(int query_num, int bulk_row_limit) {
	switch (query_num) {
	case 1:
		return PointLineitemUpdateSql("l_shipdate = DATE '1995-06-01'", bulk_row_limit);
	case 3:
		return PointLineitemUpdateSql(
		    "l_orderkey IN (SELECT o_orderkey FROM customer JOIN orders ON c_custkey = o_custkey "
		    "WHERE c_mktsegment = 'BUILDING' AND o_orderdate < DATE '1995-03-15') "
		    "AND l_shipdate > DATE '1995-03-15'",
		    bulk_row_limit);
	case 10:
		return PointLineitemUpdateSql(
		    "l_returnflag = 'R' AND l_orderkey IN ("
		    "SELECT o_orderkey FROM orders "
		    "WHERE o_orderdate >= DATE '1993-06-01' AND o_orderdate < DATE '1993-07-01')",
		    bulk_row_limit);
	case 12:
		return PointLineitemUpdateSql(
		    "l_receiptdate >= DATE '1995-01-01' AND l_receiptdate < DATE '1995-02-01' "
		    "AND l_shipmode IN ('MAIL', 'SHIP')",
		    bulk_row_limit);
	case 14:
		return PointLineitemUpdateSql(
		    "l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'",
		    bulk_row_limit);
	case 6:
		return PointLineitemUpdateSql(
		    "l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'",
		    bulk_row_limit);
	default:
		throw std::runtime_error("unsupported query for invalidating writer SQL: " + std::to_string(query_num));
	}
}

void ApplyInvalidatingWriterSql(duckdb::Connection &connection, int query_num) {
	const auto sql = InvalidatingWriterSqlForQuery(query_num);
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		const auto message = result ? result->GetError() : "null query result";
		throw std::runtime_error("invalidating writer failed for Q" + std::to_string(query_num) + ": " + message);
	}
}

void RequireCrossAccelQueryCorrectness(duckdb::Connection &connection, int query_num, const std::string &label) {
	const auto reference = CanonicalizeSql(query_num, ReferenceSql(query_num));
	const auto aux = CanonicalizeSql(query_num, AuxSql(query_num));
	RequireResultsEqual(connection, label, reference, aux);
}

void RequireCrossAccelCorrectness(duckdb::Connection &connection, const std::vector<int> &queries) {
	for (int query_num : queries) {
		RequireCrossAccelQueryCorrectness(connection, query_num, "Q" + std::to_string(query_num));
	}
}

} // namespace accelerator_codegen::test_support
