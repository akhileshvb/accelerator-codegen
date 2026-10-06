#pragma once

// Per-query parameter templates and parameter generators for the TPC-H
// parameter-generality differential (test_postgres_tpch_params.cpp).
//
// TEST-SIDE ONLY: this file is allowed to know TPC-H domains.
//
// For every query N in 1..22:
//   * `ref`  is the reference SQL (TPC-H spec shape; column order and
//            ORDER BY / LIMIT follow the catalog base_sql) with {i} slots;
//   * `call` is the accelerator call accel_tpch_qN_aux({0}, {1}, ...), slot
//            order = the SQL function signature in
//            src/generated/tpch/qN/postgres/extension/functions.cpp;
//   * `types` has one char per slot: d=date s=text i=integer b=bigint n=numeric.
//
// Parameter classes:
//   a  spec-domain random draws (what qgen would produce)
//   b  off-spec but valid SQL values (random)
//   c  adversarial edge cases (deterministic lists)
//
// A parameter value is a string; kNull means SQL NULL (rendered NULL::<type>).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace accel::postgres::tpch_params {

using Vec = std::vector<std::string>;
inline const std::string kNull = "\x01NULL";

struct QueryTemplate {
  int q;
  const char *types;
  std::string ref;
  std::string call;
  bool limit; // ORDER BY ... LIMIT: ties at the cut may legitimately differ
};

inline const char *NullType(char t) {
  switch (t) {
  case 'd': return "date";
  case 's': return "text";
  case 'i': return "integer";
  case 'b': return "bigint";
  default: return "numeric";
  }
}

inline std::string SqlQuote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "''";
    else out += c;
  }
  return out + "'";
}

inline std::string RenderOne(char type, const std::string &v) {
  if (v == kNull) return std::string("NULL::") + NullType(type);
  switch (type) {
  case 'd': return "DATE " + SqlQuote(v);
  case 's': return SqlQuote(v);
  default: return v; // i, n: numeric literal as given (may be negative)
  }
}

// Replace {i} slots. Negative numbers are parenthesised to stay unambiguous.
inline std::string Render(const std::string &tmpl, const char *types, const Vec &p) {
  std::string out;
  for (std::size_t i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] == '{' && i + 2 < tmpl.size() + 0 && tmpl[i + 2] == '}') {
      const int idx = tmpl[i + 1] - '0';
      std::string r = RenderOne(types[idx], p.at(static_cast<std::size_t>(idx)));
      if ((types[idx] == 'i' || types[idx] == 'b' || types[idx] == 'n') && !r.empty() && r[0] == '-') r = "(" + r + ")";
      out += r;
      i += 2;
    } else {
      out += tmpl[i];
    }
  }
  return out;
}

inline std::string Describe(const char *types, const Vec &p) {
  std::string out = "(";
  for (std::size_t i = 0; i < p.size(); ++i) {
    if (i) out += ", ";
    out += p[i] == kNull ? std::string("NULL") : SqlQuote(p[i]);
  }
  (void)types;
  return out + ")";
}

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------

