// PostgreSQL TPC-H differential harness.
// Builds: test_postgres_tpch_differential
//
// Modes (ACCEL_PG_DIFF_MODE):
//   read (default) — ordered SQL vs accelerator/aux match (hard fail)
//   write_maint    — read match, then mutation + maintenance + re-match
// Env: TPCH_PG_DB, ACCEL_PG_DIFF_KIND=base|aux, ACCEL_WORKLOAD_KIND

#include <unistd.h>
#include <sys/wait.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "accelerator_codegen/engine_backend/postgres_tooling.hpp"
#include "catalog/postgres/query_catalog.hpp"

namespace {

using namespace accel::postgres;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
using accelerator_codegen::aux_state::engine::postgres::PostgresSharedQueryCatalogEntry;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string ShellEscape(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

bool RunSQL(const Tools &tools, const std::string &sql) {
  std::string cmd = "PGPASSWORD=" + ShellEscape(tools.password) + " " +
                    ShellEscape(tools.psql.string()) + " -h " + ShellEscape(tools.host) +
                    " -p " + std::to_string(tools.port) + " -U " + ShellEscape(tools.user) +
                    " -d " + ShellEscape(tools.database) +
                    " -v ON_ERROR_STOP=1 -t -A -c " + ShellEscape(sql);
  return std::system(cmd.c_str()) == 0;
}

std::pair<bool, std::string> QuerySQL(const Tools &tools, const std::string &sql) {
  std::string cmd = "PGPASSWORD=" + ShellEscape(tools.password) + " " +
                    ShellEscape(tools.psql.string()) + " -h " + ShellEscape(tools.host) +
                    " -p " + std::to_string(tools.port) + " -U " + ShellEscape(tools.user) +
                    " -d " + ShellEscape(tools.database) + " -t -A -c " + ShellEscape(sql) +
                    " 2>/dev/null";
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp)
    return {false, ""};
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) {
    out += buf;
  }
  int rc = pclose(fp);
  return {WEXITSTATUS(rc) == 0, out};
}

std::string TrimLines(const std::string &input) {
  std::string result;
  result.reserve(input.size());
  size_t start = 0;
  for (size_t i = 0; i <= input.size(); ++i) {
    if (i == input.size() || input[i] == '\n') {
      size_t end = i;
      while (end > start &&
             (input[end - 1] == ' ' || input[end - 1] == '\t' || input[end - 1] == '\r')) {
        --end;
      }
      if (end > start) {
        result.append(input, start, end - start);
        result += '\n';
      }
      start = i + 1;
    }
  }
  return result;
}

std::vector<std::string> SplitLines(const std::string &s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      if (!cur.empty())
        lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    lines.push_back(cur);
  return lines;
}

bool CellsApproxEqual(const std::string &a, const std::string &b) {
  if (a == b)
    return true;
  try {
    double da = std::stod(a);
    double db = std::stod(b);
    double scale = std::max({1.0, std::fabs(da), std::fabs(db)});
    return std::fabs(da - db) <= 1e-6 * scale;
  } catch (...) {
    return false;
  }
}

void RequireOrderedMatch(const std::string &ref, const std::string &accel,
                         const std::string &label) {
  auto rlines = SplitLines(TrimLines(ref));
  auto alines = SplitLines(TrimLines(accel));
  Require(rlines.size() == alines.size(),
          label + ": row count mismatch ref=" + std::to_string(rlines.size()) +
              " accel=" + std::to_string(alines.size()) + "\nref:\n" + TrimLines(ref) +
              "accel:\n" + TrimLines(accel));
  for (size_t i = 0; i < rlines.size(); ++i) {
    auto SplitPipe = [](const std::string &line) {
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
    };
    auto rc = SplitPipe(rlines[i]);
    auto ac = SplitPipe(alines[i]);
    Require(rc.size() == ac.size(),
            label + ": col count mismatch at row " + std::to_string(i));
    for (size_t j = 0; j < rc.size(); ++j) {
      Require(CellsApproxEqual(rc[j], ac[j]),
              label + ": cell mismatch row=" + std::to_string(i) + " col=" +
                  std::to_string(j) + " ref=[" + rc[j] + "] accel=[" + ac[j] + "]");
    }
  }
}

