#pragma once

// Per-query TPC-H test knowledge shared by the engine backends' harnesses and
// benches: reference/accelerator SQL fixups, scenario queries, writes that
// invalidate a query's state, and write probes. Backends pass in how to run
// SQL and which UPDATE dialect they speak; they name no query.
//
// TEST-SIDE ONLY: this file is allowed to know TPC-H.
//
// Recipe SQL is the common subset of PostgreSQL and MySQL. In a probe's
// update/restore templates, {i} is field i of the probe's select row, with
// single quotes doubled.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace accel::tpch_params {

// ---- Query SQL ----------------------------------------------------------

// Q11's FRACTION = 0.0001 / SF, which the catalog SQL spells for SF1.
inline std::string Q11FractionLiteral() {
  if (const char *env = std::getenv("ACCEL_TPCH_Q11_FRACTION"); env && *env) return env;
  if (const char *scale = std::getenv("ACCEL_MYSQL_SWEEP_SCALE"); scale && *scale) {
    const std::string s(scale);
    if (s == "sf10" || s == "10") return "0.00001";
    if (s == "sf20" || s == "20") return "0.000005";
    if (s == "sf1" || s == "1") return "0.0001";
  }
  if (const char *sf = std::getenv("ACCEL_TPCH_SCALE_FACTOR"); sf && *sf) {
    try {
      const double scale = std::stod(sf);
      if (scale > 0.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.8g", 0.0001 / scale);
        return buf;
      }
    } catch (...) {
    }
  }
  return "0.0001";
}

inline std::string RewriteQ11Fraction(std::string sql) {
  const std::string frac = Q11FractionLiteral();
  const std::string from = "0.0001";
  for (std::size_t pos = 0; (pos = sql.find(from, pos)) != std::string::npos; pos += frac.size()) {
    sql.replace(pos, from.size(), frac);
  }
  return sql;
}

// Q11 orders by value only: tied parts may come in any order. The accelerator
// breaks ties by partkey; make the reference do the same.
inline std::string TotalOrderQ11(std::string sql) {
  const std::string from = "ORDER BY value DESC";
  const auto pos = sql.rfind(from);
  if (pos != std::string::npos && sql.find("ps_partkey", pos) == std::string::npos) {
    sql.insert(pos + from.size(), ", ps_partkey");
  }
  return sql;
}

// The catalog's base SQL made comparable to the accelerator's answer.
inline std::string ReferenceSql(int q, std::string sql) {
  return q == 11 ? TotalOrderQ11(RewriteQ11Fraction(std::move(sql))) : sql;
}

inline std::string AcceleratorSql(int q, std::string sql) {
  return q == 11 ? RewriteQ11Fraction(std::move(sql)) : sql;
}

// ---- Scenario queries ---------------------------------------------------

// A query whose answer sums lineitem rows, so lost or doubled lineitem writes
// show in it (the build-vs-write race tests drive lineitem against it).
inline constexpr int kLineitemRaceQuery = 1;

// Another query to keep busy while measuring q's interference.
inline int InterferencePeer(int q) { return q == 1 ? 3 : 1; }

// ---- Invalidating updates -----------------------------------------------

// How the engine updates the first row of a table in key order.
enum class UpdateDialect {
  Ctid,       // WHERE ctid IN (SELECT ctid ... LIMIT 1)
  OrderLimit, // UPDATE ... ORDER BY ... LIMIT 1
};

