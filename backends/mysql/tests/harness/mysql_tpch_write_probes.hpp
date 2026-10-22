#pragma once

#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"

namespace accel::mysql::tpch_harness {

// Concrete UPDATE + restore that must change this query's observable result.
struct WriteProbe {
  std::string update_sql;
  std::string restore_sql;
  int candidate = 0;
};

inline std::vector<std::string> SplitProbeFields(const std::string &line) {
  std::vector<std::string> cells;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      cells.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  cells.push_back(cur);
  return cells;
}

inline WriteProbe ResolveWriteProbe(const Tools &tools, int query_num,
                                    int candidate_offset) {
  auto Fetch = [&](const std::string &sql) {
    auto r = QuerySQL(tools, sql);
    Require(r.first, "probe select failed for Q" + std::to_string(query_num) +
                         ":\n" + sql + "\n" + r.second);
    auto lines = SplitLines(TrimLines(r.second));
    Require(!lines.empty(), "probe select returned no rows for Q" +
                                std::to_string(query_num) + " offset=" +
                                std::to_string(candidate_offset));
    return SplitProbeFields(lines[0]);
  };
  const std::string lim =
      " LIMIT 1 OFFSET " + std::to_string(candidate_offset);
  auto LinePriceBump = [candidate_offset](const std::vector<std::string> &f,
                                          const char *delta) {
    Require(f.size() >= 3, "lineitem price probe needs 3 fields");
    WriteProbe p;
    // `delta` is a percentage of the row's own price: a fixed amount would
    // round away in the fingerprint of magnified money columns.
    p.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + "
                   "l_extendedprice * " + std::string(delta) + " / 100 WHERE l_orderkey = " + f[0] +
                   " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_extendedprice = " + f[2] +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.candidate = candidate_offset;
    return p;
  };

  switch (query_num) {
  case 1:
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice FROM lineitem "
              "WHERE l_shipdate <= DATE_SUB(DATE '1998-12-01', INTERVAL 90 DAY) "
              "ORDER BY l_orderkey, l_linenumber " + lim),
        "100");
  case 2: {
    // Touch s_acctbal on a top Q2 result row. Attribute-only supplier updates
    // use ApplyDelta (nationkey unchanged); order-by change invalidates results.
    auto f = Fetch(
        "SELECT s.s_suppkey, s.s_acctbal "
        "FROM part p, supplier s, partsupp ps2, nation n, region r "
        "WHERE p.p_partkey = ps2.ps_partkey AND s.s_suppkey = ps2.ps_suppkey "
        "AND p.p_size = 15 AND p.p_type LIKE '%BRASS' "
        "AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey "
        "AND r.r_name = 'EUROPE' "
        "AND ps2.ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, "
        "nation, region WHERE p.p_partkey = ps_partkey AND s_suppkey = ps_suppkey "
        "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
        "AND r_name = 'EUROPE') "
        "ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey " + lim);
    Require(f.size() >= 2, "Q2 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE supplier SET s_acctbal = s_acctbal + s_acctbal "
                   "WHERE s_suppkey = " +
                   f[0];
    p.restore_sql = "UPDATE supplier SET s_acctbal = " + f[1] +
                    " WHERE s_suppkey = " + f[0];
    p.candidate = candidate_offset;
    return p;
  }
  case 3:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice "
              "FROM lineitem l WHERE l.l_orderkey IN ("
              "SELECT l_orderkey FROM ("
              "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
              "o_orderdate, o_shippriority "
              "FROM customer, orders, lineitem "
              "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey "
              "AND l_orderkey = o_orderkey AND o_orderdate < DATE '1995-03-15' "
              "AND l_shipdate > DATE '1995-03-15' "
              "GROUP BY l_orderkey, o_orderdate, o_shippriority "
              "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10) AS q3_top"
              ") AND l.l_shipdate > DATE '1995-03-15' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + lim),
        "100");
  case 4: {
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_commitdate, "
        "l.l_receiptdate FROM lineitem l "
        "WHERE l.l_commitdate < l.l_receiptdate AND l.l_orderkey = ("
        "SELECT o.o_orderkey FROM orders o "
        "WHERE o.o_orderdate >= DATE '1993-07-01' "
        "AND o.o_orderdate < DATE_ADD(DATE '1993-07-01', INTERVAL 3 MONTH) "
        "AND (SELECT COUNT(*) FROM lineitem lx "
        "WHERE lx.l_orderkey = o.o_orderkey AND lx.l_commitdate < lx.l_receiptdate) = 1 "
        "ORDER BY o.o_orderkey LIMIT 1) "
        "ORDER BY l.l_linenumber " + lim);
    Require(f.size() >= 4, "Q4 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[2]) +
                   " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[3]) +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.candidate = candidate_offset;
    return p;
  }
  case 5:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice "
              "FROM lineitem l, orders o, customer c, supplier s, nation n, region r "
              "WHERE l.l_orderkey = o.o_orderkey AND c.c_custkey = o.o_custkey "
              "AND l.l_suppkey = s.s_suppkey AND c.c_nationkey = s.s_nationkey "
              "AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey "
              "AND r.r_name = 'AMERICA' AND o.o_orderdate >= DATE '1995-01-01' "
              "AND o.o_orderdate < DATE_ADD(DATE '1995-01-01', INTERVAL 1 YEAR) "
              "ORDER BY l.l_orderkey, l.l_linenumber " + lim),
        "100");
  case 6:
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice FROM lineitem "
              "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
              "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25 "
              "ORDER BY l_orderkey, l_linenumber " + lim),
        "100");
  case 7:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice "
              "FROM lineitem l, supplier s, orders o, customer c, nation n1, nation n2 "
              "WHERE s.s_suppkey = l.l_suppkey AND o.o_orderkey = l.l_orderkey "
              "AND c.c_custkey = o.o_custkey AND s.s_nationkey = n1.n_nationkey "
              "AND c.c_nationkey = n2.n_nationkey "
              "AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL') "
              "OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM')) "
              "AND l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + lim),
        "100");
  case 8: {
    auto f = Fetch(
        "SELECT p.p_partkey, p.p_type FROM part p, lineitem l, orders o, "
        "customer c, nation n1, region r "
        "WHERE p.p_partkey = l.l_partkey AND l.l_orderkey = o.o_orderkey "
        "AND o.o_custkey = c.c_custkey AND c.c_nationkey = n1.n_nationkey "
        "AND n1.n_regionkey = r.r_regionkey AND r.r_name = 'ASIA' "
        "AND o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
        "ORDER BY p.p_partkey " + lim);
    Require(f.size() >= 2, "Q8 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE part SET p_type = 'PROMO BRUSHED BRASS' WHERE p_partkey = " +
                   f[0];
    p.restore_sql = "UPDATE part SET p_type = " + SqlLiteral(f[1]) +
                    " WHERE p_partkey = " + f[0];
    p.candidate = candidate_offset;
    return p;
  }
  case 9:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice "
              "FROM part p, lineitem l WHERE p.p_partkey = l.l_partkey "
              "AND p.p_name LIKE '%green%' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + lim),
        "100");
  case 10:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice "
              "FROM lineitem l, orders o "
              "WHERE l.l_orderkey = o.o_orderkey AND l.l_returnflag = 'R' "
              "AND o.o_orderdate >= DATE '1993-06-01' "
              "AND o.o_orderdate < DATE_ADD(DATE '1993-06-01', INTERVAL 3 MONTH) "
              "AND o.o_custkey IN ("
              "SELECT c_custkey FROM ("
              "SELECT c_custkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
              "FROM customer JOIN orders ON c_custkey = o_custkey "
              "JOIN lineitem ON l_orderkey = o_orderkey "
              "WHERE o_orderdate >= DATE '1993-06-01' "
              "AND o_orderdate < DATE_ADD(DATE '1993-06-01', INTERVAL 3 MONTH) "
              "AND l_returnflag = 'R' "
              "GROUP BY c_custkey ORDER BY revenue DESC, c_custkey LIMIT 20) top20) "
              "ORDER BY l.l_orderkey, l.l_linenumber " + lim),
        "100");
  case 11: {
    // Prefer the highest-contribution Romania partsupp so a cost bump crosses
    // the HAVING threshold (critical at SF10 where fraction must be 0.0001/SF).
    auto f = Fetch(
        "SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost, ps.ps_availqty "
        "FROM partsupp ps, supplier s, nation n "
        "WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey "
        "AND n.n_name = 'ROMANIA' "
        "ORDER BY (ps.ps_supplycost * ps.ps_availqty) DESC, "
        "ps.ps_partkey, ps.ps_suppkey " +
        lim);
    Require(f.size() >= 4, "Q11 probe fields");
    // Large multiplicative bump so value clearly exceeds total*fraction even
    // when the catalog fraction is still the SF1 default.
    WriteProbe p;
    p.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost * 1000 "
                   "WHERE ps_partkey = " +
                   f[0] + " AND ps_suppkey = " + f[1];
    p.restore_sql = "UPDATE partsupp SET ps_supplycost = " + f[2] +
                    " WHERE ps_partkey = " + f[0] + " AND ps_suppkey = " + f[1];
    p.candidate = candidate_offset;
    return p;
  }
  case 12: {
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_shipmode FROM orders o, lineitem l "
        "WHERE o.o_orderkey = l.l_orderkey AND l.l_shipmode IN ('MAIL', 'SHIP') "
        "AND l.l_commitdate < l.l_receiptdate AND l.l_shipdate < l.l_commitdate "
        "AND l.l_receiptdate >= DATE '1995-01-01' "
        "AND l.l_receiptdate < DATE_ADD(DATE '1995-01-01', INTERVAL 1 YEAR) "
        "ORDER BY l.l_orderkey, l.l_linenumber " + lim);
    Require(f.size() >= 3, "Q12 probe fields");
    const std::string neu = (f[2] == "MAIL") ? "AIR" : "MAIL";
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_shipmode = " + SqlLiteral(neu) +
                   " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_shipmode = " + SqlLiteral(f[2]) +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.candidate = candidate_offset;
    return p;
  }
  case 13: {
    auto f = Fetch(
        "SELECT o_orderkey, o_comment FROM orders "
        "WHERE o_comment NOT LIKE '%unusual%accounts%' "
        "ORDER BY o_orderkey " + lim);
    Require(f.size() >= 2, "Q13 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE orders SET o_comment = CONCAT(o_comment, ' unusual accounts') "
                   "WHERE o_orderkey = " +
                   f[0];
    p.restore_sql = "UPDATE orders SET o_comment = " + SqlLiteral(f[1]) +
                    " WHERE o_orderkey = " + f[0];
    p.candidate = candidate_offset;
    return p;
  }
  case 14:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice "
              "FROM lineitem l, part p WHERE l.l_partkey = p.p_partkey "
              "AND l.l_shipdate >= DATE '1995-06-01' "
              "AND l.l_shipdate < DATE_ADD(DATE '1995-06-01', INTERVAL 1 MONTH) "
              "ORDER BY l.l_orderkey, l.l_linenumber " + lim),
        "100000");
  case 15:
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice FROM lineitem "
              "WHERE l_shipdate >= DATE '1994-11-01' "
              "AND l_shipdate < DATE_ADD(DATE '1994-11-01', INTERVAL 3 MONTH) "
              "AND l_suppkey = (SELECT l_suppkey FROM lineitem "
              "WHERE l_shipdate >= DATE '1994-11-01' "
              "AND l_shipdate < DATE_ADD(DATE '1994-11-01', INTERVAL 3 MONTH) "
              "GROUP BY l_suppkey "
              "ORDER BY SUM(l_extendedprice * (1 - l_discount)) DESC LIMIT 1) "
              "ORDER BY l_orderkey, l_linenumber " + lim),
        "100");
  case 16: {
    // Flip an eligible supplier into the Customer Complaints set via comment.
    // Supplier eligibility is ApplyDelta; rewriting ps_suppkey forces RebuildAll.
    auto f = Fetch(
        "SELECT s.s_suppkey, s.s_comment FROM supplier s "
        "WHERE s.s_comment NOT LIKE '%Customer%Complaints%' "
        "AND EXISTS ("
        "  SELECT 1 FROM partsupp ps, part p "
        "  WHERE ps.ps_suppkey = s.s_suppkey AND p.p_partkey = ps.ps_partkey "
        "  AND p.p_brand <> 'Brand#13' AND p.p_type NOT LIKE 'SMALL BRUSHED%' "
        "  AND p.p_size IN (26, 24, 27, 29, 7, 20, 22, 33)"
        ") "
        "ORDER BY s.s_suppkey " +
        lim);
    Require(f.size() >= 2, "Q16 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE supplier SET s_comment = CONCAT(s_comment, "
                   "' Customer Complaints') WHERE s_suppkey = " +
                   f[0];
    p.restore_sql = "UPDATE supplier SET s_comment = " + SqlLiteral(f[1]) +
                    " WHERE s_suppkey = " + f[0];
    p.candidate = candidate_offset;
    return p;
  }
  case 17: {
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity, l.l_partkey, "
        "p.p_brand, p.p_container FROM lineitem l JOIN part p "
        "ON p.p_partkey = l.l_partkey WHERE l.l_partkey IN ("
        "SELECT l_partkey FROM lineitem GROUP BY l_partkey HAVING COUNT(*) >= 2) "
        "ORDER BY l.l_quantity, l.l_orderkey, l.l_linenumber " + lim);
    Require(f.size() >= 6, "Q17 probe fields");
    WriteProbe p;
    p.update_sql =
        "UPDATE part SET p_brand = 'Brand#34', p_container = 'LG CASE' "
        "WHERE p_partkey = " +
        f[3] + "; UPDATE lineitem SET l_quantity = 1 WHERE l_orderkey = " +
        f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql =
        "UPDATE lineitem SET l_quantity = " + f[2] + " WHERE l_orderkey = " +
        f[0] + " AND l_linenumber = " + f[1] +
        "; UPDATE part SET p_brand = " + SqlLiteral(f[4]) +
        ", p_container = " + SqlLiteral(f[5]) + " WHERE p_partkey = " + f[3];
    p.candidate = candidate_offset;
    return p;
  }
  case 18: {
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity FROM lineitem l "
        "WHERE l.l_orderkey = (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
        "ORDER BY SUM(l_quantity) DESC, l_orderkey LIMIT 1) "
        "ORDER BY l.l_linenumber " + lim);
    Require(f.size() >= 3, "Q18 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 50 "
                   "WHERE l_orderkey = " +
                   f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_quantity = " + f[2] +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.candidate = candidate_offset;
    return p;
  }
  case 19: {
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_partkey, l.l_linenumber, l.l_quantity, "
        "p.p_brand, p.p_container, p.p_size "
        "FROM lineitem l, part p WHERE p.p_partkey = l.l_partkey "
        "AND l.l_shipmode IN ('AIR','AIR REG') "
        "AND l.l_shipinstruct = 'DELIVER IN PERSON' "
        "ORDER BY l.l_orderkey, l.l_linenumber " + lim);
    Require(f.size() >= 7, "Q19 probe fields");
    WriteProbe p;
    p.update_sql =
        "UPDATE part SET p_brand = 'Brand#31', p_container = 'SM CASE', p_size = 3 "
        "WHERE p_partkey = " +
        f[1] +
        "; UPDATE lineitem SET l_quantity = 10 WHERE l_orderkey = " + f[0] +
        " AND l_partkey = " + f[1] + " AND l_linenumber = " + f[2];
    p.restore_sql =
        "UPDATE lineitem SET l_quantity = " + f[3] + " WHERE l_orderkey = " +
        f[0] + " AND l_partkey = " + f[1] + " AND l_linenumber = " + f[2] +
        "; UPDATE part SET p_brand = " + SqlLiteral(f[4]) +
        ", p_container = " + SqlLiteral(f[5]) + ", p_size = " + f[6] +
        " WHERE p_partkey = " + f[1];
    p.candidate = candidate_offset;
    return p;
  }
  case 20: {
    auto f = Fetch(
        "SELECT p.p_partkey, p.p_name FROM part p "
        "JOIN lineitem l ON l.l_partkey = p.p_partkey "
        "JOIN supplier s ON s.s_suppkey = l.l_suppkey "
        "JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'RUSSIA' "
        "WHERE l.l_shipdate >= DATE '1997-01-01' AND l.l_shipdate < DATE '1998-01-01' "
        "ORDER BY p.p_partkey " + lim);
    Require(f.size() >= 2, "Q20 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE part SET p_name = CONCAT('gainsboro ', p_name) "
                   "WHERE p_partkey = " +
                   f[0];
    p.restore_sql = "UPDATE part SET p_name = " + SqlLiteral(f[1]) +
                    " WHERE p_partkey = " + f[0];
    p.candidate = candidate_offset;
    return p;
  }
  case 21: {
    auto f = Fetch(
        "SELECT l1.l_orderkey, l1.l_linenumber, l1.l_commitdate, "
        "l1.l_receiptdate "
        "FROM supplier s, lineitem l1, orders o, nation n "
        "WHERE s.s_suppkey = l1.l_suppkey AND o.o_orderkey = l1.l_orderkey "
        "AND o.o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate "
        "AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey "
        "AND l2.l_suppkey <> l1.l_suppkey) "
        "AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey "
        "AND l3.l_suppkey = l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate "
        "AND l3.l_linenumber <> l1.l_linenumber) "
        "AND s.s_nationkey = n.n_nationkey AND n.n_name = 'SAUDI ARABIA' "
        "ORDER BY l1.l_orderkey, l1.l_linenumber " + lim);
    Require(f.size() >= 4, "Q21 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[2]) +
                   " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[3]) +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.candidate = candidate_offset;
    return p;
  }
  case 22: {
    // One orders GROUP BY, not a correlated COUNT(*) per customer (PK-only
    // SF=1 has no o_custkey index; the correlated form ran for 30+ minutes).
    auto f = Fetch(
        "SELECT c.c_custkey, s.o_orderkey, s.o_custkey "
        "FROM customer c JOIN ("
        "SELECT o_custkey, MIN(o_orderkey) AS o_orderkey "
        "FROM orders GROUP BY o_custkey HAVING COUNT(*) = 1"
        ") s ON s.o_custkey = c.c_custkey "
        "WHERE SUBSTRING(c.c_phone, 1, 2) IN "
        "('30','19','14','28','11','22','32') "
        "AND c.c_acctbal > (SELECT AVG(c_acctbal) FROM customer "
        "WHERE c_acctbal > 0.00 AND SUBSTRING(c_phone, 1, 2) IN "
        "('30','19','14','28','11','22','32')) "
        "ORDER BY c.c_custkey " + lim);
    Require(f.size() >= 3, "Q22 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE orders SET o_custkey = 0 WHERE o_orderkey = " + f[1];
    p.restore_sql = "UPDATE orders SET o_custkey = " + f[2] +
                    " WHERE o_orderkey = " + f[1];
    p.candidate = candidate_offset;
    return p;
  }
  default:
    throw std::runtime_error("no write probe for Q" + std::to_string(query_num));
  }
}

inline bool ProbeChangesBaseFingerprint(const Tools &tools,
                                        const std::string &base_sql,
                                        const std::string &update_sql,
                                        const std::string &restore_sql) {
  auto before = QuerySQL(tools, base_sql);
  Require(before.first, "probe before SQL failed");
  Require(RunSQL(tools, update_sql), "probe update failed: " + update_sql);
  auto after = QuerySQL(tools, base_sql);
  Require(after.first, "probe after SQL failed");
  RunSQL(tools, restore_sql);
  return TrimLines(before.second) != TrimLines(after.second);
}

inline WriteProbe PickInvalidatingWriteProbe(const Tools &tools, int query_num,
                                            const std::string &base_sql,
                                            int max_candidates = 64) {
  WriteProbe probe;
  for (int cand = 0; cand < max_candidates; ++cand) {
    try {
      probe = ResolveWriteProbe(tools, query_num, cand);
    } catch (const std::exception &ex) {
      if (cand == 0) {
        throw std::runtime_error("write probe Q" + std::to_string(query_num) +
                                 " failed: " + ex.what());
      }
      break;
    }
    if (ProbeChangesBaseFingerprint(tools, base_sql, probe.update_sql,
                                    probe.restore_sql)) {
      return probe;
    }
  }
  throw std::runtime_error(
      "invalidating probe did not change base SQL fingerprint for Q" +
      std::to_string(query_num) +
      " — write must impact this query's observable result");
}

// Setup check for write-mix benches: the probe must change native SQL *and*
// the aux Execute answer, and must not be a maintenance NoOp.
inline WriteProbe RequireInvalidatingAuxWrite(const Tools &tools, int query_num,
                                              const std::string &base_sql,
                                              const std::string &aux_sql,
                                              int max_candidates = 64) {
  WriteProbe probe;
  for (int cand = 0; cand < max_candidates; ++cand) {
    try {
      probe = ResolveWriteProbe(tools, query_num, cand);
    } catch (const std::exception &ex) {
      if (cand == 0) {
        throw std::runtime_error("write probe Q" + std::to_string(query_num) +
                                 " failed: " + ex.what());
      }
      break;
    }
    auto before_sql = QuerySQL(tools, base_sql);
    Require(before_sql.first, "probe before SQL failed");
    auto before_aux = QuerySQL(tools, aux_sql);
    Require(before_aux.first, "probe before aux failed: " + before_aux.second);
    Require(RunSQL(tools, probe.update_sql),
            "probe update failed: " + probe.update_sql);
    auto after_sql = QuerySQL(tools, base_sql);
    Require(after_sql.first, "probe after SQL failed");
    auto after_aux = QuerySQL(tools, aux_sql);
    Require(after_aux.first, "probe after aux failed: " + after_aux.second);
    auto action_q =
        QuerySQL(tools, "SELECT accel_get_last_maintenance_action()");
    const std::string action =
        action_q.first ? TrimLines(action_q.second) : "error";
    RunSQL(tools, probe.restore_sql);
    const bool sql_changed =
        TrimLines(before_sql.second) != TrimLines(after_sql.second);
    const bool aux_changed =
        TrimLines(before_aux.second) != TrimLines(after_aux.second);
    if (!sql_changed)
      continue;
    Require(aux_changed,
            "Q" + std::to_string(query_num) +
                " write changed native SQL but aux Execute was unchanged "
                "(action=" +
                action + ")");
    Require(action != "NoOp" && action != "None" && action != "error",
            "Q" + std::to_string(query_num) +
                " write changed results but maintenance action was " + action);
    return probe;
  }
  throw std::runtime_error(
      "no write probe changed native SQL and aux state for Q" +
      std::to_string(query_num));
}

} // namespace accel::mysql::tpch_harness
