#include "test_support/cross_accel_helpers.hpp"

#include "test_support/result_compare.hpp"

#include <unistd.h>

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {
namespace {

constexpr const char *kDefaultTpchDb = "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
constexpr const char *kDefaultSecEdgarDb = "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";

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

std::string TpchReferenceSql(int query_num) {
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
		throw std::runtime_error("unsupported TPC-H query for reference SQL: " + std::to_string(query_num));
	}
}

std::string TpchAuxSql(int query_num) {
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
		throw std::runtime_error("unsupported TPC-H query for aux SQL: " + std::to_string(query_num));
	}
}

std::string SecEdgarReferenceSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, AVG(line) AS avg_line_num "
		       "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC";
	case 2:
		return "SELECT s.name, n.tag, n.value "
		       "FROM num n "
		       "JOIN sub s ON n.adsh = s.adsh "
		       "JOIN ("
		       "    SELECT adsh, tag, MAX(value) AS max_value "
		       "    FROM num "
		       "    WHERE uom = 'pure' AND value IS NOT NULL "
		       "    GROUP BY adsh, tag"
		       ") m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
		       "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
		       "ORDER BY n.value DESC, s.name, n.tag "
		       "LIMIT 100";
	case 3:
		return "SELECT s.name, s.cik, SUM(n.value) AS total_value "
		       "FROM num n "
		       "JOIN sub s ON n.adsh = s.adsh "
		       "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
		       "GROUP BY s.name, s.cik "
		       "HAVING SUM(n.value) > ("
		       "    SELECT AVG(sub_total) FROM ("
		       "        SELECT SUM(n2.value) AS sub_total "
		       "        FROM num n2 "
		       "        JOIN sub s2 ON n2.adsh = s2.adsh "
		       "        WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL "
		       "        GROUP BY s2.cik"
		       "    ) avg_sub"
		       ") "
		       "ORDER BY total_value DESC "
		       "LIMIT 100";
	case 4:
		return "SELECT s.sic, t.tlabel, p.stmt, "
		       "COUNT(DISTINCT s.cik) AS num_companies, "
		       "SUM(n.value) AS total_value, "
		       "AVG(n.value) AS avg_value "
		       "FROM num n "
		       "JOIN sub s ON n.adsh = s.adsh "
		       "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
		       "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
		       "WHERE n.uom = 'USD' AND p.stmt = 'EQ' "
		       "AND s.sic BETWEEN 4000 AND 4999 "
		       "AND n.value IS NOT NULL AND t.abstract = 0 "
		       "GROUP BY s.sic, t.tlabel, p.stmt "
		       "HAVING COUNT(DISTINCT s.cik) >= 2 "
		       "ORDER BY total_value DESC "
		       "LIMIT 500";
	case 6:
		return "SELECT s.name, p.stmt, n.tag, p.plabel, "
		       "SUM(n.value) AS total_value, COUNT(*) AS cnt "
		       "FROM num n "
		       "JOIN sub s ON n.adsh = s.adsh "
		       "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
		       "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
		       "AND n.value IS NOT NULL "
		       "GROUP BY s.name, p.stmt, n.tag, p.plabel "
		       "ORDER BY total_value DESC, s.name, n.tag, p.plabel "
		       "LIMIT 200";
	case 24:
		return "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
		       "FROM num n "
		       "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh "
		       "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
		       "AND n.value IS NOT NULL AND p.adsh IS NULL "
		       "GROUP BY n.tag, n.version "
		       "HAVING COUNT(*) > 10 "
		       "ORDER BY cnt DESC "
		       "LIMIT 100";
	default:
		throw std::runtime_error("unsupported SEC EDGAR query for reference SQL: " + std::to_string(query_num));
	}
}