inline const std::vector<QueryTemplate> &Templates() {
  static const std::vector<QueryTemplate> t = {
      {1, "i",
       "SELECT l_returnflag, l_linestatus, "
       "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
       "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
       "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
       "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
       "avg(l_discount) AS avg_disc, count(*) AS count_order "
       "FROM lineitem "
       "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '1' DAY * {0} "
       "GROUP BY l_returnflag, l_linestatus "
       "ORDER BY l_returnflag, l_linestatus",
       "SELECT * FROM accel_tpch_q1_aux({0})", false},
      {2, "iss",
       "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
       "FROM part, supplier, partsupp, nation, region "
       "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = {0} "
       "AND p_type LIKE {1} AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
       "AND r_name = {2} "
       "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
       "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey "
       "AND n_regionkey = r_regionkey AND r_name = {2}) "
       "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100",
       "SELECT * FROM accel_tpch_q2_aux({0}, {1}, {2})", true},
      {3, "sd",
       "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
       "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
       "o_orderdate, o_shippriority "
       "FROM customer, orders, lineitem "
       "WHERE c_mktsegment = {0} AND c_custkey = o_custkey AND "
       "l_orderkey = o_orderkey "
       "AND o_orderdate < {1} AND l_shipdate > {1} "
       "GROUP BY l_orderkey, o_orderdate, o_shippriority "
       "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10) t",
       "SELECT * FROM accel_tpch_q3_aux({0}, {1})", true},
      {4, "d",
       "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
       "FROM orders "
       "WHERE o_orderdate >= {0} "
       "AND o_orderdate < {0} + INTERVAL '3' month "
       "AND EXISTS ("
       "SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
       "AND l_commitdate < l_receiptdate) "
       "GROUP BY o_orderpriority ORDER BY o_orderpriority",
       "SELECT * FROM accel_tpch_q4_aux({0})", false},
      {5, "sd",
       "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
       "FROM customer, orders, lineitem, supplier, nation, region "
       "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
       "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
       "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
       "AND r_name = {0} "
       "AND o_orderdate >= {1} "
       "AND o_orderdate < {1} + INTERVAL '1' year "
       "GROUP BY n_name ORDER BY revenue DESC",
       "SELECT * FROM accel_tpch_q5_aux({0}, {1})", false},
      {6, "dnn",
       "SELECT SUM(l_extendedprice * l_discount) AS revenue "
       "FROM lineitem "
       "WHERE l_shipdate >= {0} AND l_shipdate < {0} + INTERVAL '1' year "
       "AND l_discount BETWEEN {1} - 0.01 AND {1} + 0.01 AND l_quantity < {2}",
       "SELECT * FROM accel_tpch_q6_aux({0}, {1}, {2})", false},
      {7, "ss",
       "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM ("
       "SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, "
       "EXTRACT(year FROM l_shipdate) AS l_year, "
       "l_extendedprice * (1 - l_discount) AS volume "
       "FROM supplier, lineitem, orders, customer, nation n1, nation n2 "
       "WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
       "AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey "
       "AND c_nationkey = n2.n_nationkey "
       "AND ((n1.n_name = {0} AND n2.n_name = {1}) "
       "OR (n1.n_name = {1} AND n2.n_name = {0})) "
       "AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') AS shipping "
       "GROUP BY supp_nation, cust_nation, l_year "
       "ORDER BY supp_nation, cust_nation, l_year",
       "SELECT * FROM accel_tpch_q7_aux({0}, {1})", false},
      {8, "sss",
       "SELECT o_year, SUM(CASE WHEN nation = {0} THEN volume ELSE 0 END) / "
       "SUM(volume) AS mkt_share FROM ("
       "SELECT EXTRACT(year FROM o_orderdate) AS o_year, "
       "l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation "
       "FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
       "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey "
       "AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
       "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey "
       "AND r_name = {1} AND s_nationkey = n2.n_nationkey "
       "AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
       "AND p_type = {2}) AS all_nations "
       "GROUP BY o_year ORDER BY o_year",
       "SELECT * FROM accel_tpch_q8_aux({0}, {1}, {2})", false},
      {9, "s",
       "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
       "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
       "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
       "FROM part, supplier, lineitem, partsupp, orders, nation "
       "WHERE s_suppkey = l_suppkey AND ps_suppkey = l_suppkey "
       "AND ps_partkey = l_partkey AND p_partkey = l_partkey "
       "AND o_orderkey = l_orderkey AND s_nationkey = n_nationkey "
       "AND p_name LIKE '%' || {0} || '%'"
       ") AS profit "
       "GROUP BY nation, o_year "
       "ORDER BY nation, o_year DESC",
       "SELECT * FROM accel_tpch_q9_aux({0})", false},
      {10, "d",
       "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS "
       "revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
       "FROM customer JOIN orders ON c_custkey = o_custkey "
       "JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON "
       "c_nationkey = n_nationkey "
       "WHERE o_orderdate >= {0} "
       "AND o_orderdate < {0} + INTERVAL '3' MONTH AND "
       "l_returnflag = 'R' "
       "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, "
       "c_comment ORDER BY revenue DESC, c_custkey LIMIT 20",
       "SELECT * FROM accel_tpch_q10_aux({0})", true},
      {11, "sn",
       "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value "
       "FROM partsupp, supplier, nation "
       "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
       "AND n_name = {0} "
       "GROUP BY ps_partkey "
       "HAVING SUM(ps_supplycost * ps_availqty) > ("
       "SELECT SUM(ps_supplycost * ps_availqty) * {1} "
       "FROM partsupp, supplier, nation "
       "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
       "AND n_name = {0}) "
       "ORDER BY value DESC, ps_partkey",
       "SELECT * FROM accel_tpch_q11_aux({0}, {1})", false},
      {12, "ssd",
       "SELECT l_shipmode, "
       "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' "
       "THEN 1 ELSE 0 END) AS high_line_count, "
       "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> "
       "'2-HIGH' THEN 1 ELSE 0 END) AS low_line_count "
       "FROM orders, lineitem "
       "WHERE o_orderkey = l_orderkey AND l_shipmode IN ({0}, {1}) "
       "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate "
       "AND l_receiptdate >= {2} "
       "AND l_receiptdate < {2} + INTERVAL '1' year "
       "GROUP BY l_shipmode ORDER BY l_shipmode",
       "SELECT * FROM accel_tpch_q12_aux({0}, {1}, {2})", false},
      {13, "s",
       "SELECT c_count, COUNT(*) AS custdist FROM ("
       "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
       "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
       "{0} GROUP BY c_custkey) AS c_orders "
       "GROUP BY c_count ORDER BY custdist DESC, c_count DESC",
       "SELECT * FROM accel_tpch_q13_aux({0})", false},
      {14, "d",
       "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
       "(1 - l_discount) ELSE 0 END) / "
       "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
       "FROM lineitem, part "
       "WHERE l_partkey = p_partkey "
       "AND l_shipdate >= {0} AND l_shipdate < {0} + "
       "INTERVAL '1' month",
       "SELECT * FROM accel_tpch_q14_aux({0})", false},
      {15, "d",
       "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue "
       "FROM supplier, ("
       "SELECT l_suppkey AS supplier_no, "
       "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
       "FROM lineitem WHERE l_shipdate >= {0} "
       "AND l_shipdate < {0} + INTERVAL '3' MONTH "
       "GROUP BY l_suppkey) revenue0 "
       "WHERE s_suppkey = supplier_no "
       "AND total_revenue = (SELECT MAX(total_revenue) FROM ("
       "SELECT l_suppkey AS supplier_no, "
       "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
       "FROM lineitem WHERE l_shipdate >= {0} "
       "AND l_shipdate < {0} + INTERVAL '3' MONTH "
       "GROUP BY l_suppkey) revenue1) ORDER BY s_suppkey",
       "SELECT * FROM accel_tpch_q15_aux({0})", false},
      {16, "ssiiiiiiii",
       "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
       "FROM partsupp, part "
       "WHERE p_partkey = ps_partkey AND p_brand <> {0} "
       "AND p_type NOT LIKE {1} "
       "AND p_size IN ({2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}) "
       "AND ps_suppkey NOT IN ("
       "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
       "GROUP BY p_brand, p_type, p_size "
       "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size",
       "SELECT * FROM accel_tpch_q16_aux({0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9})", false},
      {17, "ss",
       "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
       "FROM lineitem, part, "
       "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
       "FROM lineitem GROUP BY l_partkey) part_agg "
       "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
       "AND p_brand = {0} AND p_container = {1} "
       "AND l_quantity < avg_quantity",
       "SELECT * FROM accel_tpch_q17_aux({0}, {1})", false},
      {18, "b",
       "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, "
       "SUM(l_quantity) AS sum_qty "
       "FROM customer, orders, lineitem "
       "WHERE o_orderkey IN ("
       "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
       "HAVING SUM(l_quantity) > {0}) "
       "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
       "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
       "ORDER BY o_totalprice DESC, o_orderdate ASC",
       "SELECT * FROM accel_tpch_q18_aux({0})", false},
      {19, "sbsbsb",
       "SELECT COALESCE(SUM(l_extendedprice * (1 - l_discount)), 0) AS revenue "
       "FROM lineitem, part "
       "WHERE (p_partkey = l_partkey "
       "AND p_brand = {0} "
       "AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
       "AND l_quantity >= {1} AND l_quantity <= {1} + 10 "
       "AND p_size BETWEEN 1 AND 5 "
       "AND l_shipmode IN ('AIR', 'AIR REG') "
       "AND l_shipinstruct = 'DELIVER IN PERSON') "
       "OR (p_partkey = l_partkey "
       "AND p_brand = {2} "
       "AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
       "AND l_quantity >= {3} AND l_quantity <= {3} + 10 "
       "AND p_size BETWEEN 1 AND 10 "
       "AND l_shipmode IN ('AIR', 'AIR REG') "
       "AND l_shipinstruct = 'DELIVER IN PERSON') "
       "OR (p_partkey = l_partkey "
       "AND p_brand = {4} "
       "AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
       "AND l_quantity >= {5} AND l_quantity <= {5} + 10 "
       "AND p_size BETWEEN 1 AND 15 "
       "AND l_shipmode IN ('AIR', 'AIR REG') "
       "AND l_shipinstruct = 'DELIVER IN PERSON')",
       "SELECT * FROM accel_tpch_q19_aux({0}, {1}, {2}, {3}, {4}, {5})", false},
      {20, "dss",
       "SELECT s_name, s_address "
       "FROM supplier, nation "
       "WHERE s_suppkey IN ("
       "SELECT ps_suppkey FROM partsupp, ("
       "SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
       "FROM lineitem WHERE l_shipdate >= {0} "
       "AND l_shipdate < {0} + INTERVAL '1' year "
       "GROUP BY l_partkey, l_suppkey) agg_lineitem "
       "WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
       "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE {1}) "
       "AND ps_availqty > agg_quantity) "
       "AND s_nationkey = n_nationkey AND n_name = {2} "
       "ORDER BY s_name",
       "SELECT * FROM accel_tpch_q20_aux({0}, {1}, {2})", false},
      {21, "s",
       "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
       "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
       "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
       "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
       "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
       "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
       "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
       "n_name = {0} GROUP BY s_name ORDER BY numwait DESC, s_name",
       "SELECT * FROM accel_tpch_q21_aux({0})", false},
      {22, "sssssss",
       "WITH custsale AS ("
       "SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
       "FROM customer "
       "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ({0}, {1}, {2}, {3}, {4}, {5}, {6}) "
       "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
       "AND SUBSTRING(c_phone FROM 1 FOR 2) IN ({0}, {1}, {2}, {3}, {4}, {5}, {6})) "
       "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
       "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
       "FROM custsale GROUP BY cntrycode ORDER BY cntrycode",
       "SELECT * FROM accel_tpch_q22_aux({0}, {1}, {2}, {3}, {4}, {5}, {6})", false},
  };
  return t;
}


