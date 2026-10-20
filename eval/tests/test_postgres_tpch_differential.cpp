// PostgreSQL TPC-H differential harness.
// Builds: test_postgres_tpch_differential
//
// Modes (ACCEL_PG_DIFF_MODE):
//   read (default) — ordered SQL vs accelerator/aux match (hard fail)
//   write_maint    — read match, then mutation + maintenance + re-match
// Env: TPCH_PG_DB, ACCEL_PG_DIFF_KIND=base|aux, ACCEL_WORKLOAD_KIND

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eval/tests/postgres/postgres_tpch_harness.hpp"

namespace {

using accel::postgres::Tools;
using namespace accel::postgres::tpch_harness;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
using accelerator_codegen::aux_state::engine::postgres::PostgresSharedQueryCatalogEntry;

void RunReadDifferential(const Tools &tools, int query_num, bool prefer_aux) {
  const auto &entry = LookupPostgresQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, prefer_aux);

  std::cerr << "differential(read): tpch Q" << query_num
            << (prefer_aux ? " aux" : " base") << '\n';
  std::cerr << "  reference: " << ref_sql << '\n';
  std::cerr << "  accel: " << accel_sql << '\n';

  RequireQueryMatch(tools, query_num, prefer_aux, "read");
  std::cerr << "PASS read differential Q" << query_num << '\n';
}

// Resolve a concrete invalidating UPDATE + restore for this query (DuckDB bar:
// write must change base SQL fingerprint; restore uses exact keys/values).
struct WriteProbe {
  std::string update_sql;
  std::string restore_sql;
};

