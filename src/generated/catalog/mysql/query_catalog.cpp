// GENERATED BACKEND — MySQL query catalog (TPC-H Q1–22).
#include "catalog/mysql/query_catalog.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const std::vector<MysqlSharedQueryCatalogEntry> kMysqlQueries = {
    {
        1,
        "tpch",
        "tpch_q1",
        /* base_sql */
        "SELECT l_returnflag, l_linestatus, "
        "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
        "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
        "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
        "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
        "avg(l_discount) AS avg_disc, count(*) AS count_order "
        "FROM lineitem "
        "WHERE l_shipdate <= DATE_SUB(DATE '1998-12-01', INTERVAL 90 DAY) "
        "GROUP BY l_returnflag, l_linestatus "
        "ORDER BY l_returnflag, l_linestatus",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q1_aux(90)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q1_aux(90)",
    },
    {
        2,
        "tpch",
        "tpch_q2",
        /* base_sql */
        "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
        "FROM part, supplier, partsupp, nation, region "
        "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = 15 "
        "AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
        "AND r_name = 'EUROPE' "
        "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
        "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey "
        "AND n_regionkey = r_regionkey AND r_name = 'EUROPE') "
        "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
    },
    {
        3,
        "tpch",
        "tpch_q3",
        /* base_sql */
        "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
        "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
        "o_orderdate, o_shippriority "
        "FROM customer, orders, lineitem "
        "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND "
        "l_orderkey = o_orderkey "
        "AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15' "
        "GROUP BY l_orderkey, o_orderdate, o_shippriority "
        "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10) AS q3_orders",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
    },
    {
        4,
        "tpch",
        "tpch_q4",
        /* base_sql */
        "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
        "FROM orders "
        "WHERE o_orderdate >= DATE '1993-07-01' "
        "AND o_orderdate < DATE_ADD(DATE '1993-07-01', INTERVAL 3 MONTH) "
        "AND EXISTS ("
        "SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
        "AND l_commitdate < l_receiptdate) "
        "GROUP BY o_orderpriority ORDER BY o_orderpriority",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q4_aux(DATE '1993-07-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q4_aux(DATE '1993-07-01')",
    },
    {
        5,
        "tpch",
        "tpch_q5",
        /* base_sql */
        "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
        "FROM customer, orders, lineitem, supplier, nation, region "
        "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
        "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
        "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
        "AND r_name = 'AMERICA' "
        "AND o_orderdate >= DATE '1995-01-01' "
        "AND o_orderdate < DATE_ADD(DATE '1995-01-01', INTERVAL 1 YEAR) "
        "GROUP BY n_name ORDER BY revenue DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
    },
    {
        6,
        "tpch",
        "tpch_q6",
        /* base_sql */
        "SELECT SUM(l_extendedprice * l_discount) AS revenue "
        "FROM lineitem "
        "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
        "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
    },
    {
        7,
        "tpch",
        "tpch_q7",
        /* base_sql */
        "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM ("
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
        "ORDER BY supp_nation, cust_nation, l_year",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
    },
    {
        8,
        "tpch",
        "tpch_q8",
        /* base_sql */
        "SELECT o_year, SUM(CASE WHEN nation = 'INDIA' THEN volume ELSE 0 END) / "
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
        "GROUP BY o_year ORDER BY o_year",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
    },
    {
        9,
        "tpch",
        "tpch_q9",
        /* base_sql */
        "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
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
        "ORDER BY nation, o_year DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q9_aux('green')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q9_aux('green')",
    },
    {
        10,
        "tpch",
        "tpch_q10",
        /* base_sql */
        "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS "
        "revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
        "FROM customer JOIN orders ON c_custkey = o_custkey "
        "JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON "
        "c_nationkey = n_nationkey "
        "WHERE o_orderdate >= DATE '1993-06-01' "
        "AND o_orderdate < DATE_ADD(DATE '1993-06-01', INTERVAL 3 MONTH) AND "
        "l_returnflag = 'R' "
        "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, "
        "c_comment ORDER BY revenue DESC, c_custkey LIMIT 20",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q10_aux(DATE '1993-06-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q10_aux(DATE '1993-06-01')",
    },
    {
        11,
        "tpch",
        "tpch_q11",
        /* base_sql */
        "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value "
        "FROM partsupp, supplier, nation "
        "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
        "AND n_name = 'ROMANIA' "
        "GROUP BY ps_partkey "
        "HAVING SUM(ps_supplycost * ps_availqty) > ("
        "SELECT SUM(ps_supplycost * ps_availqty) * 0.0001 "
        "FROM partsupp, supplier, nation "
        "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
        "AND n_name = 'ROMANIA') "
        "ORDER BY value DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q11_aux('ROMANIA', 0.0001)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q11_aux('ROMANIA', 0.0001)",
    },
    {
        12,
        "tpch",
        "tpch_q12",
        /* base_sql */
        "SELECT l_shipmode, "
        "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' "
        "THEN 1 ELSE 0 END) AS high_line_count, "
        "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> "
        "'2-HIGH' THEN 1 ELSE 0 END) AS low_line_count "
        "FROM orders, lineitem "
        "WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') "
        "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate "
        "AND l_receiptdate >= DATE '1995-01-01' "
        "AND l_receiptdate < DATE_ADD(DATE '1995-01-01', INTERVAL 1 YEAR) "
        "GROUP BY l_shipmode ORDER BY l_shipmode",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
    },
    {
        13,
        "tpch",
        "tpch_q13",
        /* base_sql */
        "SELECT c_count, COUNT(*) AS custdist FROM ("
        "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
        "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
        "'%unusual%accounts%' GROUP BY c_custkey) AS c_orders "
        "GROUP BY c_count ORDER BY custdist DESC, c_count DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q13_aux('%unusual%accounts%')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q13_aux('%unusual%accounts%')",
    },
    {
        14,
        "tpch",
        "tpch_q14",
        /* base_sql */
        "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
        "(1 - l_discount) ELSE 0 END) / "
        "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
        "FROM lineitem, part "
        "WHERE l_partkey = p_partkey "
        "AND l_shipdate >= DATE '1995-06-01' AND l_shipdate < "
        "DATE_ADD(DATE '1995-06-01', INTERVAL 1 MONTH)",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q14_aux(DATE '1995-06-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q14_aux(DATE '1995-06-01')",
    },
    {
        15,
        "tpch",
        "tpch_q15",
        /* base_sql */
        "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue "
        "FROM supplier, ("
        "SELECT l_suppkey AS supplier_no, "
        "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
        "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
        "AND l_shipdate < DATE_ADD(DATE '1994-11-01', INTERVAL 3 MONTH) "
        "GROUP BY l_suppkey) revenue0 "
        "WHERE s_suppkey = supplier_no "
        "AND total_revenue = (SELECT MAX(total_revenue) FROM ("
        "SELECT l_suppkey AS supplier_no, "
        "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
        "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
        "AND l_shipdate < DATE_ADD(DATE '1994-11-01', INTERVAL 3 MONTH) "
        "GROUP BY l_suppkey) revenue1) ORDER BY s_suppkey",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q15_aux(DATE '1994-11-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q15_aux(DATE '1994-11-01')",
    },
    {
        16,
        "tpch",
        "tpch_q16",
        /* base_sql */
        "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
        "FROM partsupp, part "
        "WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#13' "
        "AND p_type NOT LIKE 'SMALL BRUSHED%' "
        "AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33) "
        "AND ps_suppkey NOT IN ("
        "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
        "GROUP BY p_brand, p_type, p_size "
        "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
    },
    {
        17,
        "tpch",
        "tpch_q17",
        /* base_sql */
        "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
        "FROM lineitem, part, "
        "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
        "FROM lineitem GROUP BY l_partkey) part_agg "
        "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
        "AND p_brand = 'Brand#34' AND p_container = 'LG CASE' "
        "AND l_quantity < avg_quantity",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q17_aux('Brand#34', 'LG CASE')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q17_aux('Brand#34', 'LG CASE')",
    },
    {
        18,
        "tpch",
        "tpch_q18",
        /* base_sql */
        "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, "
        "SUM(l_quantity) AS sum_qty "
        "FROM customer, orders, lineitem "
        "WHERE o_orderkey IN ("
        "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
        "HAVING SUM(l_quantity) > 312) "
        "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
        "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
        "ORDER BY o_totalprice DESC, o_orderdate ASC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q18_aux(312)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q18_aux(312)",
    },
    {
        19,
        "tpch",
        "tpch_q19",
        /* base_sql */
        "SELECT COALESCE(SUM(l_extendedprice * (1 - l_discount)), 0) AS revenue "
        "FROM lineitem, part "
        "WHERE (p_partkey = l_partkey "
        "AND p_brand = 'Brand#31' "
        "AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
        "AND l_quantity >= 4 AND l_quantity <= 4 + 10 "
        "AND p_size BETWEEN 1 AND 5 "
        "AND l_shipmode IN ('AIR', 'AIR REG') "
        "AND l_shipinstruct = 'DELIVER IN PERSON') "
        "OR (p_partkey = l_partkey "
        "AND p_brand = 'Brand#12' "
        "AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
        "AND l_quantity >= 15 AND l_quantity <= 15 + 10 "
        "AND p_size BETWEEN 1 AND 10 "
        "AND l_shipmode IN ('AIR', 'AIR REG') "
        "AND l_shipinstruct = 'DELIVER IN PERSON') "
        "OR (p_partkey = l_partkey "
        "AND p_brand = 'Brand#51' "
        "AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
        "AND l_quantity >= 23 AND l_quantity <= 23 + 10 "
        "AND p_size BETWEEN 1 AND 15 "
        "AND l_shipmode IN ('AIR', 'AIR REG') "
        "AND l_shipinstruct = 'DELIVER IN PERSON')",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
    },
    {
        20,
        "tpch",
        "tpch_q20",
        /* base_sql */
        "SELECT s_name, s_address "
        "FROM supplier, nation "
        "WHERE s_suppkey IN ("
        "SELECT ps_suppkey FROM partsupp, ("
        "SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
        "FROM lineitem WHERE l_shipdate >= DATE '1997-01-01' "
        "AND l_shipdate < DATE_ADD(DATE '1997-01-01', INTERVAL 1 YEAR) "
        "GROUP BY l_partkey, l_suppkey) agg_lineitem "
        "WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
        "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'gainsboro%') "
        "AND ps_availqty > agg_quantity) "
        "AND s_nationkey = n_nationkey AND n_name = 'RUSSIA' "
        "ORDER BY s_name",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
    },
    {
        21,
        "tpch",
        "tpch_q21",
        /* base_sql */
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = 'SAUDI ARABIA' GROUP BY s_name ORDER BY numwait DESC, s_name",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q21_aux('SAUDI ARABIA')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q21_aux('SAUDI ARABIA')",
    },
    {
        22,
        "tpch",
        "tpch_q22",
        /* base_sql */
        "WITH custsale AS ("
        "SELECT SUBSTRING(c_phone, 1, 2) AS cntrycode, c_acctbal "
        "FROM customer "
        "WHERE SUBSTRING(c_phone, 1, 2) IN ('30', '19', '14', '28', '11', '22', '32') "
        "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
        "AND SUBSTRING(c_phone, 1, 2) IN ('30', '19', '14', '28', '11', '22', '32')) "
        "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
        "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
        "FROM custsale GROUP BY cntrycode ORDER BY cntrycode",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
    },
};

const MysqlSharedQueryCatalogEntry *FindEntry(const std::string &workload_name,
                                                  int query_id) {
  for (const auto &e : kMysqlQueries) {
    if (e.query_id == query_id && workload_name == e.workload_name) {
      return &e;
    }
  }
  return nullptr;
}

} // namespace

const MysqlSharedQueryCatalogEntry &LookupMysqlQuery(const std::string &workload_name,
                                                             int query_id) {
  const auto *entry = FindEntry(workload_name, query_id);
  if (!entry) {
    throw std::runtime_error(
        "mysql query not found: " + workload_name + " Q" + std::to_string(query_id));
  }
  return *entry;
}

std::vector<int> MysqlQueriesForWorkload(const std::string &workload_name) {
  std::vector<int> ids;
  for (const auto &e : kMysqlQueries) {
    if (workload_name == e.workload_name) ids.push_back(e.query_id);
  }
  return ids;
}

} // namespace accelerator_codegen::aux_state::engine::mysql