// ---------------------------------------------------------------------------
// Domains (TPC-H spec 4.2.2 / 4.2.3)
// ---------------------------------------------------------------------------

inline const std::vector<std::string> &Nations() {
  static const std::vector<std::string> v = {
      "ALGERIA", "ARGENTINA", "BRAZIL", "CANADA", "EGYPT", "ETHIOPIA", "FRANCE", "GERMANY", "INDIA",
      "INDONESIA", "IRAN", "IRAQ", "JAPAN", "JORDAN", "KENYA", "MOROCCO", "MOZAMBIQUE", "PERU", "CHINA",
      "ROMANIA", "SAUDI ARABIA", "VIETNAM", "RUSSIA", "UNITED KINGDOM", "UNITED STATES"};
  return v;
}
inline const std::vector<std::string> &NationRegions() { // region of Nations()[i]
  static const std::vector<std::string> v = {
      "AFRICA", "AMERICA", "AMERICA", "AMERICA", "MIDDLE EAST", "AFRICA", "EUROPE", "EUROPE", "ASIA",
      "ASIA", "MIDDLE EAST", "MIDDLE EAST", "ASIA", "MIDDLE EAST", "AFRICA", "AFRICA", "AFRICA", "AMERICA",
      "ASIA", "EUROPE", "MIDDLE EAST", "ASIA", "EUROPE", "EUROPE", "AMERICA"};
  return v;
}
inline const std::vector<std::string> &Regions() {
  static const std::vector<std::string> v = {"AFRICA", "AMERICA", "ASIA", "EUROPE", "MIDDLE EAST"};
  return v;
}
inline const std::vector<std::string> &Segments() {
  static const std::vector<std::string> v = {"AUTOMOBILE", "BUILDING", "FURNITURE", "MACHINERY", "HOUSEHOLD"};
  return v;
}
inline const std::vector<std::string> &Modes() {
  static const std::vector<std::string> v = {"REG AIR", "AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB"};
  return v;
}
inline const std::vector<std::string> &TypeS1() {
  static const std::vector<std::string> v = {"STANDARD", "SMALL", "MEDIUM", "LARGE", "ECONOMY", "PROMO"};
  return v;
}
inline const std::vector<std::string> &TypeS2() {
  static const std::vector<std::string> v = {"ANODIZED", "BURNISHED", "PLATED", "POLISHED", "BRUSHED"};
  return v;
}
inline const std::vector<std::string> &TypeS3() {
  static const std::vector<std::string> v = {"TIN", "NICKEL", "BRASS", "STEEL", "COPPER"};
  return v;
}
inline const std::vector<std::string> &ContS1() {
  static const std::vector<std::string> v = {"SM", "LG", "MED", "JUMBO", "WRAP"};
  return v;
}
inline const std::vector<std::string> &ContS2() {
  static const std::vector<std::string> v = {"CASE", "BOX", "BAG", "JAR", "PKG", "PACK", "CAN", "DRUM"};
  return v;
}
inline const std::vector<std::string> &Colors() {
  static const std::vector<std::string> v = {
      "almond", "antique", "aquamarine", "azure", "beige", "bisque", "black", "blanched", "blue", "blush",
      "brown", "burlywood", "burnished", "chartreuse", "chiffon", "chocolate", "coral", "cornflower",
      "cornsilk", "cream", "cyan", "dark", "deep", "dim", "dodger", "drab", "firebrick", "floral", "forest",
      "frosted", "gainsboro", "ghost", "goldenrod", "green", "grey", "honeydew", "hot", "indian", "ivory",
      "khaki", "lace", "lavender", "lawn", "lemon", "light", "lime", "linen", "magenta", "maroon", "medium",
      "metallic", "midnight", "mint", "misty", "moccasin", "navajo", "navy", "olive", "orange", "orchid",
      "pale", "papaya", "peach", "peru", "pink", "plum", "powder", "puff", "purple", "red", "rose", "rosy",
      "royal", "saddle", "salmon", "sandy", "seashell", "sienna", "sky", "slate", "smoke", "snow", "spring",
      "steel", "tan", "thistle", "tomato", "turquoise", "violet", "wheat", "white", "yellow"};
  return v;
}