std::vector<std::string> SplitFields(const std::string &line) {
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

WriteProbe ResolveWriteProbe(const Tools &tools, int query_num, int candidate_offset) {
  auto Fetch = [&](const std::string &sql) {
    auto r = QuerySQL(tools, sql);
    Require(r.first, "probe select failed for Q" + std::to_string(query_num) +
                         ":\n" + sql + "\n" + r.second);
    auto lines = SplitLines(TrimLines(r.second));
    Require(!lines.empty(), "probe select returned no rows for Q" +
                                std::to_string(query_num) + " offset=" +
                                std::to_string(candidate_offset));
    return SplitFields(lines[0]);
  };
  const std::string off = " OFFSET " + std::to_string(candidate_offset) + " ";
  auto LinePriceBump = [](const std::vector<std::string> &f, const char *delta) {
    // f: orderkey|linenumber|extendedprice
    Require(f.size() >= 3, "lineitem price probe needs 3 fields");
    WriteProbe p;
    // At least the row's own value, so the bump survives a x3000 money scale.
    p.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + "
                   "GREATEST(" + std::string(delta) + ", l_extendedprice) WHERE l_orderkey = " + f[0] +
                   " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_extendedprice = " + f[2] +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    return p;
  };

  switch (query_num) {
  case 1:
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice::text FROM lineitem "
              "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
              "ORDER BY l_orderkey, l_linenumber " + off + "LIMIT 1"),
        "100");
  case 2: {
    auto f = Fetch(
        "SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost::text "
        "FROM partsupp ps WHERE (ps.ps_partkey, ps.ps_suppkey) = ("
        "SELECT p.p_partkey, s.s_suppkey FROM part p, supplier s, partsupp ps2, "
        "nation n, region r "
        "WHERE p.p_partkey = ps2.ps_partkey AND s.s_suppkey = ps2.ps_suppkey "
        "AND p.p_size = 15 AND p.p_type LIKE '%BRASS' "
        "AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey "
        "AND r.r_name = 'EUROPE' "
        "AND ps2.ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, "
        "nation, region WHERE p.p_partkey = ps_partkey AND s_suppkey = ps_suppkey "
        "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
        "AND r_name = 'EUROPE') "
        "ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey " + off +
        "LIMIT 1)");
    Require(f.size() >= 3, "Q2 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + GREATEST(1000, ps_supplycost) "
                   "WHERE ps_partkey = " +
                   f[0] + " AND ps_suppkey = " + f[1];
    p.restore_sql = "UPDATE partsupp SET ps_supplycost = " + f[2] +
                    " WHERE ps_partkey = " + f[0] + " AND ps_suppkey = " + f[1];
    return p;
  }
  case 3:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l WHERE l.l_orderkey IN ("
              "SELECT l_orderkey FROM ("
              "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
              "o_orderdate, o_shippriority "
              "FROM customer, orders, lineitem "
              "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey "
              "AND l_orderkey = o_orderkey AND o_orderdate < DATE '1995-03-15' "
              "AND l_shipdate > DATE '1995-03-15' "
              "GROUP BY l_orderkey, o_orderdate, o_shippriority "
              "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)"
              ") AND l.l_shipdate > DATE '1995-03-15' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 4: {
    // Clear late-receipt on the sole late line of a windowed order (price never
    // changes Q4 counts). Mirrors DuckDB BuildTpchQ4Probe.
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_commitdate::text, "
        "l.l_receiptdate::text FROM lineitem l "
        "WHERE l.l_commitdate < l.l_receiptdate AND l.l_orderkey = ("
        "SELECT o.o_orderkey FROM orders o "
        "WHERE o.o_orderdate >= DATE '1993-07-01' "
        "AND o.o_orderdate < DATE '1993-07-01' + INTERVAL '3' month "
        "AND (SELECT COUNT(*) FROM lineitem lx "
        "WHERE lx.l_orderkey = o.o_orderkey AND lx.l_commitdate < lx.l_receiptdate) = 1 "
        "ORDER BY o.o_orderkey LIMIT 1) "
        "ORDER BY l.l_linenumber " + off + "LIMIT 1");
    Require(f.size() >= 4, "Q4 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[2]) +
                   " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[3]) +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    return p;
  }
  case 5:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, orders o, customer c, supplier s, nation n, region r "
              "WHERE l.l_orderkey = o.o_orderkey AND c.c_custkey = o.o_custkey "
              "AND l.l_suppkey = s.s_suppkey AND c.c_nationkey = s.s_nationkey "
              "AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey "
              "AND r.r_name = 'AMERICA' AND o.o_orderdate >= DATE '1995-01-01' "
              "AND o.o_orderdate < DATE '1995-01-01' + INTERVAL '1' year "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 6:
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice::text FROM lineitem "
              "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
              "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25 "
              "ORDER BY l_orderkey, l_linenumber " + off + "LIMIT 1"),
        "100");
  case 7:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, supplier s, orders o, customer c, nation n1, nation n2 "
              "WHERE s.s_suppkey = l.l_suppkey AND o.o_orderkey = l.l_orderkey "
              "AND c.c_custkey = o.o_custkey AND s.s_nationkey = n1.n_nationkey "
              "AND c.c_nationkey = n2.n_nationkey "
              "AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL') "
              "OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM')) "
              "AND l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 8:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM part p, supplier s, lineitem l, orders o, customer c, "
              "nation n1, nation n2, region r "
              "WHERE p.p_partkey = l.l_partkey AND s.s_suppkey = l.l_suppkey "
              "AND l.l_orderkey = o.o_orderkey AND o.o_custkey = c.c_custkey "
              "AND c.c_nationkey = n1.n_nationkey AND n1.n_regionkey = r.r_regionkey "
              "AND r.r_name = 'ASIA' AND s.s_nationkey = n2.n_nationkey "
              "AND o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
              "AND p.p_type = 'PROMO BRUSHED BRASS' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 9:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM part p, lineitem l WHERE p.p_partkey = l.l_partkey "
              "AND p.p_name LIKE '%green%' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 10:
    // Must hit a TOP-20 customer so LIMIT 20 fingerprint moves (DuckDB Q10).
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, orders o "
              "WHERE l.l_orderkey = o.o_orderkey AND l.l_returnflag = 'R' "
              "AND o.o_orderdate >= DATE '1993-06-01' "
              "AND o.o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH "
              "AND o.o_custkey IN ("
              "SELECT c_custkey FROM ("
              "SELECT c_custkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
              "FROM customer JOIN orders ON c_custkey = o_custkey "
              "JOIN lineitem ON l_orderkey = o_orderkey "
              "WHERE o_orderdate >= DATE '1993-06-01' "
              "AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH "
              "AND l_returnflag = 'R' "
              "GROUP BY c_custkey ORDER BY revenue DESC, c_custkey LIMIT 20) top20) "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 11: {
    auto f = Fetch(
        "SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost::text "
        "FROM partsupp ps, supplier s, nation n "
        "WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey "
        "AND n.n_name = 'ROMANIA' ORDER BY ps.ps_partkey, ps.ps_suppkey " + off + "LIMIT 1");
    Require(f.size() >= 3, "Q11 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + GREATEST(100, ps_supplycost) "
                   "WHERE ps_partkey = " +
                   f[0] + " AND ps_suppkey = " + f[1];
    p.restore_sql = "UPDATE partsupp SET ps_supplycost = " + f[2] +
                    " WHERE ps_partkey = " + f[0] + " AND ps_suppkey = " + f[1];
    return p;
  }
  case 12: {
    // Counts by shipmode — flip MAIL out of {'MAIL','SHIP'} (DuckDB Q12 probe).
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_shipmode FROM orders o, lineitem l "
        "WHERE o.o_orderkey = l.l_orderkey AND l.l_shipmode IN ('MAIL', 'SHIP') "
        "AND l.l_commitdate < l.l_receiptdate AND l.l_shipdate < l.l_commitdate "
        "AND l.l_receiptdate >= DATE '1995-01-01' "
        "AND l.l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year "
        "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1");
    Require(f.size() >= 3, "Q12 probe fields");
    const std::string neu = (f[2] == "MAIL") ? "AIR" : "MAIL";
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_shipmode = " + SqlLiteral(neu) +
                   " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_shipmode = " + SqlLiteral(f[2]) +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    return p;
  }
  case 13: {
    auto f = Fetch(
        "SELECT o_orderkey, o_comment FROM orders "
        "WHERE o_comment NOT LIKE '%unusual%accounts%' "
        "ORDER BY o_orderkey " + off + "LIMIT 1");
    Require(f.size() >= 2, "Q13 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE orders SET o_comment = o_comment || ' unusual accounts' "
                   "WHERE o_orderkey = " +
                   f[0];
    p.restore_sql = "UPDATE orders SET o_comment = " + SqlLiteral(f[1]) +
                    " WHERE o_orderkey = " + f[0];
    return p;
  }
  case 14:
    // Large bump so promo_revenue fingerprint moves (DuckDB uses +1e7).
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, part p WHERE l.l_partkey = p.p_partkey "
              "AND l.l_shipdate >= DATE '1995-06-01' "
              "AND l.l_shipdate < DATE '1995-06-01' + INTERVAL '1' month "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "10000000");
  case 15:
    // Bump a line belonging to the current max-revenue supplier (DuckDB Q15).
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice::text FROM lineitem "
              "WHERE l_shipdate >= DATE '1994-11-01' "
              "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
              "AND l_suppkey = (SELECT l_suppkey FROM lineitem "
              "WHERE l_shipdate >= DATE '1994-11-01' "
              "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
              "GROUP BY l_suppkey "
              "ORDER BY SUM(l_extendedprice * (1 - l_discount)) DESC LIMIT 1) "
              "ORDER BY l_orderkey, l_linenumber " + off + "LIMIT 1"),
        "100");
  case 16: {
    // Move the row onto a complaints supplier so COUNT(DISTINCT) drops.
    auto f = Fetch(
        "SELECT ps.ps_partkey, ps.ps_suppkey, "
        "(SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%' "
        "ORDER BY s_suppkey LIMIT 1)::text "
        "FROM partsupp ps, part p "
        "WHERE p.p_partkey = ps.ps_partkey AND p.p_brand <> 'Brand#13' "
        "AND p.p_type NOT LIKE 'SMALL BRUSHED%' "
        "AND p.p_size IN (26, 24, 27, 29, 7, 20, 22, 33) "
        "AND ps.ps_suppkey NOT IN ("
        "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
        "AND NOT EXISTS (SELECT 1 FROM partsupp ps2 WHERE ps2.ps_partkey = ps.ps_partkey "
        "AND ps2.ps_suppkey = (SELECT s_suppkey FROM supplier "
        "WHERE s_comment LIKE '%Customer%Complaints%' ORDER BY s_suppkey LIMIT 1)) "
        "ORDER BY ps.ps_partkey, ps.ps_suppkey " + off + "LIMIT 1");
    Require(f.size() >= 3, "Q16 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE partsupp SET ps_suppkey = " + f[2] +
                   " WHERE ps_partkey = " + f[0] + " AND ps_suppkey = " + f[1];
    p.restore_sql = "UPDATE partsupp SET ps_suppkey = " + f[1] +
                    " WHERE ps_partkey = " + f[0] + " AND ps_suppkey = " + f[2];
    return p;
  }
  case 17:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, part p WHERE p.p_partkey = l.l_partkey "
              "AND p.p_brand = 'Brand#34' AND p.p_container = 'LG CASE' "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 18: {
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity::text FROM lineitem l "
        "WHERE l.l_orderkey IN ("
        "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
        "HAVING SUM(l_quantity) > 312) "
        "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1");
    Require(f.size() >= 3, "Q18 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 1 "
                   "WHERE l_orderkey = " +
                   f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_quantity = " + f[2] +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    return p;
  }
  case 19: {
    // Quantity bump exits Q19 brand/qty window (price alone is too weak).
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_partkey, l.l_linenumber, l.l_quantity::text "
        "FROM lineitem l, part p WHERE p.p_partkey = l.l_partkey "
        "AND p.p_brand = 'Brand#31' AND p.p_container IN "
        "('SM CASE','SM BOX','SM PACK','SM PKG') "
        "AND l.l_quantity >= 4 AND l.l_quantity <= 14 "
        "AND p.p_size BETWEEN 1 AND 5 "
        "AND l.l_shipmode IN ('AIR','AIR REG') "
        "AND l.l_shipinstruct = 'DELIVER IN PERSON' "
        "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1");
    Require(f.size() >= 4, "Q19 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 100 "
                   "WHERE l_orderkey = " +
                   f[0] + " AND l_partkey = " + f[1] + " AND l_linenumber = " +
                   f[2];
    p.restore_sql = "UPDATE lineitem SET l_quantity = " + f[3] +
                    " WHERE l_orderkey = " + f[0] + " AND l_partkey = " + f[1] +
                    " AND l_linenumber = " + f[2];
    return p;
  }
  case 20: {
    // Target a RUSSIA / gainsboro line that feeds Q20 availability (DuckDB probe).
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity::text "
        "FROM lineitem l "
        "JOIN part p ON p.p_partkey = l.l_partkey AND p.p_name LIKE 'gainsboro%' "
        "JOIN partsupp ps ON ps.ps_partkey = l.l_partkey AND ps.ps_suppkey = l.l_suppkey "
        "JOIN supplier s ON s.s_suppkey = l.l_suppkey "
        "JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'RUSSIA' "
        "WHERE l.l_shipdate >= DATE '1997-01-01' "
        "AND l.l_shipdate < DATE '1998-01-01' "
        "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1");
    Require(f.size() >= 3, "Q20 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 100000 "
                   "WHERE l_orderkey = " +
                   f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_quantity = " + f[2] +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    return p;
  }
  case 21: {
    // Clear late receipt on a waiting Saudi line (DuckDB Q21 probe).
    auto f = Fetch(
        "SELECT l1.l_orderkey, l1.l_linenumber, l1.l_commitdate::text, "
        "l1.l_receiptdate::text "
        "FROM supplier s, lineitem l1, orders o, nation n "
        "WHERE s.s_suppkey = l1.l_suppkey AND o.o_orderkey = l1.l_orderkey "
        "AND o.o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate "
        "AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey "
        "AND l2.l_suppkey <> l1.l_suppkey) "
        "AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey "
        "AND l3.l_suppkey = l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate "
        "AND l3.l_linenumber <> l1.l_linenumber) "
        "AND s.s_nationkey = n.n_nationkey AND n.n_name = 'SAUDI ARABIA' "
        "ORDER BY l1.l_orderkey, l1.l_linenumber " + off + "LIMIT 1");
    Require(f.size() >= 4, "Q21 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[2]) +
                   " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE " + SqlLiteral(f[3]) +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    return p;
  }
  case 22: {
    auto f = Fetch(
        "SELECT c_custkey, c_acctbal::text FROM customer "
        "WHERE substring(c_phone from 1 for 2) IN "
        "('30','19','14','28','11','22','32') "
        "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
        "AND substring(c_phone from 1 for 2) IN "
        "('30','19','14','28','11','22','32')) "
        "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey) "
        "ORDER BY c_custkey " + off + "LIMIT 1");
    Require(f.size() >= 2, "Q22 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE customer SET c_acctbal = c_acctbal + GREATEST(100, abs(c_acctbal)) "
                   "WHERE c_custkey = " +
                   f[0];
    p.restore_sql = "UPDATE customer SET c_acctbal = " + f[1] +
                    " WHERE c_custkey = " + f[0];
    return p;
  }
  default:
    throw std::runtime_error("no write probe for Q" + std::to_string(query_num));
  }
}

std::string SectionAfter(const std::string &blob, const std::string &marker) {
  const auto pos = blob.find(marker);
  Require(pos != std::string::npos, "missing marker " + marker);
  auto start = pos + marker.size();
  while (start < blob.size() && (blob[start] == '\n' || blob[start] == '\r')) {
    ++start;
  }
  const auto next = blob.find("===", start);
  if (next == std::string::npos) {
    return blob.substr(start);
  }
  return blob.substr(start, next - start);
}

bool ProbeChangesBaseFingerprint(const Tools &tools, const std::string &base_sql,
                                 const std::string &update_sql) {
  // Must be one backend session: separate psql -c calls do not share a txn.
  std::ostringstream script;
  script << "SELECT '===BEFORE===' AS m;\n"
         << base_sql << ";\n"
         << "BEGIN;\n"
         << update_sql << ";\n"
         << "SELECT '===AFTER===' AS m;\n"
         << base_sql << ";\n"
         << "ROLLBACK;\n";
  const std::string out = RunPsqlScript(tools, script.str());
  return TrimLines(SectionAfter(out, "===BEFORE===")) !=
         TrimLines(SectionAfter(out, "===AFTER==="));
}

void RunWriteMaintDifferential(const Tools &tools, int query_num, bool prefer_aux,
                               const std::string &force_action) {
  Require(prefer_aux, "write_maint requires aux (ACCEL_PG_DIFF_KIND=aux)");
  RunReadDifferential(tools, query_num, prefer_aux);
  EnsureMutationTriggers(tools);

  const auto &entry = LookupPostgresQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, /*prefer_aux=*/true);

  WriteProbe probe;
  bool found = false;
  for (int cand = 0; cand < 64; ++cand) {
    try {
      probe = ResolveWriteProbe(tools, query_num, cand);
    } catch (const std::exception &) {
      break;
    }
    if (ProbeChangesBaseFingerprint(tools, ref_sql, probe.update_sql)) {
      found = true;
      break;
    }
  }
  Require(found, "invalidating probe did not change base SQL fingerprint for Q" +
                     std::to_string(query_num) +
                     " — write must impact this query's observable result");

  // Commits shipped to the accelerators' shard workers, before and after the write.
  const std::string stats = "SELECT accel_shard_stats();\n";
  std::ostringstream script;
  script << "LOAD '" << tools.extension_so << "';\n"
         << "SELECT '===WARM===' AS m;\n"
         << accel_sql << ";\n"
         // A cold read answers from SQL while the build runs: wait for the state.
         << "DO $$ BEGIN FOR i IN 1..1200 LOOP "
            "EXIT WHEN substring(accel_shard_stats() from 'synced=([0-9]+)')::bigint >= 1; "
            "PERFORM pg_sleep(0.05); END LOOP; END $$;\n";
  // The accelerator's worker exists once it was read: force from here on.
  script << "SELECT accel_set_force_maintenance_action("
         << SqlLiteral(force_action) << ");\n"
         << "SELECT '===MUT_BEFORE===' AS m;\n"
         << stats << probe.update_sql << ";\n"
         // The read answers from the state once it can serve (or from SQL when
         // waiting costs more); the worker applies the commit and publishes its
         // decision either way: wait for that.
         << "SELECT '===ACCEL===' AS m;\n"
         << accel_sql << ";\n"
         << "DO $$ DECLARE s text; BEGIN FOR i IN 1..600 LOOP s := accel_shard_stats(); "
            "EXIT WHEN substring(s from 'applied=([0-9]+)')::bigint >= substring(s from 'commits=([0-9]+)')::bigint; "
            "PERFORM pg_sleep(0.05); END LOOP; END $$;\n"
         << "SELECT pg_sleep(0.5);\n"
         << "SELECT '===MUT_AFTER===' AS m;\n"
         << stats
         << "SELECT '===ACTION===' AS m;\n"
         << "SELECT accel_get_last_maintenance_action();\n"
         << "SELECT '===REF===' AS m;\n"
         << ref_sql << ";\n"
         << "SELECT '===DONE===' AS m;\n"
         << probe.restore_sql << ";\n"
         << "SELECT accel_set_force_maintenance_action('');\n";

  const std::string session = RunPsqlScript(tools, script.str());
  const auto mut_before = TrimLines(SectionAfter(session, "===MUT_BEFORE==="));
  const auto mut_after = TrimLines(SectionAfter(session, "===MUT_AFTER==="));
  const auto action_out = TrimLines(SectionAfter(session, "===ACTION==="));
  const auto accel_out = SectionAfter(session, "===ACCEL===");
  const auto ref_out = SectionAfter(session, "===REF===");

  auto commits_of = [](const std::string &text) {
    const auto pos = text.find("commits=");
    if (pos == std::string::npos)
      throw std::runtime_error("failed to parse shard stats [" + text + "]");
    return std::stoi(text.substr(pos + 8));
  };
  const int before_n = commits_of(mut_before);
  const int after_n = commits_of(mut_after);
  Require(after_n > before_n,
          "no commit reached the shard worker (before=" + std::to_string(before_n) +
              " after=" + std::to_string(after_n) +
              ") — triggers must capture the write into aux maintenance");

  std::string action = action_out;
  while (!action.empty() && (action.back() == '\n' || action.back() == '\r'))
    action.pop_back();
  auto ActionOk = [](const std::string &got, std::initializer_list<const char *> ok) {
    for (const char *a : ok) {
      if (got == a)
        return true;
    }
    return false;
  };
  // Rematch is the correctness bar. Forced actions may escalate when a module
  // has no ApplyDelta path, or when Postgres defers Rebuild* to EnsureBuilt.
  if (force_action == "ApplyDelta") {
    Require(ActionOk(action, {"ApplyDelta", "RebuildPartition", "RebuildAll",
                              "InvalidateAndRebuildOnRead"}),
            "forced ApplyDelta produced unexpected action=[" + action + "]");
  } else if (force_action == "RebuildPartition") {
    Require(ActionOk(action, {"RebuildPartition", "RebuildAll",
                              "InvalidateAndRebuildOnRead"}),
            "forced RebuildPartition produced unexpected action=[" + action + "]");
  } else if (force_action == "RebuildAll") {
    Require(ActionOk(action, {"RebuildAll", "InvalidateAndRebuildOnRead"}),
            "forced RebuildAll produced unexpected action=[" + action + "]");
  } else {
    Require(!action.empty() && action != "None",
            "policy write_maint recorded no maintenance action");
  }

  RequireOrderedMatch(ref_out, accel_out,
                      force_action.empty() ? "write_maint"
                                           : ("write_maint_" + force_action));
  std::cerr << "PASS write_maint differential Q" << query_num
            << (force_action.empty() ? "" : (" force=" + force_action))
            << " (invalidating write + shipped commits " << before_n << "->" << after_n
            << " + aux rematch action=[" << action << "])\n";
}