std::string SecEdgarAuxSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT * FROM accel_sec_edgar_q1_aux()";
	case 2:
		return "SELECT * FROM accel_sec_edgar_q2_aux()";
	case 3:
		return "SELECT * FROM accel_sec_edgar_q3_aux()";
	case 4:
		return "SELECT * FROM accel_sec_edgar_q4_aux()";
	case 6:
		return "SELECT * FROM accel_sec_edgar_q6_aux()";
	case 24:
		return "SELECT * FROM accel_sec_edgar_q24_aux()";
	default:
		throw std::runtime_error("unsupported SEC EDGAR query for aux SQL: " + std::to_string(query_num));
	}
}

std::string CanonicalizeSql(CrossAccelBenchmark benchmark, int query_num, const std::string &sql) {
	if (benchmark == CrossAccelBenchmark::Tpch && query_num == 3) {
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

std::string SecEdgarQ1InvalidatingWriterSql(int write_seq) {
	(void)write_seq;
	return "";
}

void ApplySecEdgarQ1FilingAppendWriter(duckdb::Connection &connection, int write_seq) {
	const auto sql = "INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating) "
	                 "SELECT 'bench-" +
	                 std::to_string(write_seq) + "-' || adsh, report, line + " + std::to_string(1 + (write_seq % 1000)) +
	                 ", stmt, inpth, rfile, tag, version, plabel, negating FROM pre WHERE stmt = 'BS' AND rfile = 'H' "
	                 "LIMIT 1";
	connection.BeginTransaction();
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		connection.Rollback();
		throw std::runtime_error("SEC EDGAR Q1 filing_append writer failed: " + (result ? result->GetError() : "null"));
	}
	connection.Commit();
}

void ApplySecEdgarQ1InvalidatingWriter(duckdb::Connection &connection, int write_seq) {
	const auto sql = "UPDATE pre SET plabel = 'cross-accel-" + std::to_string(write_seq) +
	                 "' WHERE adsh = (SELECT adsh FROM pre WHERE stmt IS NOT NULL AND rfile IS NOT NULL ORDER BY adsh "
	                 "LIMIT 1 OFFSET " +
	                 std::to_string(write_seq % 256) + ")";
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error("SEC EDGAR Q1 invalidating writer failed: " + (result ? result->GetError() : "null"));
	}
}

std::string SqlLiteralOrNull(const duckdb::Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	return "'" + value.ToString() + "'";
}

void ApplySecEdgarNumValueInsertWriter(duckdb::Connection &connection, const std::string &uom, int write_seq) {
	auto seed = connection.Query(
	    "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.coreg, n.value, n.footnote "
	    "FROM num n JOIN sub s ON n.adsh = s.adsh "
	    "WHERE n.uom = '" +
	    uom + "' AND s.fy = 2022 AND n.value IS NOT NULL ORDER BY n.adsh LIMIT 1 OFFSET " +
	    std::to_string(write_seq % 256));
	if (!seed || seed->HasError()) {
		throw std::runtime_error("SEC EDGAR num writer seed query failed: " + (seed ? seed->GetError() : "null"));
	}
	auto &materialized = seed->Cast<duckdb::MaterializedQueryResult>();
	if (materialized.RowCount() == 0) {
		throw std::runtime_error("SEC EDGAR num writer seed query returned no rows");
	}
	const auto adsh = materialized.GetValue(0, 0).ToString();
	const auto tag = materialized.GetValue(1, 0).ToString();
	const auto version = materialized.GetValue(2, 0).ToString();
	const auto ddate = materialized.GetValue(3, 0).ToString();
	const auto qtrs = materialized.GetValue(4, 0).ToString();
	const auto coreg = SqlLiteralOrNull(materialized.GetValue(5, 0));
	const auto value = materialized.GetValue(6, 0).GetValue<double>() + 0.001 * static_cast<double>(1 + (write_seq % 1000));
	const auto footnote = SqlLiteralOrNull(materialized.GetValue(7, 0));
	const auto sql = "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value, footnote) VALUES ('" +
	                 adsh + "', '" + tag + "', '" + version + "', " + ddate + ", " + qtrs + ", '" + uom + "', " +
	                 coreg + ", " + std::to_string(value) + ", " + footnote + ")";
	connection.BeginTransaction();
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		connection.Rollback();
		throw std::runtime_error("SEC EDGAR num invalidating writer failed: " + (result ? result->GetError() : "null"));
	}
	connection.Commit();
}