// ---------------------------------------------------------------------------
// Random helpers
// ---------------------------------------------------------------------------

using Rng = std::mt19937_64;
inline int64_t RI(Rng &g, int64_t lo, int64_t hi) { // inclusive, portable
  return lo + static_cast<int64_t>(g() % static_cast<uint64_t>(hi - lo + 1));
}
template <class T> inline const T &Pick(Rng &g, const std::vector<T> &v) {
  return v[static_cast<std::size_t>(g() % v.size())];
}

inline int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
inline std::string CivilFromDays(int64_t z) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(y + (m <= 2)), m, d);
  return buf;
}
inline std::string Ymd(int y, int m, int d) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return buf;
}
inline std::string AnyDate(Rng &g, int y0, int y1) { // uniformly random valid day
  return CivilFromDays(RI(g, DaysFromCivil(y0, 1, 1), DaysFromCivil(y1, 12, 31)));
}
inline std::string Num(double v, int prec) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  return buf;
}
inline std::string Brand(Rng &g) { return "Brand#" + std::to_string(RI(g, 1, 5)) + std::to_string(RI(g, 1, 5)); }
inline std::string FullType(Rng &g) { return Pick(g, TypeS1()) + " " + Pick(g, TypeS2()) + " " + Pick(g, TypeS3()); }
inline std::string Container(Rng &g) { return Pick(g, ContS1()) + " " + Pick(g, ContS2()); }
inline std::string Mangle(Rng &g, const std::string &s) { // random case change / prefix / suffix / truncation
  switch (g() % 6) {
  case 0: { std::string o = s; for (auto &c : o) c = static_cast<char>(std::tolower(c)); return o; }
  case 1: return s.substr(0, 1 + g() % s.size());
  case 2: return s + " ";
  case 3: return s + "X";
  case 4: return "";
  default: return s.substr(s.size() / 2);
  }
}
// A random LIKE pattern built from fragments of type words.
inline std::string TypePattern(Rng &g) {
  const std::string w = g() % 2 ? Pick(g, TypeS3()) : (g() % 2 ? Pick(g, TypeS1()) : Pick(g, TypeS2()));
  const std::size_t a = g() % w.size();
  const std::string frag = w.substr(a, 1 + g() % (w.size() - a));
  switch (g() % 7) {
  case 0: return "%" + frag;
  case 1: return frag + "%";
  case 2: return "%" + frag + "%";
  case 3: return w + "%";
  case 4: return "%" + w;
  case 5: return FullType(g);
  default: return Pick(g, TypeS1()) + " " + frag + "%";
  }
}
inline std::string ColorFragment(Rng &g) {
  const std::string &c = Pick(g, Colors());
  const std::size_t a = g() % c.size();
  return c.substr(a, 1 + g() % (c.size() - a));
}
inline std::string AnyNation(Rng &g, int offspec_pct) {
  if (RI(g, 0, 99) < offspec_pct) return Mangle(g, Pick(g, Nations()));
  return Pick(g, Nations());
}

// ---------------------------------------------------------------------------
// Class (a): spec-domain draws
// ---------------------------------------------------------------------------