std::vector<std::string> MaintenanceStrats() {
  const char *raw = std::getenv("ACCEL_PG_DIFF_MAINT_STRATS");
  const char *skip = std::getenv("ACCEL_PG_DIFF_SKIP_MAINT_STRATS");
  if (skip && std::string(skip) == "1")
    return {};
  std::string src = raw && *raw ? raw : "ApplyDelta,RebuildPartition,RebuildAll";
  if (src == "none" || src == "None")
    return {};
  std::vector<std::string> out;
  std::stringstream stream(src);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty())
      out.push_back(token);
  }
  return out;
}

void RunWriteMixAndStrats(const Tools &tools, int query_id, bool prefer_aux) {
  RunWriteMaintDifferential(tools, query_id, prefer_aux, "");
  const char *force_env = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  if (force_env && *force_env) {
    RunWriteMaintDifferential(tools, query_id, prefer_aux, force_env);
    return;
  }
  for (const auto &strat : MaintenanceStrats()) {
    RunWriteMaintDifferential(tools, query_id, prefer_aux, strat);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    int query_id = 1;
    if (argc >= 2) {
      query_id = std::stoi(argv[1]);
    }

    auto tools = ConnectTpch();
    std::cerr << "postgres differential: " << tools.host << ":" << tools.port
              << " db=" << tools.database << '\n';

    const bool prefer_aux = PreferAux(query_id);
    const char *mode = std::getenv("ACCEL_PG_DIFF_MODE");
    const std::string mode_s = mode ? mode : "read";

    if (mode_s == "write_maint") {
      RunWriteMixAndStrats(tools, query_id, prefer_aux);
    } else {
      RunReadDifferential(tools, query_id, prefer_aux);
      if (prefer_aux && (!mode || mode_s == "read")) {
        const char *skip = std::getenv("ACCEL_PG_DIFF_SKIP_WRITE_MAINT");
        if (!(skip && std::string(skip) == "1")) {
          RunWriteMixAndStrats(tools, query_id, prefer_aux);
        }
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_postgres_tpch_differential failed: " << error.what() << '\n';
    return 1;
  }
}