std::string SecEdgarQ4ValueAppendSql(int write_seq) {
	return "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value, footnote) "
	       "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
	       "n.value + 0.001 * " +
	       std::to_string(1 + (write_seq % 1000)) +
	       ", n.footnote FROM num n "
	       "JOIN sub s ON n.adsh = s.adsh "
	       "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
	       "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
	       "WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999 "
	       "AND n.value IS NOT NULL AND t.abstract = 0 LIMIT 1";
}

std::string SecEdgarQ6ValueAppendSql(int write_seq) {
	return "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value, footnote) "
	       "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
	       "n.value + 0.001 * " +
	       std::to_string(1 + (write_seq % 1000)) +
	       ", n.footnote FROM num n "
	       "JOIN sub s ON n.adsh = s.adsh "
	       "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
	       "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
	       "AND n.value IS NOT NULL LIMIT 1";
}

std::string SecEdgarQ24ValueAppendSql(int write_seq) {
	return "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value, footnote) "
	       "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
	       "n.value + 0.001 * " +
	       std::to_string(1 + (write_seq % 1000)) +
	       ", n.footnote FROM num n "
	       "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh "
	       "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
	       "AND n.value IS NOT NULL AND p.adsh IS NULL LIMIT 1";
}

void ApplySecEdgarValueAppendWriter(duckdb::Connection &connection, const std::string &sql) {
	connection.BeginTransaction();
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		connection.Rollback();
		throw std::runtime_error("SEC EDGAR value_append writer failed: " + (result ? result->GetError() : "null"));
	}
	connection.Commit();
}

std::string SecEdgarValueAppendSql(const std::string &uom, int write_seq) {
	return "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value, footnote) "
	       "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
	       "n.value + 0.001 * " +
	       std::to_string(1 + (write_seq % 1000)) +
	       ", n.footnote FROM num n JOIN sub s ON n.adsh = s.adsh "
	       "WHERE n.uom = '" +
	       uom + "' AND s.fy = 2022 AND n.value IS NOT NULL LIMIT 1";
}

std::vector<int> ParseQueryListEnv(const char *env_name) {
	const char *raw = std::getenv(env_name);
	if (raw == nullptr || *raw == '\0') {
		return {};
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
		throw std::runtime_error(std::string(env_name) + " is empty");
	}
	return queries;
}

} // namespace

CrossAccelBenchmark DetectCrossAccelBenchmark() {
	const char *benchmark = std::getenv("ACCEL_AUX_BENCHMARK");
	if (benchmark != nullptr) {
		if (std::strcmp(benchmark, "sec_edgar") == 0) {
			return CrossAccelBenchmark::SecEdgar;
		}
		if (std::strcmp(benchmark, "tpch") == 0) {
			return CrossAccelBenchmark::Tpch;
		}
	}
	if (std::getenv("ACCEL_SEC_EDGAR_DB") != nullptr) {
		return CrossAccelBenchmark::SecEdgar;
	}
	return CrossAccelBenchmark::Tpch;
}

void EnableCrossAccelHybridMaintenance() {
	setenv("ACCEL_AUX_MAINTENANCE_SELECTOR", "hybrid", 1);
}

std::string CrossAccelDatabasePath() {
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		return GetBenchmarkDbPath("ACCEL_SEC_EDGAR_DB", kDefaultSecEdgarDb);
	}
	return GetBenchmarkDbPath("ACCEL_TPCH_DB", kDefaultTpchDb);
}