// Substitute sequential '?' placeholders with SF1 defaults for TPC-H queries.
std::string SubstituteParams(int query_num, const std::string &sql_template) {
  std::vector<std::string> params;
  switch (query_num) {
  case 1:
    params = {"90"};
    break;
  case 2:
    params = {"15", "%BRASS", "EUROPE"};
    break;
  case 3:
    // base_sql has 3 '?'; accel has 2
    params = {"BUILDING", "1995-03-15", "1995-03-15"};
    break;
  case 4:
    params = {"1993-07-01"};
    break;
  case 5:
    params = {"ASIA", "1994-01-01"};
    break;
  case 6:
    params = {"1994-01-01", "0.05", "0.07", "24"};
    break;
  case 14:
    params = {"1995-09-01"};
    break;
  default:
    params = {"BUILDING", "1995-03-15", "1995-03-15"};
    break;
  }
  std::string out;
  out.reserve(sql_template.size());
  size_t pi = 0;
  for (size_t i = 0; i < sql_template.size(); ++i) {
    if (sql_template[i] == '?' && pi < params.size()) {
      out += params[pi++];
    } else {
      out += sql_template[i];
    }
  }
  return out;
}

std::string ConcreteBaseSql(int query_num, const PostgresSharedQueryCatalogEntry &entry) {
  return SubstituteParams(query_num, entry.base_sql ? entry.base_sql : "");
}

std::string ConcreteAccelSql(int query_num, const PostgresSharedQueryCatalogEntry &entry,
                             bool prefer_aux) {
  const char *tmpl =
      prefer_aux && entry.aux_sql && *entry.aux_sql ? entry.aux_sql : entry.accelerator_sql;
  Require(tmpl && *tmpl, "catalog missing accelerator/aux SQL for Q" + std::to_string(query_num));
  return SubstituteParams(query_num, tmpl);
}

bool PreferAux(int /*query_num*/) {
  const char *kind = std::getenv("ACCEL_PG_DIFF_KIND");
  if (kind && std::string(kind) == "aux")
    return true;
  if (kind && std::string(kind) == "base")
    return false;
  // Auto: prefer aux when ACCEL_WORKLOAD_KIND=aux_state
  const char *wk = std::getenv("ACCEL_WORKLOAD_KIND");
  return wk && std::string(wk) == "aux_state";
}

void EnsureExtension(const Tools &tools) {
  if (!tools.extension_so.empty()) {
    RunSQL(tools, "LOAD '" + tools.extension_so + "'");
  }
}

