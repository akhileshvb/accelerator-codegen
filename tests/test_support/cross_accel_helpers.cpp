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
	case 2:
		return "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
		       "FROM part, supplier, partsupp, nation, region "
		       "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = 15 "
		       "AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
		       "AND r_name = 'EUROPE' "
		       "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
		       "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey "
		       "AND n_regionkey = r_regionkey AND r_name = 'EUROPE') "
		       "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100";
	case 3:
		return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
		       "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority "
		       "FROM customer, orders, lineitem "
		       "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey "
		       "AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15' "
		       "GROUP BY l_orderkey, o_orderdate, o_shippriority "
		       "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)";
	case 4:
		return "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
		       "FROM orders "
		       "WHERE o_orderdate >= DATE '1996-11-01' "
		       "AND o_orderdate < DATE '1996-11-01' + INTERVAL '3' MONTH "
		       "AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
		       "AND l_commitdate < l_receiptdate) "
		       "GROUP BY o_orderpriority ORDER BY o_orderpriority";
	case 5:
		return "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
		       "FROM customer, orders, lineitem, supplier, nation, region "
		       "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
		       "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
		       "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
		       "AND r_name = 'AMERICA' "
		       "AND o_orderdate >= DATE '1995-01-01' "
		       "AND o_orderdate < DATE '1995-01-01' + INTERVAL '1' year "
		       "GROUP BY n_name ORDER BY revenue DESC";
	case 7:
		return "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM ("
		       "SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, "
		       "EXTRACT(year FROM l_shipdate) AS l_year, "
		       "l_extendedprice * (1 - l_discount) AS volume "
		       "FROM supplier, lineitem, orders, customer, nation n1, nation n2 "
		       "WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
		       "AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey "
		       "AND c_nationkey = n2.n_nationkey "
		       "AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL') "
		       "OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM')) "
		       "AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') AS shipping "
		       "GROUP BY supp_nation, cust_nation, l_year "
		       "ORDER BY supp_nation, cust_nation, l_year";
	case 8:
		return "SELECT o_year, SUM(CASE WHEN nation = 'INDIA' THEN volume ELSE 0 END) / "
		       "SUM(volume) AS mkt_share FROM ("
		       "SELECT EXTRACT(year FROM o_orderdate) AS o_year, "
		       "l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation "
		       "FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
		       "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey "
		       "AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
		       "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey "
		       "AND r_name = 'ASIA' AND s_nationkey = n2.n_nationkey "
		       "AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
		       "AND p_type = 'PROMO BRUSHED BRASS') AS all_nations "
		       "GROUP BY o_year ORDER BY o_year";
	case 10:
		return "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')";
	case 11: {
		const std::string from_clause =
		    "FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND "
		    "s_nationkey = n_nationkey AND n_name = 'ROMANIA'";
		return "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value " + from_clause +
		       " GROUP BY ps_partkey HAVING SUM(ps_supplycost * ps_availqty) > (SELECT "
		       "SUM(ps_supplycost * ps_availqty) * 0.0001 " + from_clause + ") ORDER BY value DESC, ps_partkey";
	}
	case 12:
		return "SELECT * FROM accel_tpch_q12('MAIL', 'SHIP', DATE '1995-01-01')";
	case 13:
		return "SELECT c_count, COUNT(*) AS custdist FROM ("
		       "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
		       "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
		       "'%unusual%accounts%' GROUP BY c_custkey) AS c_orders "
		       "GROUP BY c_count ORDER BY custdist DESC, c_count DESC";
	case 15:
		return "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue "
		       "FROM supplier, ("
		       "SELECT l_suppkey AS supplier_no, "
		       "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
		       "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
		       "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
		       "GROUP BY l_suppkey) revenue0 "
		       "WHERE s_suppkey = supplier_no "
		       "AND total_revenue = (SELECT MAX(total_revenue) FROM ("
		       "SELECT l_suppkey AS supplier_no, "
		       "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
		       "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
		       "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
		       "GROUP BY l_suppkey) revenue1) ORDER BY s_suppkey";
	case 19:
		return "SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue "
		       "FROM lineitem, part "
		       "WHERE (p_partkey = l_partkey AND p_brand = 'Brand#31' "
		       "AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
		       "AND l_quantity >= 4 AND l_quantity <= 4 + 10 "
		       "AND p_size BETWEEN 1 AND 5 "
		       "AND l_shipmode IN ('AIR', 'AIR REG') "
		       "AND l_shipinstruct = 'DELIVER IN PERSON') "
		       "OR (p_partkey = l_partkey AND p_brand = 'Brand#12' "
		       "AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
		       "AND l_quantity >= 15 AND l_quantity <= 15 + 10 "
		       "AND p_size BETWEEN 1 AND 10 "
		       "AND l_shipmode IN ('AIR', 'AIR REG') "
		       "AND l_shipinstruct = 'DELIVER IN PERSON') "
		       "OR (p_partkey = l_partkey AND p_brand = 'Brand#51' "
		       "AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
		       "AND l_quantity >= 23 AND l_quantity <= 23 + 10 "
		       "AND p_size BETWEEN 1 AND 15 "
		       "AND l_shipmode IN ('AIR', 'AIR REG') "
		       "AND l_shipinstruct = 'DELIVER IN PERSON')";
	case 20:
		return "SELECT s_name, s_address "
		       "FROM supplier, nation "
		       "WHERE s_suppkey IN ("
		       "SELECT ps_suppkey FROM partsupp, ("
		       "SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
		       "FROM lineitem WHERE l_shipdate >= DATE '1997-01-01' "
		       "AND l_shipdate < DATE '1997-01-01' + INTERVAL '1' year "
		       "GROUP BY l_partkey, l_suppkey) agg_lineitem "
		       "WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
		       "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'gainsboro%') "
		       "AND ps_availqty > agg_quantity) "
		       "AND s_nationkey = n_nationkey AND n_name = 'RUSSIA' "
		       "ORDER BY s_name";
	case 14:
		return "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) / "
		       "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
		       "FROM lineitem, part "
		       "WHERE l_partkey = p_partkey "
		       "AND l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month";
	case 16:
		return "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
		       "FROM partsupp, part "
		       "WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#13' "
		       "AND p_type NOT LIKE 'SMALL BRUSHED%' "
		       "AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33) "
		       "AND ps_suppkey NOT IN ("
		       "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
		       "GROUP BY p_brand, p_type, p_size "
		       "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size";
	case 9:
		return "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
		       "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
		       "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
		       "FROM part, supplier, lineitem, partsupp, orders, nation "
		       "WHERE s_suppkey = l_suppkey "
		       "AND ps_suppkey = l_suppkey "
		       "AND ps_partkey = l_partkey "
		       "AND p_partkey = l_partkey "
		       "AND o_orderkey = l_orderkey "
		       "AND s_nationkey = n_nationkey "
		       "AND p_name LIKE '%green%'"
		       ") AS profit "
		       "GROUP BY nation, o_year "
		       "ORDER BY nation, o_year DESC";
	case 17:
		return "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
		       "FROM lineitem, part, "
		       "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
		       "FROM lineitem GROUP BY l_partkey) part_agg "
		       "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
		       "AND p_brand = 'Brand#34' AND p_container = 'LG CASE' "
		       "AND l_quantity < avg_quantity";
	case 6:
		return "SELECT revenue FROM accel_tpch_q6(DATE '1995-01-01', 0.03, 25)";
	case 18:
		return "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, "
		       "SUM(l_quantity) AS sum_qty "
		       "FROM customer, orders, lineitem "
		       "WHERE o_orderkey IN ("
		       "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
		       "HAVING SUM(l_quantity) > 312) "
		       "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
		       "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
		       "ORDER BY o_totalprice DESC, o_orderdate ASC";
	case 22:
		return "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
		       "FROM (SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
		       "FROM customer "
		       "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN "
		       "('13', '30', '99', '98', '97', '96', '95') "
		       "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer "
		       "WHERE c_acctbal > 0.00 AND SUBSTRING(c_phone FROM 1 FOR 2) IN "
		       "('13', '30', '99', '98', '97', '96', '95')) "
		       "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) AS custsale "
		       "GROUP BY cntrycode ORDER BY cntrycode";
	case 21:
		return "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
		       "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
		       "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
		       "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
		       "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
		       "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
		       "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
		       "n_name = 'SAUDI ARABIA' GROUP BY s_name ORDER BY numwait DESC, s_name";
	default:
		throw std::runtime_error("unsupported TPC-H query for reference SQL: " + std::to_string(query_num));
	}
}