inline Vec SpecDraw(int q, Rng &g) {
  auto FirstOfMonth = [&](int y0, int m0, int y1, int m1) {
    const int n = (y1 - y0) * 12 + (m1 - m0);
    const int k = static_cast<int>(RI(g, 0, n));
    return Ymd(y0 + (m0 - 1 + k) / 12, (m0 - 1 + k) % 12 + 1, 1);
  };
  auto Jan1 = [&](int y0, int y1) { return Ymd(static_cast<int>(RI(g, y0, y1)), 1, 1); };
  auto Distinct = [&](int n, int64_t lo, int64_t hi) {
    std::vector<int64_t> v;
    while (static_cast<int>(v.size()) < n) {
      const int64_t x = RI(g, lo, hi);
      if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
    }
    return v;
  };
  switch (q) {
  case 1: return {std::to_string(RI(g, 60, 120))};
  case 2: return {std::to_string(RI(g, 1, 50)), "%" + Pick(g, TypeS3()), Pick(g, Regions())};
  case 3: return {Pick(g, Segments()), Ymd(1995, 3, static_cast<int>(RI(g, 1, 31)))};
  case 4: return {FirstOfMonth(1993, 1, 1997, 10)};
  case 5: return {Pick(g, Regions()), Jan1(1993, 1997)};
  case 6: return {Jan1(1993, 1997), Num(static_cast<double>(RI(g, 2, 9)) / 100.0, 2), std::to_string(RI(g, 24, 25))};
  case 7: {
    const auto a = RI(g, 0, 24);
    auto b = RI(g, 0, 23);
    if (b >= a) ++b;
    return {Nations()[static_cast<std::size_t>(a)], Nations()[static_cast<std::size_t>(b)]};
  }
  case 8: {
    const auto n = static_cast<std::size_t>(RI(g, 0, 24));
    return {Nations()[n], NationRegions()[n], FullType(g)};
  }
  case 9: return {Pick(g, Colors())};
  case 10: return {FirstOfMonth(1993, 2, 1995, 1)};
  case 11: return {Pick(g, Nations()), Pick(g, std::vector<std::string>{"0.0001", "0.001", "0.01", "0.00001"})};
  case 12: {
    const auto m = Distinct(2, 0, 6);
    return {Modes()[static_cast<std::size_t>(m[0])], Modes()[static_cast<std::size_t>(m[1])], Jan1(1993, 1997)};
  }
  case 13: return {"%" + Pick(g, std::vector<std::string>{"special", "pending", "unusual", "express"}) + "%" +
                   Pick(g, std::vector<std::string>{"packages", "requests", "accounts", "deposits"}) + "%"};
  case 14: return {FirstOfMonth(1993, 1, 1997, 12)};
  case 15: return {FirstOfMonth(1993, 1, 1997, 10)};
  case 16: {
    Vec v = {Brand(g), Pick(g, TypeS1()) + " " + Pick(g, TypeS2()) + "%"};
    for (auto s : Distinct(8, 1, 50)) v.push_back(std::to_string(s));
    return v;
  }
  case 17: return {Brand(g), Container(g)};
  case 18: return {std::to_string(RI(g, 312, 315))};
  case 19: return {Brand(g), std::to_string(RI(g, 1, 10)), Brand(g), std::to_string(RI(g, 10, 20)), Brand(g),
                   std::to_string(RI(g, 20, 30))};
  case 20: return {Jan1(1993, 1997), Pick(g, Colors()) + "%", Pick(g, Nations())};
  case 21: return {Pick(g, Nations())};
  case 22: {
    Vec v;
    for (auto c : Distinct(7, 10, 34)) v.push_back(std::to_string(c));
    return v;
  }
  }
  return {};
}

// ---------------------------------------------------------------------------
// Class (b): off-spec but valid SQL values
// ---------------------------------------------------------------------------

inline Vec OffSpecDraw(int q, Rng &g) {
  auto Dt = [&] { return AnyDate(g, 1988, 2002); };
  auto Qty = [&] { return Num(static_cast<double>(RI(g, 0, 120)) / 2.0, 1); };
  switch (q) {
  case 1: return {std::to_string(RI(g, -200, 3000))};
  case 2: return {std::to_string(RI(g, 0, 60)), TypePattern(g), g() % 3 ? Pick(g, Regions()) : Mangle(g, Pick(g, Regions()))};
  case 3: return {g() % 4 ? Pick(g, Segments()) : Mangle(g, Pick(g, Segments())), Dt()};
  case 4: return {Dt()};
  case 5: return {g() % 4 ? Pick(g, Regions()) : Mangle(g, Pick(g, Regions())), Dt()};
  case 6: return {Dt(), Num(static_cast<double>(RI(g, 0, 120)) / 1000.0, 3), Qty()};
  case 7: {
    Vec v = {AnyNation(g, 15), AnyNation(g, 15)};
    if (g() % 5 == 0) v[1] = v[0];
    return v;
  }
  case 8: return {AnyNation(g, 10), g() % 3 ? Pick(g, Regions()) : Mangle(g, Pick(g, Regions())),
                  g() % 2 ? FullType(g) : Mangle(g, FullType(g))};
  case 9: return {g() % 3 ? ColorFragment(g) : Mangle(g, Pick(g, Colors()))};
  case 10: return {Dt()};
  case 11: {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3g", std::pow(10.0, -static_cast<double>(RI(g, 0, 90)) / 10.0));
    return {AnyNation(g, 10), buf};
  }
  case 12: {
    Vec v = {g() % 6 ? Pick(g, Modes()) : Mangle(g, Pick(g, Modes())), g() % 6 ? Pick(g, Modes()) : Mangle(g, Pick(g, Modes())), Dt()};
    if (g() % 5 == 0) v[1] = v[0];
    return v;
  }
  case 13: {
    const std::vector<std::string> w = {"special", "pending", "unusual", "express", "final", "regular", "ironic",
                                        "furious", "quick", "careful", "requests", "accounts", "deposits",
                                        "packages", "foxes", "theodolites", "even", "bold", "_", "x"};
    switch (g() % 4) {
    case 0: return {"%" + Pick(g, w) + "%" + Pick(g, w) + "%"};
    case 1: return {Pick(g, w) + "%" + Pick(g, w)};
    case 2: return {"%" + Pick(g, w) + "%"};
    default: return {"%" + Pick(g, w) + "_" + Pick(g, w) + "%"};
    }
  }
  case 14: return {Dt()};
  case 15: return {Dt()};
  case 16: {
    Vec v = {g() % 5 ? Brand(g) : "Brand#" + std::to_string(RI(g, 0, 9)) + std::to_string(RI(g, 0, 9)), TypePattern(g)};
    for (int i = 0; i < 8; ++i) v.push_back(std::to_string(RI(g, -2, 60)));
    return v;
  }
  case 17: return {g() % 5 ? Brand(g) : Mangle(g, Brand(g)), g() % 5 ? Container(g) : Mangle(g, Container(g))};
  case 18: return {std::to_string(g() % 3 ? RI(g, 100, 400) : RI(g, -5, 99))};
  case 19: return {Brand(g), std::to_string(RI(g, -3, 60)), Brand(g), std::to_string(RI(g, -3, 60)), Brand(g), std::to_string(RI(g, -3, 60))};
  case 20: return {Dt(), g() % 2 ? ColorFragment(g) + "%" : Mangle(g, Pick(g, Colors())) + "%", AnyNation(g, 10)};
  case 21: return {AnyNation(g, 15)};
  case 22: {
    Vec v;
    for (int i = 0; i < 7; ++i) v.push_back(std::to_string(RI(g, 5, 45)));
    return v;
  }
  }
  return {};
}