std::string CrossAccelMutableDatabasePath() {
	const char *work_db = std::getenv("ACCEL_CROSS_ACCEL_WORK_DB");
	if (work_db != nullptr && *work_db != '\0') {
		return work_db;
	}
	if (DetectCrossAccelBenchmark() != CrossAccelBenchmark::SecEdgar) {
		return CrossAccelDatabasePath();
	}

	const auto source = CrossAccelDatabasePath();
	static std::string copied_db;
	if (!copied_db.empty()) {
		return copied_db;
	}

	if (const char *work_dir = std::getenv("ACCEL_CROSS_ACCEL_WORK_DIR")) {
		copied_db = (std::filesystem::path(work_dir) / "cross_accel_work.duckdb").string();
	} else if (const char *build_dir = std::getenv("ACCEL_BUILD_DIR")) {
		copied_db = (std::filesystem::path(build_dir) / "cross_accel_work.duckdb").string();
	} else {
		copied_db = (std::filesystem::temp_directory_path() /
		             ("accel_cross_accel_sec_edgar_" + std::to_string(::getpid()) + ".duckdb"))
		                .string();
	}
	std::error_code error;
	std::filesystem::remove(copied_db, error);
	std::filesystem::copy_file(source, copied_db, std::filesystem::copy_options::overwrite_existing, error);
	if (error) {
		throw std::runtime_error("failed to copy SEC EDGAR database for cross-accel tests: " + error.message());
	}
	return copied_db;
}

std::string CrossAccelAuxSql(int query_num) {
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		return SecEdgarAuxSql(query_num);
	}
	return TpchAuxSql(query_num);
}

std::string CrossAccelReferenceSql(int query_num) {
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		return SecEdgarReferenceSql(query_num);
	}
	return TpchReferenceSql(query_num);
}

std::vector<int> CrossAccelAllQueries() {
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		return {1, 2, 3, 4, 6, 24};
	}
	return {1, 3, 6, 10, 12, 14};
}

std::vector<int> CrossAccelTestQueries() {
	auto queries = ParseQueryListEnv("ACCEL_CROSS_ACCEL_TEST_QUERIES");
	if (!queries.empty()) {
		return queries;
	}
	return CrossAccelAllQueries();
}

std::vector<int> CrossAccelQueriesIncludingTarget(int target_query) {
	auto queries = CrossAccelAllQueries();
	if (std::find(queries.begin(), queries.end(), target_query) == queries.end()) {
		queries.push_back(target_query);
	}
	return queries;
}

void BuildCrossAccelAux(duckdb::Connection &connection, int query_num) {
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		switch (query_num) {
		case 1:
			connection.Query("SELECT * FROM accel_sec_edgar_q1_aux_build()");
			return;
		case 2:
			connection.Query("SELECT * FROM accel_sec_edgar_q2_aux_build()");
			return;
		case 3:
			connection.Query("SELECT * FROM accel_sec_edgar_q3_aux_build()");
			return;
		case 4:
			connection.Query("SELECT * FROM accel_sec_edgar_q4_aux_build()");
			return;
		case 6:
			connection.Query("SELECT * FROM accel_sec_edgar_q6_aux_build()");
			return;
		case 24:
			connection.Query("SELECT * FROM accel_sec_edgar_q24_aux_build()");
			return;
		default:
			throw std::runtime_error("unsupported SEC EDGAR query for aux build: " + std::to_string(query_num));
		}
	}
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
		throw std::runtime_error("unsupported TPC-H query for aux build: " + std::to_string(query_num));
	}
}

std::string InvalidatingWriterSqlForQuery(int query_num, int bulk_row_limit, int write_seq) {
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		switch (query_num) {
		case 1:
			return SecEdgarQ1InvalidatingWriterSql(write_seq);
		case 2:
			return SecEdgarValueAppendSql("pure", write_seq);
		case 3:
			return SecEdgarValueAppendSql("USD", write_seq);
		case 4:
			return SecEdgarQ4ValueAppendSql(write_seq);
		case 6:
			return SecEdgarQ6ValueAppendSql(write_seq);
		case 24:
			return SecEdgarQ24ValueAppendSql(write_seq);
		default:
			throw std::runtime_error("unsupported SEC EDGAR query for invalidating writer SQL: " +
			                         std::to_string(query_num));
		}
	}

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
		return PointLineitemUpdateSql("l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'",
		                              bulk_row_limit);
	case 6:
		return PointLineitemUpdateSql("l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'",
		                              bulk_row_limit);
	default:
		throw std::runtime_error("unsupported TPC-H query for invalidating writer SQL: " + std::to_string(query_num));
	}
}