std::string TpchAuxSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT * FROM accel_tpch_q1_aux(90)";
	case 2:
		return "SELECT * FROM accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')";
	case 3:
		return "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')";
	case 4:
		return "SELECT * FROM accel_tpch_q4_aux(DATE '1996-11-01')";
	case 5:
		return "SELECT * FROM accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')";
	case 7:
		return "SELECT * FROM accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')";
	case 8:
		return "SELECT * FROM accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')";
	case 10:
		return "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')";
	case 11:
		return "SELECT * FROM accel_tpch_q11_aux('ROMANIA', 0.0001)";
	case 12:
		return "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')";
	case 13:
		return "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')";
	case 15:
		return "SELECT * FROM accel_tpch_q15_aux(DATE '1994-11-01')";
	case 19:
		return "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)";
	case 20:
		return "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')";
	case 14:
		return "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')";
	case 16:
		return "SELECT * FROM accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)";
	case 6:
		return "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)";
	case 9:
		return "SELECT * FROM accel_tpch_q9_aux('green')";
	case 18:
		return "SELECT * FROM accel_tpch_q18_aux(312)";
	case 17:
		return "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LG CASE')";
	case 22:
		return "SELECT * FROM accel_tpch_q22_aux('13', '30', '99', '98', '97', '96', '95')";
	case 21:
		return "SELECT * FROM accel_tpch_q21_aux('SAUDI ARABIA')";
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
	return {1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 14, 17, 18, 21};
}