// ---------------------------------------------------------------------------
// Class (c): adversarial edge cases. A base vector with one slot varied at a
// time, plus explicit combinations.
// ---------------------------------------------------------------------------

struct EdgeSet {
  Vec base;
  std::vector<std::pair<int, Vec>> vary; // slot -> values (subsampled by --max-edge)
  std::vector<Vec> must;                 // always run
};

inline const Vec &EdgeDates() {
  static const Vec v = {
      "1991-12-31", "1992-01-01", "1992-01-02", "1992-02-29", "1993-01-31", "1993-03-31", "1993-05-31",
      "1993-07-31", "1993-08-31", "1993-10-31", "1993-11-30", "1994-12-31", "1995-01-29", "1995-01-30",
      "1995-01-31", "1995-02-28", "1995-03-01", "1995-03-31", "1995-04-30", "1995-12-31", "1996-01-01",
      "1996-02-28", "1996-02-29", "1996-03-01", "1996-12-31", "1997-01-01", "1997-08-31", "1997-11-30",
      "1998-01-01", "1998-08-02", "1998-12-01", "1998-12-31", "1999-01-01", "1985-06-01", "2000-02-29",
      "2005-01-01", "2100-03-01", "9999-12-31", "0001-01-01", kNull};
  return v;
}
inline Vec Mix(const Vec &a, const Vec &b) {
  Vec o = a;
  o.insert(o.end(), b.begin(), b.end());
  return o;
}
inline Vec NationEdges() {
  Vec v = Nations();
  for (const char *x : {"", "NOPE", "brazil", "Brazil", "BRAZIL ", " BRAZIL", "UNITED", "%", "_"}) v.push_back(x);
  v.push_back(kNull);
  return v;
}
inline Vec RegionEdges() { return Mix(Regions(), {"", "NOPE", "asia", "Asia", "ASIA ", "%", kNull}); }