namespace detail {

struct FirstRowUpdate {
  std::string table, set, where, order_by;
};

inline FirstRowUpdate LinePrice(const std::string &where) {
  return {"lineitem", "l_extendedprice = l_extendedprice + 0.01", where, "l_orderkey, l_linenumber"};
}

inline FirstRowUpdate InvalidatingUpdate(int q) {
  const FirstRowUpdate kPartsuppCost = {"partsupp", "ps_supplycost = ps_supplycost + 0.01", "", "ps_partkey, ps_suppkey"};
  switch (q) {
  case 1: return LinePrice("l_shipdate <= DATE '1998-09-02'");
  case 2: return kPartsuppCost;
  case 3:
    return LinePrice("l_orderkey IN (SELECT o_orderkey FROM customer JOIN orders ON c_custkey = o_custkey "
                     "WHERE c_mktsegment = 'BUILDING' AND o_orderdate < DATE '1995-03-15') "
                     "AND l_shipdate > DATE '1995-03-15'");
  case 4:
    return LinePrice("l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_orderdate >= DATE '1993-07-01' "
                     "AND o_orderdate < DATE '1993-10-01') AND l_commitdate < l_receiptdate");
  case 5:
    return LinePrice("l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_orderdate >= DATE '1995-01-01' "
                     "AND o_orderdate < DATE '1996-01-01')");
  case 6: return LinePrice("l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'");
  case 7: return LinePrice("l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'");
  case 8:
    return LinePrice("l_orderkey IN (SELECT o_orderkey FROM orders "
                     "WHERE o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31')");
  case 9: return LinePrice("l_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE '%green%')");
  case 10:
    return LinePrice("l_returnflag = 'R' AND l_orderkey IN (SELECT o_orderkey FROM orders "
                     "WHERE o_orderdate >= DATE '1993-06-01' AND o_orderdate < DATE '1993-09-01')");
  case 11: {
    auto update = kPartsuppCost;
    update.where = "ps_suppkey IN (SELECT s_suppkey FROM supplier, nation "
                   "WHERE s_nationkey = n_nationkey AND n_name = 'ROMANIA')";
    return update;
  }
  case 12:
    return LinePrice("l_receiptdate >= DATE '1995-01-01' AND l_receiptdate < DATE '1996-01-01' "
                     "AND l_shipmode IN ('MAIL', 'SHIP')");
  case 13: return {"orders", "o_comment = CONCAT(o_comment, ' ')", "", "o_orderkey"};
  case 14: return LinePrice("l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'");
  case 15: return LinePrice("l_shipdate >= DATE '1994-11-01' AND l_shipdate < DATE '1995-02-01'");
  case 16: return {"supplier", "s_comment = CONCAT(s_comment, ' ')", "", "s_suppkey"};
  case 17:
    return LinePrice("l_partkey IN (SELECT p_partkey FROM part WHERE p_brand = 'Brand#34' "
                     "AND p_container = 'LG CASE')");
  case 18:
    return LinePrice("l_orderkey IN (SELECT x.l_orderkey FROM (SELECT l_orderkey FROM lineitem "
                     "GROUP BY l_orderkey ORDER BY SUM(l_quantity) DESC LIMIT 1) x)");
  case 19: return LinePrice("l_shipmode IN ('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON'");
  case 20: return LinePrice("l_shipdate >= DATE '1997-01-01' AND l_shipdate < DATE '1998-01-01'");
  case 21:
    return LinePrice("l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F') "
                     "AND l_receiptdate > l_commitdate");
  case 22: return {"orders", "o_orderstatus = o_orderstatus", "", "o_orderkey"};
  default: return LinePrice("true");
  }
}

} // namespace detail

// One UPDATE touching a row that feeds q's state.
inline std::string InvalidatingUpdateSql(int q, UpdateDialect dialect) {
  const auto u = detail::InvalidatingUpdate(q);
  const std::string where = u.where.empty() ? "" : " WHERE " + u.where;
  if (dialect == UpdateDialect::OrderLimit) {
    return "UPDATE " + u.table + " SET " + u.set + where + " ORDER BY " + u.order_by + " LIMIT 1";
  }
  return "UPDATE " + u.table + " SET " + u.set + " WHERE ctid IN (SELECT ctid FROM " + u.table + where +
         " ORDER BY " + u.order_by + " LIMIT 1)";
}

// ---- Write probes -------------------------------------------------------

// A concrete UPDATE and its restore; the update must change q's answer.
struct WriteProbe {
  std::string update_sql;
  std::string restore_sql;
  int candidate = 0;
};