std::vector<int> CrossAccelTestQueries() {
	auto queries = ParseQueryListEnv("ACCEL_CROSS_ACCEL_TEST_QUERIES");
	if (!queries.empty()) {
		return queries;
	}
	return CrossAccelAllQueries();
}

void BuildCrossAccelAux(duckdb::Connection &connection, int query_num) {
	auto result = connection.Query(CrossAccelAuxSql(query_num));
	if (!result || result->HasError()) {
		const auto message = result ? result->GetError() : "null query result";
		throw std::runtime_error("aux warmup failed for Q" + std::to_string(query_num) + ": " + message);
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
	case 2:
		return "UPDATE partsupp SET ps_supplycost = ps_supplycost * 1.0000001 "
		       "WHERE (ps_partkey, ps_suppkey) IN (SELECT ps_partkey, ps_suppkey FROM partsupp "
		       "ORDER BY ps_partkey, ps_suppkey LIMIT " + std::to_string(bulk_row_limit) + ")";
	case 1:
		return PointLineitemUpdateSql("l_shipdate = DATE '1995-06-01'", bulk_row_limit);
	case 3:
		return PointLineitemUpdateSql(
		    "l_orderkey IN (SELECT o_orderkey FROM customer JOIN orders ON c_custkey = o_custkey "
		    "WHERE c_mktsegment = 'BUILDING' AND o_orderdate < DATE '1995-03-15') "
		    "AND l_shipdate > DATE '1995-03-15'",
		    bulk_row_limit);
	case 4:
		return PointLineitemUpdateSql(
		    "l_orderkey IN (SELECT o_orderkey FROM orders "
		    "WHERE o_orderdate >= DATE '1996-11-01' AND o_orderdate < DATE '1997-02-01') "
		    "AND l_commitdate < l_receiptdate",
		    bulk_row_limit);
	case 5:
		return PointLineitemUpdateSql(
		    "l_orderkey IN (SELECT o_orderkey FROM orders "
		    "WHERE o_orderdate >= DATE '1995-01-01' "
		    "AND o_orderdate < DATE '1996-01-01')",
		    bulk_row_limit);
	case 7:
		return PointLineitemUpdateSql(
		    "l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'",
		    bulk_row_limit);
	case 8:
		return PointLineitemUpdateSql(
		    "l_orderkey IN (SELECT o_orderkey FROM orders "
		    "WHERE o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31')",
		    bulk_row_limit);
	case 10:
		return PointLineitemUpdateSql(
		    "l_returnflag = 'R' AND l_orderkey IN ("
		    "SELECT o_orderkey FROM orders "
		    "WHERE o_orderdate >= DATE '1993-06-01' AND o_orderdate < DATE '1993-07-01')",
		    bulk_row_limit);
	case 11:
		return "UPDATE partsupp SET ps_supplycost = ps_supplycost * 1.0000001 "
		       "WHERE (ps_partkey, ps_suppkey) IN ("
		       "SELECT ps.ps_partkey, ps.ps_suppkey FROM partsupp ps, supplier s, nation n "
		       "WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey "
		       "AND n.n_name = 'ROMANIA' "
		       "ORDER BY ps.ps_partkey, ps.ps_suppkey LIMIT " +
		       std::to_string(bulk_row_limit) + ")";
	case 12:
		return PointLineitemUpdateSql(
		    "l_receiptdate >= DATE '1995-01-01' AND l_receiptdate < DATE '1995-02-01' "
		    "AND l_shipmode IN ('MAIL', 'SHIP')",
		    bulk_row_limit);
	case 13:
		return "UPDATE orders SET o_comment = o_comment || ' ' WHERE o_orderkey IN ("
		       "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " +
		       std::to_string(bulk_row_limit) + ")";
	case 15:
		return PointLineitemUpdateSql(
		    "l_shipdate >= DATE '1994-11-01' AND l_shipdate < DATE '1995-02-01'",
		    bulk_row_limit);
	case 19:
		return PointLineitemUpdateSql(
		    "l_shipmode IN ('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON'",
		    bulk_row_limit);
	case 20:
		return PointLineitemUpdateSql(
		    "l_shipdate >= DATE '1997-01-01' AND l_shipdate < DATE '1998-01-01'",
		    bulk_row_limit);
	case 14:
		return PointLineitemUpdateSql("l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'",
		                              bulk_row_limit);
	case 16:
		// Q16 watches part / partsupp / supplier (no lineitem). Touch a watched
		// supplier comment so aux must invalidate; trailing space does not
		// create new '%Customer%Complaints%' matches.
		return "UPDATE supplier SET s_comment = s_comment || ' ' "
		       "WHERE s_suppkey IN ("
		       "SELECT s_suppkey FROM supplier ORDER BY s_suppkey LIMIT " +
		       std::to_string(bulk_row_limit) + ")";
	case 6:
		return PointLineitemUpdateSql("l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'",
		                              bulk_row_limit);
	case 9:
		return PointLineitemUpdateSql(
		    "l_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE '%green%')",
		    bulk_row_limit);
	case 18:
		return PointLineitemUpdateSql(
		    "l_orderkey IN (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
		    "HAVING SUM(l_quantity) > 312)",
		    bulk_row_limit);
	case 17:
		return PointLineitemUpdateSql(
		    "l_partkey IN (SELECT p_partkey FROM part WHERE p_brand = 'Brand#34' "
		    "AND p_container = 'LG CASE')",
		    bulk_row_limit);
	case 22:
		return "UPDATE orders SET o_custkey = o_custkey + 1 WHERE o_orderkey "
		       "IN (SELECT o_orderkey FROM orders ORDER BY o_orderkey "
		       "LIMIT " + std::to_string(bulk_row_limit) + ")";
	case 21:
		return PointLineitemUpdateSql(
		    "l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F') "
		    "AND l_receiptdate > l_commitdate",
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