inline EdgeSet EdgeCases(int q) {
  switch (q) {
  case 1:
    return {{"90"}, {{0, {"0", "1", "-1", "60", "120", "121", "365", "1000", "2000", "2526", "2527", "2528", "5000", "100000", "1000000", "3000000", kNull}}}, {}};
  case 2:
    return {{"15", "%BRASS", "EUROPE"},
            {{0, {"0", "1", "50", "51", "-1", "1000", "2147483647", kNull}},
             {1, {"", "%", "_RASS", "%BRASS", "BRASS", "%brass", "%BRASS%", "STANDARD%", "%TIN", "%%", "%RA_S", "_%",
                  "ECONOMY ANODIZED STEEL", "PROMO BRUSHED BRASS", "%STEEL", "%NICKEL", "SMALL%", "%A_ASS", kNull}},
             {2, RegionEdges()}},
            {{"0", "%", "EUROPE"}, {"51", "", ""}, {"1", "_", "AMERICA"}}};
  case 3: {
    Vec segs = Mix(Segments(), {"", "ZZTEST", "building", "Building", "BUILDING ", "%", kNull});
    return {{"BUILDING", "1995-03-15"}, {{0, segs}, {1, EdgeDates()}}, {{"AUTOMOBILE", "1992-01-01"}, {"MACHINERY", "1998-12-31"}, {"BUILDING", "1992-01-01"}}};
  }
  case 4: return {{"1993-07-01"}, {{0, EdgeDates()}}, {}};
  case 5: return {{"ASIA", "1994-01-01"}, {{0, RegionEdges()}, {1, EdgeDates()}}, {{"AMERICA", "1995-01-31"}, {"EUROPE", "1996-02-29"}}};
  case 6:
    return {{"1994-01-01", "0.06", "24"},
            {{0, EdgeDates()},
             {1, {"0.065", "0.0", "0", "0.005", "0.01", "0.02", "0.1", "0.11", "-0.01", "1", "0.0049", "0.055", "0.075", "0.095", "0.09", "0.06", kNull}},
             {2, {"24.5", "24", "25", "0", "1", "50", "51", "100", "-1", "0.5", "1000", "24.99", "25.01", kNull}}},
            {{"1995-01-01", "0.065", "24.5"}, {"1996-02-29", "0.065", "24.5"}, {"1995-03-31", "0.0", "50"}, {"1993-01-01", "0.04", "24"}}};
  case 7: {
    Vec n = NationEdges();
    return {{"UNITED KINGDOM", "BRAZIL"},
            {{0, n}, {1, n}},
            {{"FRANCE", "FRANCE"}, {"GERMANY", "GERMANY"}, {"BRAZIL", "BRAZIL"}, {"", ""}, {kNull, kNull}, {"FRANCE", "GERMANY"},
             {"GERMANY", "FRANCE"}, {"INDIA", "INDIA"}, {"UNITED KINGDOM", "UNITED KINGDOM"}}};
  }
  case 8:
    return {{"INDIA", "ASIA", "PROMO BRUSHED BRASS"},
            {{0, NationEdges()},
             {1, RegionEdges()},
             {2, {"ECONOMY ANODIZED STEEL", "", "PROMO BRUSHED", "promo brushed brass", "STANDARD POLISHED TIN", "%", "PROMO BRUSHED BRASS ", "SMALL PLATED COPPER", "LARGE BURNISHED NICKEL", kNull}}},
            {{"BRAZIL", "AMERICA", "ECONOMY ANODIZED STEEL"}, {"ROMANIA", "EUROPE", "STANDARD POLISHED TIN"}, {"INDIA", "AMERICA", "PROMO BRUSHED BRASS"}}};
  case 9:
    return {{"green"},
            {{0, {"gree", "n", "GREEN", "Green", "green ", " green", "", " ", "e", "een", "reen", "gr", "lavender", "l", "a", "ab", "zzzzz", "pink", "forest", "almond", "sienna", "blanched", "navy", "ste", "el", "frosted", "ghost", "gh", "ost", "Chocolate", "GAINSBORO", "aquamarine", kNull}}},
            {}};
  case 10: return {{"1993-10-01"}, {{0, EdgeDates()}}, {}};
  case 11:
    return {{"ROMANIA", "0.0001"},
            {{0, NationEdges()},
             {1, {"0", "1", "1e-9", "0.5", "0.1", "0.00001", "2", "-1", "0.9999999", "1e-12", "100", "0.0", "0.001", "0.01", kNull}}},
            {{"GERMANY", "0"}, {"GERMANY", "1"}, {"FRANCE", "1e-9"}}};
  case 12: {
    Vec m = Mix(Modes(), {"", "ZZZ", "mail", "Mail", "MAIL ", "REG", kNull});
    return {{"MAIL", "SHIP", "1994-01-01"},
            {{0, m}, {1, m}, {2, EdgeDates()}},
            {{"MAIL", "MAIL", "1994-01-01"}, {"SHIP", "SHIP", "1995-01-01"}, {"AIR", "AIR", "1994-01-01"}, {"RAIL", "RAIL", "1993-01-01"},
             {"", "", "1994-01-01"}, {"TRUCK", "FOB", "1996-02-29"}, {"MAIL", "SHIP", "1995-03-31"}, {"FOB", "FOB", "1997-01-01"}, {kNull, kNull, "1994-01-01"}}};
  }
  case 13:
    return {{"%special%requests%"},
            {{0, {"%unusual%accounts%", "%pending%deposits%", "%express%packages%", "%", "", "%%", "%a%b%", "unusual", "%unusual%", "%unusual%accounts",
                  "unusual%accounts%", "%unusual%accounts%%", "%un_sual%accounts%", "%unusual_accounts%", "%unusual\\_accounts%", "%100\\%%", "%_%", "_%",
                  "%_", "%x%y%z%", "%unusual%unusual%accounts%", "UNUSUAL", "%UNUSUAL%ACCOUNTS%", "%special%requests%accounts%", "% %", "%,%",
                  "%special%requests", "special%requests%", "%special requests%", "%specialrequests%", "%s%r%", "%special%%requests%", "%special%requests%%",
                  "%a%", "%e%e%", "%final%", "%special%pending%", "%sp_cial%req_ests%", "%special__%requests%", "%\\%%", kNull}}},
            {}};
  case 14: return {{"1995-09-01"}, {{0, EdgeDates()}}, {}};
  case 15: return {{"1996-01-01"}, {{0, EdgeDates()}}, {}};
  case 16: {
    Vec b = Mix({"Brand#11", "Brand#55", "Brand#13", "Brand#45", "Brand#23"}, {"Brand#99", "Brand#00", "", "brand#13", "Brand#1", kNull});
    Vec t = {"%", "", "SMALL BRUSHED%", "MEDIUM POLISHED%", "%TIN", "%BRASS%", "_MALL BRUSHED%", "SMALL%", "STANDARD%", "small brushed%", "%BRUSHED%",
             "PROMO ANODIZED STEEL", "MEDIUM%", "% %", "%_", "MEDIUM POLISHED", "SMALL BRUSHED", "STANDARD ANODIZED%", "ECONOMY%", "%ANODIZED%", "_%", "%%", kNull};
    auto S = [](std::initializer_list<const char *> l) { Vec v; for (auto x : l) v.push_back(x); return v; };
    Vec base = Mix({"Brand#13", "SMALL BRUSHED%"}, S({"26", "24", "27", "29", "7", "20", "22", "33"}));
    auto Sizes = [&](Vec s) { return Mix({"Brand#45", "MEDIUM POLISHED%"}, s); };
    EdgeSet e{base, {{0, b}, {1, t}, {2, {"0", "51", "1000", "26", "-1", "50", "1", kNull}}, {9, {"0", "51", "1000", "33", "-1", "50", "1", kNull}}}, {}};
    e.must = {Sizes(S({"0", "0", "0", "0", "0", "0", "0", "0"})),
              Sizes(S({"51", "52", "53", "54", "55", "56", "57", "58"})),
              Sizes(S({"1000", "1000", "1000", "1000", "1000", "1000", "1000", "1000"})),
              Sizes(S({"1", "1", "1", "1", "1", "1", "1", "1"})),
              Sizes(S({"1", "2", "3", "4", "5", "6", "7", "8"})),
              Sizes(S({"49", "14", "23", "45", "19", "3", "36", "9"})),
              Sizes(S({"50", "50", "49", "49", "48", "48", "47", "47"})),
              Sizes(S({"-1", "0", "51", "52", "100", "1000", "2000", "3000"})),
              Sizes({kNull, kNull, kNull, kNull, kNull, kNull, kNull, kNull}),
              Sizes({"1", kNull, "2", kNull, "3", kNull, "4", kNull}),
              Sizes(S({"26", "24", "27", "29", "7", "20", "22", "0"})),
              Sizes(S({"26", "24", "27", "29", "7", "20", "22", "51"})),
              Sizes(S({"26", "24", "27", "29", "7", "20", "22", "1000"})),
              Sizes(S({"26", "26", "26", "24", "24", "24", "27", "27"})),
              Sizes(S({"10", "11", "12", "13", "14", "15", "16", "17"})),
              Sizes(S({"18", "19", "20", "21", "22", "23", "24", "25"})),
              Sizes(S({"32", "33", "34", "35", "36", "37", "38", "39"})),
              Sizes(S({"40", "41", "42", "43", "44", "45", "46", "47"})),
              Sizes(S({"256", "257", "258", "259", "260", "261", "262", "263"})),
              Sizes(S({"2147483647", "2147483646", "3", "4", "5", "6", "7", "8"}))};
    return e;
  }
  case 17: {
    Vec brands, conts;
    for (int a = 1; a <= 5; ++a)
      for (int b = 1; b <= 5; ++b) brands.push_back("Brand#" + std::to_string(a) + std::to_string(b));
    for (const char *x : {"Brand#00", "Brand#99", "Brand#6", "", "brand#34", "Brand#34 "}) brands.push_back(x);
    brands.push_back(kNull);
    for (const auto &a : ContS1())
      for (const auto &b : ContS2()) conts.push_back(a + " " + b);
    for (const char *x : {"", "ZZ BOX", "lg case", "LG CASE ", "LG", "CASE"}) conts.push_back(x);
    conts.push_back(kNull);
    return {{"Brand#34", "LG CASE"},
            {{0, brands}, {1, conts}},
            {{"Brand#23", "MED PKG"}, {"Brand#23", "JUMBO PACK"}, {"Brand#35", "SM CAN"}, {"Brand#45", "LG DRUM"},
             {"Brand#23", "MED BOX"}, {"Brand#23", "MED BAG"}, {"Brand#11", "SM CASE"}, {"Brand#55", "WRAP DRUM"}, {"Brand#11", "WRAP DRUM"},
             {"Brand#55", "SM CASE"}, {"Brand#41", "JUMBO JAR"}, {"Brand#14", "JUMBO JAR"}, {"Brand#51", "MED CAN"}, {"Brand#15", "MED CAN"}}};
  }
  case 18:
    return {{"312"}, {{0, {"0", "1", "50", "100", "250", "300", "311", "312", "313", "314", "315", "320", "350", "1000", "-1", "10000", "9223372036854775807", kNull}}}, {}};
  case 19: {
    Vec b = {"Brand#12", "Brand#23", "Brand#34", "Brand#99", "", "Brand#1", "Brand#31", "Brand#51", "brand#12", kNull};
    Vec q = {"0", "1", "100", "-1", "10", "20", "30", "40", "50", "5", "15", "23", "4", "11", "1000", "4611686018427387904", kNull};
    return {{"Brand#31", "4", "Brand#12", "15", "Brand#51", "23"},
            {{0, b}, {1, q}, {2, b}, {3, q}, {4, b}, {5, q}},
            {{"Brand#12", "1", "Brand#23", "10", "Brand#34", "20"},
             {"Brand#12", "1", "Brand#12", "10", "Brand#12", "20"},
             {"Brand#12", "0", "Brand#12", "0", "Brand#12", "0"},
             {"Brand#12", "100", "Brand#23", "100", "Brand#34", "100"},
             {"Brand#12", "-10", "Brand#23", "-10", "Brand#34", "-10"},
             {"Brand#12", "1", "Brand#12", "1", "Brand#12", "1"},
             {"", "0", "", "0", "", "0"}, {kNull, kNull, kNull, kNull, kNull, kNull}}};
  }
  case 20: {
    Vec pats = {"gre%", "g%", "%", "", "a%", "gainsboro", "gainsboro%", "%gainsboro%", "GAINSBORO%", "Gainsboro%", "_ainsboro%", "forest%", "ivory%",
                "green %", "almond antique%", "sky%", "sky blue%", "fo%", "f%", "azure%", "%en%", "ch%", "b%", "bl%", "bla%", "blue%", "forest", "for%t%", kNull};
    return {{"1997-01-01", "gainsboro%", "RUSSIA"},
            {{0, EdgeDates()}, {1, pats}, {2, NationEdges()}},
            {{"1996-01-01", "forest%", "CANADA"}, {"1995-03-01", "azure%", "ARGENTINA"}, {"1996-02-29", "gre%", "GERMANY"}, {"1995-03-01", "gre%", "RUSSIA"},
             {"1996-01-01", "g%", "RUSSIA"}, {"1994-02-28", "forest%", "FRANCE"}, {"1996-01-01", "gainsboro%", "RUSSIA"}, {"1993-03-01", "%", "CANADA"}}};
  }
  case 21: return {{"SAUDI ARABIA"}, {{0, NationEdges()}}, {}};
  case 22: {
    auto S = [](std::initializer_list<const char *> l) { Vec v; for (auto x : l) v.push_back(x); return v; };
    return {{S({"30", "19", "14", "28", "11", "22", "32"})},
            {},
            {S({"13", "13", "13", "13", "13", "13", "13"}), S({"99", "99", "99", "99", "99", "99", "99"}),
             S({"1", "3", "4", "5", "6", "7", "8"}), S({"130", "131", "132", "133", "134", "135", "136"}),
             S({"13", "31", "23", "29", "30", "18", "17"}), S({"13", "1", "130", "31", "3", "310", "23"}),
             S({"", "", "", "", "", "", ""}), {kNull, kNull, kNull, kNull, kNull, kNull, kNull}, S({"13", "31", "23", "29", "30", "18", "17"}),
             {"13", kNull, "31", kNull, "23", kNull, "29"}, S({"10", "11", "12", "13", "14", "15", "16"}), S({"17", "18", "19", "20", "21", "22", "23"}),
             S({"24", "25", "26", "27", "28", "29", "30"}), S({"31", "32", "33", "34", "10", "11", "12"}),
             S({"13", "13", "31", "31", "23", "23", "29"}), S({" 3", "3 ", " 13", "13 ", "1 ", " 1", "1-"}),
             S({"13", "14", "15", "16", "17", "18", "19"}), S({"00", "01", "02", "03", "04", "05", "06"}),
             S({"35", "36", "37", "38", "39", "40", "41"}), S({"1", "2", "3", "4", "5", "6", "7"}),
             S({"13", "31", "23", "29", "30", "18", "17"}), S({"30", "19", "14", "28", "11", "22", "99"}),
             S({"30", "19", "14", "28", "11", "22", "3"}), S({"30", "19", "14", "28", "11", "22", "322"}),
             S({"30", "19", "14", "28", "11", "22", "30"}), S({"3_", "1%", "%", "_", "__", "%%", "1_"})}};
  }
  }
  return {};
}

} // namespace accel::postgres::tpch_params
