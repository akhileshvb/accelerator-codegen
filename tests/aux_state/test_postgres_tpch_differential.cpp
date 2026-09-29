// PostgreSQL TPC-H differential harness.
// Builds: test_postgres_tpch_differential
//
// Modes (ACCEL_PG_DIFF_MODE):
//   read (default) — ordered SQL vs accelerator/aux match (hard fail)
//   write_maint    — read match, then mutation + maintenance + re-match
// Env: TPCH_PG_DB, ACCEL_PG_DIFF_KIND=base|aux, ACCEL_WORKLOAD_KIND

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "aux_state/postgres_tpch_harness.hpp"
#include "tpch_arg_variants.hpp"

namespace {

using accel::postgres::Tools;
using namespace accel::postgres::tpch_harness;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
namespace core = accelerator_codegen::aux_state::core;
namespace aux_sweep = accelerator_codegen::benchmarks::aux_sweep;

void RequireSqlMatch(const Tools &tools, const std::string &ref_sql,
                     const std::string &accel_sql, const std::string &label) {
  auto ref = QuerySQL(tools, ref_sql);
  Require(ref.first, label + ": reference SQL failed:\n" + ref_sql + "\n" +
                         ref.second);
  auto accel = QuerySQL(tools, accel_sql);
  Require(accel.first, label + ": accel SQL failed:\n" + accel_sql + "\n" +
                           accel.second);
  RequireOrderedMatch(ref.second, accel.second, label);
}

void RunReadDifferential(const Tools &tools, int query_num, bool prefer_aux,
                         const aux_sweep::TpchArgVariant &variant) {
  const std::string ref_sql = variant.base_sql;
  const std::string accel_sql =
      prefer_aux ? variant.aux_sql : variant.kernel_sql;

  std::cerr << "differential(read): tpch Q" << query_num
            << (prefer_aux ? " aux" : " base") << " sql=" << accel_sql << '\n';

  RequireSqlMatch(tools, ref_sql, accel_sql, "read Q" + std::to_string(query_num));
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

WriteProbe ResolveWriteProbe(const Tools &tools, int query_num, int candidate_offset,
                             const core::QueryArguments &arguments) {
  auto S = [&](std::size_t i) {
    Require(arguments.values.size() > i &&
                std::holds_alternative<std::string>(arguments.values[i].payload),
            "expected string arg");
    return aux_sweep::FormatTpchSqlString(
        std::get<std::string>(arguments.values[i].payload));
  };
  auto D = [&](std::size_t i) {
    Require(arguments.values.size() > i &&
                std::holds_alternative<core::AuxDate>(arguments.values[i].payload),
            "expected date arg");
    return aux_sweep::FormatTpchDateLiteral(
        std::get<core::AuxDate>(arguments.values[i].payload));
  };
  auto I64 = [&](std::size_t i) {
    Require(arguments.values.size() > i &&
                std::holds_alternative<int64_t>(arguments.values[i].payload),
            "expected int arg");
    return std::get<int64_t>(arguments.values[i].payload);
  };
  auto Dbl = [&](std::size_t i) {
    Require(arguments.values.size() > i, "expected numeric arg");
    if (std::holds_alternative<double>(arguments.values[i].payload))
      return std::get<double>(arguments.values[i].payload);
    if (std::holds_alternative<int64_t>(arguments.values[i].payload))
      return static_cast<double>(std::get<int64_t>(arguments.values[i].payload));
    throw std::runtime_error("expected numeric arg");
  };
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
    p.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + " +
                   std::string(delta) + " WHERE l_orderkey = " + f[0] +
                   " AND l_linenumber = " + f[1];
    p.restore_sql = "UPDATE lineitem SET l_extendedprice = " + f[2] +
                    " WHERE l_orderkey = " + f[0] + " AND l_linenumber = " + f[1];
    return p;
  };

  switch (query_num) {
  case 1:
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice::text FROM lineitem "
              "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '" +
              std::to_string(I64(0)) + "' DAY "
              "ORDER BY l_orderkey, l_linenumber " + off + "LIMIT 1"),
        "100");
  case 2: {
    // Lower a non-winning regional partsupp below the current min so ApplyDelta
    // can install a new min (raising the unique min forces RebuildAll).
    auto f = Fetch(
        "SELECT ps.ps_partkey::text, ps.ps_suppkey::text, ps.ps_supplycost::text, "
        "mins.min_cost::text "
        "FROM partsupp ps "
        "JOIN supplier s ON s.s_suppkey = ps.ps_suppkey "
        "JOIN nation n ON n.n_nationkey = s.s_nationkey "
        "JOIN region r ON r.r_regionkey = n.n_regionkey "
        "JOIN part p ON p.p_partkey = ps.ps_partkey "
        "JOIN LATERAL ("
        "  SELECT MIN(ps2.ps_supplycost) AS min_cost "
        "  FROM partsupp ps2 "
        "  JOIN supplier s2 ON s2.s_suppkey = ps2.ps_suppkey "
        "  JOIN nation n2 ON n2.n_nationkey = s2.s_nationkey "
        "  JOIN region r2 ON r2.r_regionkey = n2.n_regionkey "
        "  WHERE ps2.ps_partkey = ps.ps_partkey AND r2.r_name = r.r_name"
        ") mins ON true "
        "WHERE r.r_name = " +
        S(2) + " AND p.p_size = " + std::to_string(I64(0)) +
        " AND p.p_type LIKE " + S(1) +
        " AND ps.ps_supplycost > mins.min_cost "
        "ORDER BY ps.ps_partkey, ps.ps_suppkey " + off + "LIMIT 1");
    Require(f.size() >= 4, "Q2 probe fields");
    const double min_cost = std::stod(f[3]);
    const double new_cost = (static_cast<int64_t>(std::llround(min_cost * 100.0)) - 1) /
                            100.0;
    WriteProbe p;
    p.update_sql = "UPDATE partsupp SET ps_supplycost = " + std::to_string(new_cost) +
                   " WHERE ps_partkey = " + f[0] + " AND ps_suppkey = " + f[1];
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
              "WHERE c_mktsegment = " + S(0) + " AND c_custkey = o_custkey "
              "AND l_orderkey = o_orderkey AND o_orderdate < " + D(1) + " "
              "AND l_shipdate > " + D(1) + " "
              "GROUP BY l_orderkey, o_orderdate, o_shippriority "
              "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)"
              ") AND l.l_shipdate > " + D(1) + " "
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
        "WHERE o.o_orderdate >= " + D(0) + " "
        "AND o.o_orderdate < " + D(0) + " + INTERVAL '3' month "
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
              "AND r.r_name = " + S(0) + " AND o.o_orderdate >= " + D(1) + " "
              "AND o.o_orderdate < " + D(1) + " + INTERVAL '1' year "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 6:
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice::text FROM lineitem "
              "WHERE l_shipdate >= " + D(0) + " AND l_shipdate < " + D(0) +
              " + INTERVAL '1' year "
              "AND l_discount BETWEEN " + std::to_string(Dbl(1) - 0.01) +
              " AND " + std::to_string(Dbl(1) + 0.01) +
              " AND l_quantity < " + std::to_string(Dbl(2)) + " "
              "ORDER BY l_orderkey, l_linenumber " + off + "LIMIT 1"),
        "100");
  case 7:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, supplier s, orders o, customer c, nation n1, nation n2 "
              "WHERE s.s_suppkey = l.l_suppkey AND o.o_orderkey = l.l_orderkey "
              "AND c.c_custkey = o.o_custkey AND s.s_nationkey = n1.n_nationkey "
              "AND c.c_nationkey = n2.n_nationkey "
              "AND ((n1.n_name = " + S(0) + " AND n2.n_name = " + S(1) + ") "
              "OR (n1.n_name = " + S(1) + " AND n2.n_name = " + S(0) + ")) "
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
              "AND r.r_name = " + S(1) + " AND s.s_nationkey = n2.n_nationkey "
              "AND o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
              "AND p.p_type = " + S(2) + " "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 9:
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM part p, lineitem l WHERE p.p_partkey = l.l_partkey "
              "AND p.p_name LIKE " + aux_sweep::FormatTpchSqlString(
                  "%" + std::get<std::string>(arguments.values[0].payload) + "%") +
              " ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 10:
    // Must hit a TOP-20 customer so LIMIT 20 fingerprint moves (DuckDB Q10).
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, orders o "
              "WHERE l.l_orderkey = o.o_orderkey AND l.l_returnflag = 'R' "
              "AND o.o_orderdate >= " + D(0) + " "
              "AND o.o_orderdate < " + D(0) + " + INTERVAL '3' MONTH "
              "AND o.o_custkey IN ("
              "SELECT c_custkey FROM ("
              "SELECT c_custkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
              "FROM customer JOIN orders ON c_custkey = o_custkey "
              "JOIN lineitem ON l_orderkey = o_orderkey "
              "WHERE o_orderdate >= " + D(0) + " "
              "AND o_orderdate < " + D(0) + " + INTERVAL '3' MONTH "
              "AND l_returnflag = 'R' "
              "GROUP BY c_custkey ORDER BY revenue DESC, c_custkey LIMIT 20) top20) "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 11: {
    auto f = Fetch(
        "SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost::text "
        "FROM partsupp ps, supplier s, nation n "
        "WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey "
        "AND n.n_name = " + S(0) + " ORDER BY ps.ps_partkey, ps.ps_suppkey " + off + "LIMIT 1");
    Require(f.size() >= 3, "Q11 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + 100 "
                   "WHERE ps_partkey = " +
                   f[0] + " AND ps_suppkey = " + f[1];
    p.restore_sql = "UPDATE partsupp SET ps_supplycost = " + f[2] +
                    " WHERE ps_partkey = " + f[0] + " AND ps_suppkey = " + f[1];
    return p;
  }
  case 12: {
    // Priority reclassify — order-side ApplyDelta must update day counts via
    // the reverse index (not only lineitem shipmode flips).
    auto f = Fetch(
        "SELECT o.o_orderkey, o.o_orderpriority, l.l_shipmode "
        "FROM orders o, lineitem l "
        "WHERE o.o_orderkey = l.l_orderkey AND l.l_shipmode IN (" + S(0) + ", " + S(1) + ") "
        "AND l.l_commitdate < l.l_receiptdate AND l.l_shipdate < l.l_commitdate "
        "AND l.l_receiptdate >= " + D(2) + " "
        "AND l.l_receiptdate < " + D(2) + " + INTERVAL '1' year "
        "AND (o.o_orderpriority = '1-URGENT' OR o.o_orderpriority = '2-HIGH') "
        "ORDER BY o.o_orderkey, l.l_linenumber " + off + "LIMIT 1");
    Require(f.size() >= 3, "Q12 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE orders SET o_orderpriority = '3-MEDIUM' "
                   "WHERE o_orderkey = " + f[0];
    p.restore_sql = "UPDATE orders SET o_orderpriority = " + SqlLiteral(f[1]) +
                    " WHERE o_orderkey = " + f[0];
    return p;
  }
  case 13: {
    auto f = Fetch(
        "SELECT o_orderkey, o_comment FROM orders "
        "WHERE o_comment NOT LIKE " + S(0) + " "
        "ORDER BY o_orderkey " + off + "LIMIT 1");
    Require(f.size() >= 2, "Q13 probe fields");
    std::string append = std::get<std::string>(arguments.values[0].payload);
    for (char &c : append) {
      if (c == '%')
        c = ' ';
    }
    WriteProbe p;
    p.update_sql = "UPDATE orders SET o_comment = o_comment || " + SqlLiteral(append) +
                   " WHERE o_orderkey = " + f[0];
    p.restore_sql = "UPDATE orders SET o_comment = " + SqlLiteral(f[1]) +
                    " WHERE o_orderkey = " + f[0];
    return p;
  }
  case 14:
    // Large bump so promo_revenue fingerprint moves (DuckDB uses +1e7).
    return LinePriceBump(
        Fetch("SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice::text "
              "FROM lineitem l, part p WHERE l.l_partkey = p.p_partkey "
              "AND l.l_shipdate >= " + D(0) + " "
              "AND l.l_shipdate < " + D(0) + " + INTERVAL '1' month "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "10000000");
  case 15:
    // Bump a line belonging to the current max-revenue supplier (DuckDB Q15).
    return LinePriceBump(
        Fetch("SELECT l_orderkey, l_linenumber, l_extendedprice::text FROM lineitem "
              "WHERE l_shipdate >= " + D(0) + " "
              "AND l_shipdate < " + D(0) + " + INTERVAL '3' MONTH "
              "AND l_suppkey = (SELECT l_suppkey FROM lineitem "
              "WHERE l_shipdate >= " + D(0) + " "
              "AND l_shipdate < " + D(0) + " + INTERVAL '3' MONTH "
              "GROUP BY l_suppkey "
              "ORDER BY SUM(l_extendedprice * (1 - l_discount)) DESC LIMIT 1) "
              "ORDER BY l_orderkey, l_linenumber " + off + "LIMIT 1"),
        "100");
  case 16: {
    // Move the row onto a complaints supplier so COUNT(DISTINCT) drops.
    std::string sizes;
    for (int i = 0; i < 8; ++i) {
      if (i)
        sizes += ", ";
      sizes += std::to_string(I64(static_cast<std::size_t>(2 + i)));
    }
    auto f = Fetch(
        "SELECT ps.ps_partkey, ps.ps_suppkey, "
        "(SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%' "
        "ORDER BY s_suppkey LIMIT 1)::text "
        "FROM partsupp ps, part p "
        "WHERE p.p_partkey = ps.ps_partkey AND p.p_brand <> " + S(0) + " "
        "AND p.p_type NOT LIKE " + S(1) + " "
        "AND p.p_size IN (" + sizes + ") "
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
              "AND p.p_brand = " + S(0) + " AND p.p_container = " + S(1) + " "
              "ORDER BY l.l_orderkey, l.l_linenumber " + off + "LIMIT 1"),
        "100");
  case 18: {
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity::text FROM lineitem l "
        "WHERE l.l_orderkey IN ("
        "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
        "HAVING SUM(l_quantity) > " + std::to_string(I64(0)) + ") "
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
        "AND p.p_brand = " + S(0) + " AND p.p_container IN "
        "('SM CASE','SM BOX','SM PACK','SM PKG') "
        "AND l.l_quantity >= " + std::to_string(I64(1)) +
        " AND l.l_quantity <= " + std::to_string(I64(1) + 10) + " "
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
    auto f = Fetch(
        "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity::text "
        "FROM lineitem l "
        "JOIN part p ON p.p_partkey = l.l_partkey AND p.p_name LIKE " + S(1) + " "
        "JOIN partsupp ps ON ps.ps_partkey = l.l_partkey AND ps.ps_suppkey = l.l_suppkey "
        "JOIN supplier s ON s.s_suppkey = l.l_suppkey "
        "JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = " + S(2) + " "
        "WHERE l.l_shipdate >= " + D(0) + " "
        "AND l.l_shipdate < " + D(0) + " + INTERVAL '1' year "
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
        "AND s.s_nationkey = n.n_nationkey AND n.n_name = " + S(0) + " "
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
    std::string codes;
    for (int i = 0; i < 7; ++i) {
      if (i)
        codes += ", ";
      codes += S(static_cast<std::size_t>(i));
    }
    auto f = Fetch(
        "SELECT c_custkey, c_acctbal::text FROM customer "
        "WHERE substring(c_phone from 1 for 2) IN (" + codes + ") "
        "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
        "AND substring(c_phone from 1 for 2) IN (" + codes + ")) "
        "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey) "
        "ORDER BY c_custkey " + off + "LIMIT 1");
    Require(f.size() >= 2, "Q22 probe fields");
    WriteProbe p;
    p.update_sql = "UPDATE customer SET c_acctbal = c_acctbal + 100 "
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
                               const std::string &force_action,
                               const aux_sweep::TpchArgVariant &variant) {
  Require(prefer_aux, "write_maint requires aux (ACCEL_PG_DIFF_KIND=aux)");
  RunReadDifferential(tools, query_num, prefer_aux, variant);
  EnsureMutationTriggers(tools);

  const std::string ref_sql = variant.base_sql;
  const std::string accel_sql = variant.aux_sql;

  WriteProbe probe;
  bool found = false;
  for (int cand = 0; cand < 64; ++cand) {
    try {
      probe = ResolveWriteProbe(tools, query_num, cand, variant.arguments);
    } catch (const std::exception &) {
      break;
    }
    if (ProbeChangesBaseFingerprint(tools, ref_sql, probe.update_sql)) {
      found = true;
      break;
    }
  }
  Require(found, "invalidating probe did not change base SQL fingerprint for Q" +
                     std::to_string(query_num) + " sql=" + variant.kernel_sql +
                     " — write must impact this query's observable result");

  std::ostringstream script;
  script << "LOAD '" << tools.extension_so << "';\n"
         << "SELECT accel_postgres_reset_observer();\n";
  if (!force_action.empty()) {
    script << "SELECT accel_postgres_set_force_maintenance_action("
           << SqlLiteral(force_action) << ");\n";
  } else {
    script << "SELECT accel_postgres_set_force_maintenance_action('');\n";
  }
  script << "SELECT '===WARM===' AS m;\n"
         << accel_sql << ";\n"
         << "SELECT '===MUT_BEFORE===' AS m;\n"
         << "SELECT accel_postgres_get_observer_mutation_count();\n"
         << probe.update_sql << ";\n"
         << "SELECT '===MUT_AFTER===' AS m;\n"
         << "SELECT accel_postgres_get_observer_mutation_count();\n"
         << "SELECT '===ACTION===' AS m;\n"
         << "SELECT accel_postgres_get_last_maintenance_action();\n"
         << "SELECT '===ACCEL===' AS m;\n"
         << accel_sql << ";\n"
         << "SELECT '===REF===' AS m;\n"
         << ref_sql << ";\n"
         << "SELECT '===DONE===' AS m;\n"
         << probe.restore_sql << ";\n"
         << "SELECT accel_postgres_set_force_maintenance_action('');\n";

  const std::string session = RunPsqlScript(tools, script.str());
  const auto mut_before = TrimLines(SectionAfter(session, "===MUT_BEFORE==="));
  const auto mut_after = TrimLines(SectionAfter(session, "===MUT_AFTER==="));
  const auto action_out = TrimLines(SectionAfter(session, "===ACTION==="));
  const auto accel_out = SectionAfter(session, "===ACCEL===");
  const auto ref_out = SectionAfter(session, "===REF===");

  int before_n = 0;
  int after_n = 0;
  try {
    before_n = std::stoi(mut_before);
    after_n = std::stoi(mut_after);
  } catch (...) {
    throw std::runtime_error("failed to parse mutation counts before=[" + mut_before +
                             "] after=[" + mut_after + "]");
  }
  Require(after_n > before_n,
          "mutation observer did not advance (before=" + std::to_string(before_n) +
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
                              "InvalidateAndFallback"}),
            "forced ApplyDelta produced unexpected action=[" + action + "]");
  } else if (force_action == "RebuildPartition") {
    Require(ActionOk(action, {"RebuildPartition", "RebuildAll",
                              "InvalidateAndFallback"}),
            "forced RebuildPartition produced unexpected action=[" + action + "]");
  } else if (force_action == "RebuildAll") {
    Require(ActionOk(action, {"RebuildAll", "InvalidateAndFallback"}),
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
            << " sql=" << variant.kernel_sql
            << " (invalidating write + mutation_count " << before_n << "->" << after_n
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

void RunWriteMixAndStrats(const Tools &tools, int query_id, bool prefer_aux,
                          const aux_sweep::TpchArgVariant &variant) {
  RunWriteMaintDifferential(tools, query_id, prefer_aux, "", variant);
  const char *force_env = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  if (force_env && *force_env) {
    RunWriteMaintDifferential(tools, query_id, prefer_aux, force_env, variant);
    return;
  }
  for (const auto &strat : MaintenanceStrats()) {
    RunWriteMaintDifferential(tools, query_id, prefer_aux, strat, variant);
  }
}

int ArgSampleCount(const char *name, int fallback) {
  const char *raw = std::getenv(name);
  if (!raw || !*raw)
    return fallback;
  return std::max(1, std::stoi(raw));
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
    const int read_samples = ArgSampleCount("ACCEL_PG_DIFF_ARG_SAMPLES", 4);
    const int maint_samples = ArgSampleCount("ACCEL_PG_DIFF_MAINT_SAMPLES", 2);

    setenv("ACCEL_BENCH_VARY_ARGS", "1", 0);
    setenv("ACCEL_BENCH_ARG_SEED", "42", 0);

    const auto &pg = LookupPostgresQuery("tpch", query_id);
    aux_sweep::TpchArgCatalogSeed seed;
    seed.arguments = aux_sweep::CatalogDefaultTpchArgs(query_id);
    seed.base_sql = pg.base_sql;
    seed.kernel_sql = pg.accelerator_sql;
    seed.aux_sql = pg.aux_sql;
    auto pool = aux_sweep::BuildTpchDifferentialArgPool(
        query_id, static_cast<std::size_t>(read_samples), &seed);
    Require(!pool.empty(), "empty arg pool for Q" + std::to_string(query_id));
    std::cerr << "arg pool Q" << query_id << " size=" << pool.size() << '\n';

    if (mode_s == "write_maint") {
      const std::size_t n =
          std::min(pool.size(), static_cast<std::size_t>(maint_samples));
      for (std::size_t i = 0; i < n; ++i)
        RunWriteMixAndStrats(tools, query_id, prefer_aux, pool[i]);
    } else {
      for (const auto &variant : pool)
        RunReadDifferential(tools, query_id, prefer_aux, variant);
      if (prefer_aux && (!mode || mode_s == "read")) {
        const char *skip = std::getenv("ACCEL_PG_DIFF_SKIP_WRITE_MAINT");
        if (!(skip && std::string(skip) == "1")) {
          const std::size_t n =
              std::min(pool.size(), static_cast<std::size_t>(maint_samples));
          for (std::size_t i = 0; i < n; ++i)
            RunWriteMixAndStrats(tools, query_id, prefer_aux, pool[i]);
        }
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_postgres_tpch_differential failed: " << error.what() << '\n';
    return 1;
  }
}