void ApplyInvalidatingWriterSql(duckdb::Connection &connection, int query_num) {
	static int write_seq = 0;
	write_seq++;
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		switch (query_num) {
		case 1:
			ApplySecEdgarQ1FilingAppendWriter(connection, write_seq);
			return;
		case 2:
			ApplySecEdgarNumValueInsertWriter(connection, "pure", write_seq);
			return;
		case 3:
			ApplySecEdgarNumValueInsertWriter(connection, "USD", write_seq);
			return;
		case 4:
			ApplySecEdgarValueAppendWriter(connection, SecEdgarQ4ValueAppendSql(write_seq));
			return;
		case 6:
			ApplySecEdgarValueAppendWriter(connection, SecEdgarQ6ValueAppendSql(write_seq));
			return;
		case 24:
			ApplySecEdgarValueAppendWriter(connection, SecEdgarQ24ValueAppendSql(write_seq));
			return;
		default:
			throw std::runtime_error("unsupported SEC EDGAR query for invalidating writer: " +
			                         std::to_string(query_num));
		}
	}
	const auto sql = InvalidatingWriterSqlForQuery(query_num, 1, write_seq);
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		const auto message = result ? result->GetError() : "null query result";
		throw std::runtime_error("invalidating writer failed for Q" + std::to_string(query_num) + ": " + message);
	}
}

void ApplyMultiConnectionStressWriter(duckdb::Connection &connection, int query_num) {
	static int write_seq = 0;
	write_seq++;
	if (DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		switch (query_num) {
		case 1:
			ApplySecEdgarQ1FilingAppendWriter(connection, write_seq);
			return;
		case 2:
			ApplySecEdgarNumValueInsertWriter(connection, "pure", write_seq);
			return;
		case 3:
			ApplySecEdgarNumValueInsertWriter(connection, "USD", write_seq);
			return;
		case 4:
			ApplySecEdgarValueAppendWriter(connection, SecEdgarQ4ValueAppendSql(write_seq));
			return;
		case 6:
			ApplySecEdgarValueAppendWriter(connection, SecEdgarQ6ValueAppendSql(write_seq));
			return;
		case 24:
			ApplySecEdgarValueAppendWriter(connection, SecEdgarQ24ValueAppendSql(write_seq));
			return;
		default:
			throw std::runtime_error("unsupported SEC EDGAR query for stress writer: " + std::to_string(query_num));
		}
	}
	const auto sql = InvalidatingWriterSqlForQuery(query_num, 1, write_seq);
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		const auto message = result ? result->GetError() : "null query result";
		throw std::runtime_error("stress writer failed for Q" + std::to_string(query_num) + ": " + message);
	}
}

void RequireCrossAccelQueryCorrectness(duckdb::Connection &connection, int query_num, const std::string &label) {
	const auto benchmark = DetectCrossAccelBenchmark();
	const auto reference = CanonicalizeSql(benchmark, query_num, CrossAccelReferenceSql(query_num));
	const auto aux = CanonicalizeSql(benchmark, query_num, CrossAccelAuxSql(query_num));
	RequireResultsEqual(connection, label, reference, aux);
}

void RequireCrossAccelCorrectness(duckdb::Connection &connection, const std::vector<int> &queries) {
	for (int query_num : queries) {
		RequireCrossAccelQueryCorrectness(connection, query_num, "Q" + std::to_string(query_num));
	}
}

} // namespace accelerator_codegen::test_support