namespace detail {

struct ProbeRecipe {
  std::string select; // candidate rows in a stable order; no LIMIT
  std::string update;
  std::string restore;
};

// A lineitem whose price is raised by at least its own value, so the bump
// survives a magnified money scale. `from` is "FROM lineitem l ...".
inline ProbeRecipe PriceBump(const std::string &from, const std::string &delta = "100") {
  return {"SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice " + from +
              " ORDER BY l.l_orderkey, l.l_linenumber",
          "UPDATE lineitem SET l_extendedprice = l_extendedprice + GREATEST(" + delta +
              ", l_extendedprice) WHERE l_orderkey = {0} AND l_linenumber = {1}",
          "UPDATE lineitem SET l_extendedprice = {2} WHERE l_orderkey = {0} AND l_linenumber = {1}"};
}

inline ProbeRecipe ProbeFor(int q) {
  switch (q) {
  case 1: return PriceBump("FROM lineitem l WHERE l.l_shipdate <= DATE '1998-09-02'");
  case 2:
    // Raising a top result row's supplier balance reorders and changes the answer.
    return {"SELECT s.s_suppkey, s.s_acctbal FROM part p, supplier s, partsupp ps2, nation n, region r "
            "WHERE p.p_partkey = ps2.ps_partkey AND s.s_suppkey = ps2.ps_suppkey "
            "AND p.p_size = 15 AND p.p_type LIKE '%BRASS' AND s.s_nationkey = n.n_nationkey "
            "AND n.n_regionkey = r.r_regionkey AND r.r_name = 'EUROPE' "
            "AND ps2.ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
            "WHERE p.p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey "
            "AND n_regionkey = r_regionkey AND r_name = 'EUROPE') "
            "ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey",
            "UPDATE supplier SET s_acctbal = s_acctbal + s_acctbal WHERE s_suppkey = {0}",
            "UPDATE supplier SET s_acctbal = {1} WHERE s_suppkey = {0}"};
  case 3:
    return PriceBump("FROM lineitem l WHERE l.l_orderkey IN (SELECT l_orderkey FROM ("
                     "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
                     "o_orderdate, o_shippriority FROM customer, orders, lineitem "
                     "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey "
                     "AND l_orderkey = o_orderkey AND o_orderdate < DATE '1995-03-15' "
                     "AND l_shipdate > DATE '1995-03-15' "
                     "GROUP BY l_orderkey, o_orderdate, o_shippriority "
                     "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10) AS q3_top) "
                     "AND l.l_shipdate > DATE '1995-03-15'");
  case 4:
    // Clears the late receipt on the sole late line of a windowed order.
    return {"SELECT l.l_orderkey, l.l_linenumber, l.l_commitdate, l.l_receiptdate FROM lineitem l "
            "WHERE l.l_commitdate < l.l_receiptdate AND l.l_orderkey = ("
            "SELECT o.o_orderkey FROM orders o WHERE o.o_orderdate >= DATE '1993-07-01' "
            "AND o.o_orderdate < DATE '1993-10-01' AND (SELECT COUNT(*) FROM lineitem lx "
            "WHERE lx.l_orderkey = o.o_orderkey AND lx.l_commitdate < lx.l_receiptdate) = 1 "
            "ORDER BY o.o_orderkey LIMIT 1) ORDER BY l.l_linenumber",
            "UPDATE lineitem SET l_receiptdate = DATE '{2}' WHERE l_orderkey = {0} AND l_linenumber = {1}",
            "UPDATE lineitem SET l_receiptdate = DATE '{3}' WHERE l_orderkey = {0} AND l_linenumber = {1}"};
  case 5:
    return PriceBump("FROM lineitem l, orders o, customer c, supplier s, nation n, region r "
                     "WHERE l.l_orderkey = o.o_orderkey AND c.c_custkey = o.o_custkey "
                     "AND l.l_suppkey = s.s_suppkey AND c.c_nationkey = s.s_nationkey "
                     "AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey "
                     "AND r.r_name = 'AMERICA' AND o.o_orderdate >= DATE '1995-01-01' "
                     "AND o.o_orderdate < DATE '1996-01-01'");
  case 6:
    return PriceBump("FROM lineitem l WHERE l.l_shipdate >= DATE '1995-01-01' "
                     "AND l.l_shipdate < DATE '1996-01-01' AND l.l_discount BETWEEN 0.02 AND 0.04 "
                     "AND l.l_quantity < 25");
  case 7:
    return PriceBump("FROM lineitem l, supplier s, orders o, customer c, nation n1, nation n2 "
                     "WHERE s.s_suppkey = l.l_suppkey AND o.o_orderkey = l.l_orderkey "
                     "AND c.c_custkey = o.o_custkey AND s.s_nationkey = n1.n_nationkey "
                     "AND c.c_nationkey = n2.n_nationkey "
                     "AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL') "
                     "OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM')) "
                     "AND l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'");
  case 8:
    return PriceBump("FROM part p, supplier s, lineitem l, orders o, customer c, nation n1, nation n2, region r "
                     "WHERE p.p_partkey = l.l_partkey AND s.s_suppkey = l.l_suppkey "
                     "AND l.l_orderkey = o.o_orderkey AND o.o_custkey = c.c_custkey "
                     "AND c.c_nationkey = n1.n_nationkey AND n1.n_regionkey = r.r_regionkey "
                     "AND r.r_name = 'ASIA' AND s.s_nationkey = n2.n_nationkey "
                     "AND o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
                     "AND p.p_type = 'PROMO BRUSHED BRASS'");
  case 9:
    return PriceBump("FROM part p, lineitem l WHERE p.p_partkey = l.l_partkey AND p.p_name LIKE '%green%'");
  case 10:
    // Must hit a top-20 customer so the LIMIT 20 answer moves.
    return PriceBump("FROM lineitem l, orders o WHERE l.l_orderkey = o.o_orderkey AND l.l_returnflag = 'R' "
                     "AND o.o_orderdate >= DATE '1993-06-01' AND o.o_orderdate < DATE '1993-09-01' "
                     "AND o.o_custkey IN (SELECT c_custkey FROM ("
                     "SELECT c_custkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
                     "FROM customer JOIN orders ON c_custkey = o_custkey "
                     "JOIN lineitem ON l_orderkey = o_orderkey "
                     "WHERE o_orderdate >= DATE '1993-06-01' AND o_orderdate < DATE '1993-09-01' "
                     "AND l_returnflag = 'R' GROUP BY c_custkey ORDER BY revenue DESC, c_custkey LIMIT 20) top20)");
  case 11:
    // The highest-contribution Romanian partsupp, scaled up to cross the HAVING threshold.
    return {"SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost FROM partsupp ps, supplier s, nation n "
            "WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey AND n.n_name = 'ROMANIA' "
            "ORDER BY (ps.ps_supplycost * ps.ps_availqty) DESC, ps.ps_partkey, ps.ps_suppkey",
            "UPDATE partsupp SET ps_supplycost = ps_supplycost * 1000 WHERE ps_partkey = {0} AND ps_suppkey = {1}",
            "UPDATE partsupp SET ps_supplycost = {2} WHERE ps_partkey = {0} AND ps_suppkey = {1}"};
  case 12:
    // Counts are by shipmode: flip a MAIL/SHIP line out of (or into) the pair.
    return {"SELECT l.l_orderkey, l.l_linenumber, l.l_shipmode FROM orders o, lineitem l "
            "WHERE o.o_orderkey = l.l_orderkey AND l.l_shipmode IN ('MAIL', 'SHIP') "
            "AND l.l_commitdate < l.l_receiptdate AND l.l_shipdate < l.l_commitdate "
            "AND l.l_receiptdate >= DATE '1995-01-01' AND l.l_receiptdate < DATE '1996-01-01' "
            "ORDER BY l.l_orderkey, l.l_linenumber",
            "UPDATE lineitem SET l_shipmode = CASE WHEN '{2}' = 'MAIL' THEN 'AIR' ELSE 'MAIL' END "
            "WHERE l_orderkey = {0} AND l_linenumber = {1}",
            "UPDATE lineitem SET l_shipmode = '{2}' WHERE l_orderkey = {0} AND l_linenumber = {1}"};
  case 13:
    return {"SELECT o_orderkey, o_comment FROM orders WHERE o_comment NOT LIKE '%unusual%accounts%' "
            "ORDER BY o_orderkey",
            "UPDATE orders SET o_comment = CONCAT(o_comment, ' unusual accounts') WHERE o_orderkey = {0}",
            "UPDATE orders SET o_comment = '{1}' WHERE o_orderkey = {0}"};
  case 14:
    return PriceBump("FROM lineitem l, part p WHERE l.l_partkey = p.p_partkey "
                     "AND l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'",
                     "10000000");
  case 15:
    // A line of the current max-revenue supplier.
    return PriceBump("FROM lineitem l WHERE l.l_shipdate >= DATE '1994-11-01' "
                     "AND l.l_shipdate < DATE '1995-02-01' AND l.l_suppkey = ("
                     "SELECT l_suppkey FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
                     "AND l_shipdate < DATE '1995-02-01' GROUP BY l_suppkey "
                     "ORDER BY SUM(l_extendedprice * (1 - l_discount)) DESC LIMIT 1)");
  case 16:
    // Moves an eligible supplier into the Customer Complaints set via its comment.
    return {"SELECT s.s_suppkey, s.s_comment FROM supplier s "
            "WHERE s.s_comment NOT LIKE '%Customer%Complaints%' AND EXISTS ("
            "SELECT 1 FROM partsupp ps, part p WHERE ps.ps_suppkey = s.s_suppkey "
            "AND p.p_partkey = ps.ps_partkey AND p.p_brand <> 'Brand#13' "
            "AND p.p_type NOT LIKE 'SMALL BRUSHED%' AND p.p_size IN (26, 24, 27, 29, 7, 20, 22, 33)) "
            "ORDER BY s.s_suppkey",
            "UPDATE supplier SET s_comment = CONCAT(s_comment, ' Customer Complaints') WHERE s_suppkey = {0}",
            "UPDATE supplier SET s_comment = '{1}' WHERE s_suppkey = {0}"};
  case 17:
    // A small-quantity line of a part that is made to match the brand and container.
    return {"SELECT l.l_orderkey, l.l_linenumber, l.l_quantity, l.l_partkey, p.p_brand, p.p_container "
            "FROM lineitem l JOIN part p ON p.p_partkey = l.l_partkey WHERE l.l_partkey IN ("
            "SELECT l_partkey FROM lineitem GROUP BY l_partkey HAVING COUNT(*) >= 2) "
            "ORDER BY l.l_quantity, l.l_orderkey, l.l_linenumber",
            "UPDATE part SET p_brand = 'Brand#34', p_container = 'LG CASE' WHERE p_partkey = {3}; "
            "UPDATE lineitem SET l_quantity = 1 WHERE l_orderkey = {0} AND l_linenumber = {1}",
            "UPDATE lineitem SET l_quantity = {2} WHERE l_orderkey = {0} AND l_linenumber = {1}; "
            "UPDATE part SET p_brand = '{4}', p_container = '{5}' WHERE p_partkey = {3}"};
  case 18:
    // Raises the quantity of a line of the order with the largest total.
    return {"SELECT l.l_orderkey, l.l_linenumber, l.l_quantity FROM lineitem l WHERE l.l_orderkey = ("
            "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey ORDER BY SUM(l_quantity) DESC, l_orderkey "
            "LIMIT 1) ORDER BY l.l_linenumber",
            "UPDATE lineitem SET l_quantity = l_quantity + 50 WHERE l_orderkey = {0} AND l_linenumber = {1}",
            "UPDATE lineitem SET l_quantity = {2} WHERE l_orderkey = {0} AND l_linenumber = {1}"};
  case 19:
    // A shipped-by-air line made to match the first brand's quantity and size window.
    return {"SELECT l.l_orderkey, l.l_partkey, l.l_linenumber, l.l_quantity, p.p_brand, p.p_container, p.p_size "
            "FROM lineitem l, part p WHERE p.p_partkey = l.l_partkey AND l.l_shipmode IN ('AIR', 'AIR REG') "
            "AND l.l_shipinstruct = 'DELIVER IN PERSON' ORDER BY l.l_orderkey, l.l_linenumber",
            "UPDATE part SET p_brand = 'Brand#31', p_container = 'SM CASE', p_size = 3 WHERE p_partkey = {1}; "
            "UPDATE lineitem SET l_quantity = 10 "
            "WHERE l_orderkey = {0} AND l_partkey = {1} AND l_linenumber = {2}",
            "UPDATE lineitem SET l_quantity = {3} "
            "WHERE l_orderkey = {0} AND l_partkey = {1} AND l_linenumber = {2}; "
            "UPDATE part SET p_brand = '{4}', p_container = '{5}', p_size = {6} WHERE p_partkey = {1}"};
  case 20:
    // A part supplied from the nation inside the window, renamed to match the color.
    return {"SELECT p.p_partkey, p.p_name FROM part p JOIN lineitem l ON l.l_partkey = p.p_partkey "
            "JOIN supplier s ON s.s_suppkey = l.l_suppkey "
            "JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'RUSSIA' "
            "WHERE l.l_shipdate >= DATE '1997-01-01' AND l.l_shipdate < DATE '1998-01-01' "
            "ORDER BY p.p_partkey",
            "UPDATE part SET p_name = CONCAT('gainsboro ', p_name) WHERE p_partkey = {0}",
            "UPDATE part SET p_name = '{1}' WHERE p_partkey = {0}"};
  case 21:
    // Clears the late receipt on a waiting line of the nation.
    return {"SELECT l1.l_orderkey, l1.l_linenumber, l1.l_commitdate, l1.l_receiptdate "
            "FROM supplier s, lineitem l1, orders o, nation n "
            "WHERE s.s_suppkey = l1.l_suppkey AND o.o_orderkey = l1.l_orderkey "
            "AND o.o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate "
            "AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey "
            "AND l2.l_suppkey <> l1.l_suppkey) "
            "AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey "
            "AND l3.l_suppkey = l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate "
            "AND l3.l_linenumber <> l1.l_linenumber) "
            "AND s.s_nationkey = n.n_nationkey AND n.n_name = 'SAUDI ARABIA' "
            "ORDER BY l1.l_orderkey, l1.l_linenumber",
            "UPDATE lineitem SET l_receiptdate = DATE '{2}' WHERE l_orderkey = {0} AND l_linenumber = {1}",
            "UPDATE lineitem SET l_receiptdate = DATE '{3}' WHERE l_orderkey = {0} AND l_linenumber = {1}"};
  case 22:
    // A qualifying customer without orders: its balance moves the sum.
    return {"SELECT c_custkey, c_acctbal FROM customer "
            "WHERE SUBSTRING(c_phone, 1, 2) IN ('30', '19', '14', '28', '11', '22', '32') "
            "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
            "AND SUBSTRING(c_phone, 1, 2) IN ('30', '19', '14', '28', '11', '22', '32')) "
            "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey) ORDER BY c_custkey",
            "UPDATE customer SET c_acctbal = c_acctbal + GREATEST(100, ABS(c_acctbal)) WHERE c_custkey = {0}",
            "UPDATE customer SET c_acctbal = {1} WHERE c_custkey = {0}"};
  default: throw std::runtime_error("no write probe for Q" + std::to_string(q));
  }
}

inline std::string Fill(const std::string &tmpl, const std::vector<std::string> &fields) {
  std::string out;
  for (std::size_t i = 0; i < tmpl.size(); ++i) {
    if (tmpl[i] != '{') {
      out += tmpl[i];
      continue;
    }
    const std::size_t field = static_cast<std::size_t>(tmpl[i + 1] - '0');
    if (field >= fields.size()) throw std::runtime_error("write probe: row has no field " + std::to_string(field));
    for (char c : fields[field]) {
      if (c == '\'') out += '\'';
      out += c;
    }
    i += 2;
  }
  return out;
}

} // namespace detail

// Runs the probe's select through `query` (ok, output) and builds the probe
// from the first row of the output, '|'-separated, at `candidate_offset`.
inline WriteProbe ResolveWriteProbe(
    int q, int candidate_offset,
    const std::function<std::pair<bool, std::string>(const std::string &)> &query) {
  const auto recipe = detail::ProbeFor(q);
  const std::string select = recipe.select + " LIMIT 1 OFFSET " + std::to_string(candidate_offset);
  const std::string where = "Q" + std::to_string(q) + " offset=" + std::to_string(candidate_offset);
  const auto result = query(select);
  if (!result.first) throw std::runtime_error("probe select failed for " + where + ":\n" + select);
  const std::string &out = result.second;
  const auto begin = out.find_first_not_of("\r\n");
  const std::string line = begin == std::string::npos ? "" : out.substr(begin, out.find('\n', begin) - begin);
  if (line.empty()) throw std::runtime_error("probe select returned no rows for " + where);
  std::vector<std::string> row(1);
  for (char c : line) {
    if (c == '|') row.emplace_back();
    else if (c != '\r') row.back() += c;
  }
  return {detail::Fill(recipe.update, row), detail::Fill(recipe.restore, row), candidate_offset};
}

} // namespace accel::tpch_params