void RunReadDifferential(const Tools &tools, int query_num, bool prefer_aux) {
  const auto &entry = LookupPostgresQuery("tpch", query_num);
  const std::string ref_sql = ConcreteBaseSql(query_num, entry);
  const std::string accel_sql = ConcreteAccelSql(query_num, entry, prefer_aux);

  std::cerr << "differential(read): tpch Q" << query_num
            << (prefer_aux ? " aux" : " base") << '\n';
  std::cerr << "  reference: " << ref_sql << '\n';
  std::cerr << "  accel: " << accel_sql << '\n';

  auto ref = QuerySQL(tools, ref_sql);
  Require(ref.first, "reference SQL failed");
  auto accel = QuerySQL(tools, accel_sql);
  Require(accel.first, "accelerator SQL failed (hard fail — no soft smoke)");
  RequireOrderedMatch(ref.second, accel.second, "read");
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

std::string SqlLiteral(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "''";
    else
      out += c;
  }
  out += "'";
  return out;
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
    p.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + 1000 "
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
    p.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + 100 "
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

std::string RunPsqlScript(const Tools &tools, const std::string &script) {
  const std::string path =
      std::string("/tmp/accel_pg_wm_") + std::to_string(::getpid()) + ".sql";
  {
    std::ofstream out(path);
    out << script;
  }
  std::string cmd = "PGPASSWORD=" + ShellEscape(tools.password) + " " +
                    ShellEscape(tools.psql.string()) + " -h " + ShellEscape(tools.host) +
                    " -p " + std::to_string(tools.port) + " -U " + ShellEscape(tools.user) +
                    " -d " + ShellEscape(tools.database) +
                    " -v ON_ERROR_STOP=1 -q -t -A -f " + ShellEscape(path) + " 2>&1";
  FILE *fp = popen(cmd.c_str(), "r");
  Require(fp != nullptr, "popen psql script failed");
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) {
    out += buf;
  }
  int rc = pclose(fp);
  Require(WEXITSTATUS(rc) == 0, "psql write_maint session failed:\n" + out);
  return out;
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

void EnsureRowCaptureTriggers(const Tools &tools) {
  // Idempotent install so mutations enter the extension observer.
  const std::string so =
      tools.extension_so.empty() ? "accelerator_codegen_pg" : tools.extension_so;
  Require(RunSQL(tools,
                 "CREATE OR REPLACE FUNCTION accel_postgres_capture_mutation_trigger() "
                 "RETURNS trigger AS " +
                     SqlLiteral(so) + ", 'accel_postgres_capture_mutation_trigger' "
                                      "LANGUAGE C"),
          "install capture trigger function failed");
  Require(RunSQL(tools,
                 "CREATE OR REPLACE FUNCTION accel_postgres_get_observer_mutation_count() "
                 "RETURNS bigint AS " +
                     SqlLiteral(so) +
                     ", 'accel_postgres_get_observer_mutation_count' LANGUAGE C STRICT"),
          "install observer mutation_count helper failed");
  const char *tables[] = {"lineitem", "orders", "customer", "part",
                          "partsupp", "supplier", "nation", "region"};
  for (const char *t : tables) {
    std::string sql = std::string(
                          "DO $$ BEGIN "
                          "EXECUTE format('DROP TRIGGER IF EXISTS accel_pg_row_capture_u ON %I', '") +
                      t +
                      "'); "
                      "EXECUTE format("
                      "'CREATE TRIGGER accel_pg_row_capture_u AFTER UPDATE ON %I "
                      "FOR EACH ROW EXECUTE FUNCTION "
                      "accel_postgres_capture_mutation_trigger()', '" +
                      t + "'); END $$";
    Require(RunSQL(tools, sql), std::string("install trigger failed for ") + t);
  }
}

void RunWriteMaintDifferential(const Tools &tools, int query_num, bool prefer_aux) {
  Require(prefer_aux, "write_maint requires aux (ACCEL_PG_DIFF_KIND=aux)");
  RunReadDifferential(tools, query_num, prefer_aux);
  EnsureRowCaptureTriggers(tools);

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

  // One backend session: warm aux Runtime → mutate (triggers) → aux Execute must
  // observe mutation_count and rematch. Do NOT call an external rebuild helper.
  std::ostringstream script;
  script << "LOAD '" << tools.extension_so << "';\n"
         << "SELECT '===WARM===' AS m;\n"
         << accel_sql << ";\n"
         << "SELECT '===MUT_BEFORE===' AS m;\n"
         << "SELECT accel_postgres_get_observer_mutation_count();\n"
         << probe.update_sql << ";\n"
         << "SELECT '===MUT_AFTER===' AS m;\n"
         << "SELECT accel_postgres_get_observer_mutation_count();\n"
         << "SELECT '===ACCEL===' AS m;\n"
         << accel_sql << ";\n"
         << "SELECT '===REF===' AS m;\n"
         << ref_sql << ";\n"
         << "SELECT '===DONE===' AS m;\n"
         << probe.restore_sql << ";\n";

  const std::string session = RunPsqlScript(tools, script.str());
  const auto mut_before = TrimLines(SectionAfter(session, "===MUT_BEFORE==="));
  const auto mut_after = TrimLines(SectionAfter(session, "===MUT_AFTER==="));
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

  RequireOrderedMatch(ref_out, accel_out, "write_maint");
  std::cerr << "PASS write_maint differential Q" << query_num
            << " (invalidating write + mutation_count " << before_n << "->" << after_n
            << " + aux rematch)\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    int query_id = 3;
    if (argc >= 2) {
      query_id = std::stoi(argv[1]);
    }

    auto tools = DiscoverTools();
    Require(tools.ok, "PostgreSQL not available: " + tools.error);

    const char *tpch_db = std::getenv("TPCH_PG_DB");
    tools.database = (tpch_db && *tpch_db) ? tpch_db : "accel_tpch_test";

    std::cerr << "postgres differential: " << tools.host << ":" << tools.port
              << " db=" << tools.database << '\n';

    EnsureExtension(tools);

    // Ensure TPC-H tables exist (fail if not loaded).
    auto check = QuerySQL(tools, "SELECT COUNT(*) FROM lineitem");
    Require(check.first, "TPC-H tables missing — run setup_accel_postgres_tpch_test_db.sh");

    const bool prefer_aux = PreferAux(query_id);
    const char *mode = std::getenv("ACCEL_PG_DIFF_MODE");
    const std::string mode_s = mode ? mode : "read";

    if (mode_s == "write_maint") {
      RunWriteMaintDifferential(tools, query_id, prefer_aux);
    } else {
      RunReadDifferential(tools, query_id, prefer_aux);
      // Always also run write_maint for aux workloads unless explicitly disabled.
      if (prefer_aux && (!mode || mode_s == "read")) {
        const char *skip = std::getenv("ACCEL_PG_DIFF_SKIP_WRITE_MAINT");
        if (!(skip && std::string(skip) == "1")) {
          RunWriteMaintDifferential(tools, query_id, prefer_aux);
        }
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_postgres_tpch_differential failed: " << error.what() << '\n';
    return 1;
  }
}
