// Parameter-generality differential for the TPC-H aux accelerators on MySQL.
//
// MySQL port of test_postgres_tpch_params.cpp. For every query, runs the
// reference SQL (the spec template of postgres_tpch_param_templates.hpp,
// rewritten to MySQL dialect below) and mysql_accel_tpch_qN_aux(...) on many
// parameter vectors and compares the result sets. The reference SQL run by
// MySQL itself is ground truth, including its semantics that differ from
// PostgreSQL: the default collation (utf8mb4_0900_ai_ci) makes =, LIKE, GROUP
// BY and ORDER BY on strings case-insensitive (and NO PAD: a trailing space
// matters), DATE +/- INTERVAL returns NULL outside 0000-9999, DECIMAL division
// carries div_precision_increment extra digits, and NULL parameters follow SQL
// three-valued logic.
//
// Parameter classes: a = spec-domain random draws, b = off-spec valid values,
// c = adversarial edge cases, n = NULL parameters (the subset of c with a NULL).
//
// Modes (default: plain on the template DB):
//   --data-perturb  fresh DB copy + off-spec rows (shipdate 1985/2005, segment
//                   'ZZTEST', part size 0/1000, quantity 24.5, 'GREEN', special
//                   comments, ...). Run twice: writes BEFORE the first
//                   accelerator call (rebuild path) and AFTER a warm-up call
//                   with the capture triggers installed (incremental path).
//   --wide-keys     fresh DB copy with every key shifted beyond the narrow
//                   packings (orderkey +5e9, suppkey +2e6, partkey +4e7,
//                   custkey +3e8), FK-consistent; spec draws for all queries.
//   --negative-keys fresh DB copy with every key (incl. nation/region) negated.
//   --plain         also run plain when another mode is given.
//
// The copy lives in database accel_tpch_params; the plugin's scans are pointed
// at it with mysql_accel_set_scan_database() + mysql_accel_invalidate_all(),
// and back at the template database afterwards.
//
// Usage: test_mysql_tpch_params [queries|all] [draws_per_class=10] [seed=1]
//          [--keep-going] [--data-perturb] [--wide-keys] [--negative-keys] [--plain]
//          [--max-edge=N] [--tol=X (loose numeric compare)] [--classes=abcn] [--timeout-ms=N]
//   queries: all | 6 | 1,3,5 | 3-7
// Env: TPCH_MYSQL_DB (template DB) plus the usual MYSQL_* variables.
// Exit status: 0 = all calls matched, 1 = at least one mismatch/error, 2 = harness error.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "mysql_tpch_harness.hpp"
#include "postgres_tpch_param_templates.hpp"

namespace {

using accel::mysql::Tools;
using namespace accel::mysql::tpch_harness;
namespace tp = accel::postgres::tpch_params;

struct Options {
  std::vector<int> queries;
  int draws = 10;
  uint64_t seed = 1;
  bool keep_going = false, perturb = false, wide = false, negative = false, plain = false;
  bool strict_text = true; // numeric cells must match MySQL's DECIMAL text exactly (scale included)
  int max_edge = 0;
  double tol = 1e-9;
  int timeout_ms = 0; // reference SELECTs are cut off after this long (skipped, not failed)
  std::string classes = "abcn";
  bool classes_explicit = false;
};

std::string RenderMy(const std::string &tmpl, const char *types, const tp::Vec &p);

// ---- MySQL access (mysql CLI; the plugin state lives in mysqld, so one
// session per statement is as good as a persistent one) ---------------------

struct Res {
  bool ok = false, crashed = false;
  std::string text, err;
};

Res Try(const Tools &tools, const std::string &sql) {
  const std::string cmd = MysqlPrefix(tools) + " -e " + ShellEscape(sql) + " 2>&1";
  Res out;
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp) {
    out.err = "popen failed";
    return out;
  }
  std::string raw;
  char buf[4096];
  while (fgets(buf, sizeof(buf), fp)) raw += buf;
  const int rc = pclose(fp);
  raw = TabsToPipes(StripMysqlWarnings(raw));
  if (WEXITSTATUS(rc) != 0 || raw.rfind("ERROR ", 0) == 0) {
    out.err = raw;
    while (!out.err.empty() && out.err.back() == '\n') out.err.pop_back();
    out.crashed = out.err.find("Lost connection") != std::string::npos || out.err.find("gone away") != std::string::npos ||
                  out.err.find("Can't connect") != std::string::npos;
    return out;
  }
  out.ok = true;
  out.text = raw;
  return out;
}

void Exec(const Tools &tools, const std::string &sql) {
  auto r = Try(tools, sql);
  Require(r.ok, sql.substr(0, 300) + ": " + r.err);
}

// After a server crash (docker restarts mysqld in place): wait until it answers.
void WaitForServer(const Tools &tools) {
  for (int i = 0; i < 180; ++i) {
    if (Try(tools, "SELECT 1").ok) return;
    sleep(1);
  }
}

// ---- comparison ----------------------------------------------------------

// Whole-string numeric parse (the shared CellsApproxEqual accepts prefixes,
// so "3-MEDIUM" == "3-OTHER" and 1995-01-01 == 1995-06-30 there).
bool ParseNum(const std::string &s, double &out) {
  if (s.empty()) return false;
  char *end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size() && std::isfinite(out);
}
bool CellEq(const std::string &a, const std::string &b, double tol) {
  if (a == b) return true;
  if (tol < 0) return false; // --strict-text: numeric text (scale included) must be identical
  double x, y;
  if (!ParseNum(a, x) || !ParseNum(b, y)) return false;
  return std::fabs(x - y) <= tol * std::max({1.0, std::fabs(x), std::fabs(y)});
}
std::vector<std::string> Cells(const std::string &line) {
  std::vector<std::string> c;
  std::string cur;
  for (char ch : line) {
    if (ch == '|') {
      c.push_back(cur);
      cur.clear();
    } else cur += ch;
  }
  c.push_back(cur);
  return c;
}
std::vector<std::string> Rows(const std::string &t) {
  std::vector<std::string> r;
  std::stringstream ss(t);
  for (std::string l; std::getline(ss, l);) r.push_back(l);
  // mysql prints the UDF's text result (which ends in a newline) plus a newline.
  while (!r.empty() && r.back().empty()) r.pop_back();
  return r;
}
std::string Clip(const std::string &s, size_t n = 160) { return s.size() > n ? s.substr(0, n) + "..." : s; }

// "" if equal, else a one-line description of the first difference.
std::string DiffRows(const std::vector<std::string> &ref, const std::vector<std::string> &acc, double tol) {
  const size_t n = std::min(ref.size(), acc.size());
  for (size_t i = 0; i < n; ++i) {
    const auto rc = Cells(ref[i]), ac = Cells(acc[i]);
    if (rc.size() != ac.size()) return "row " + std::to_string(i) + " column count ref=" + std::to_string(rc.size()) + " accel=" + std::to_string(ac.size());
    for (size_t j = 0; j < rc.size(); ++j)
      if (!CellEq(rc[j], ac[j], tol))
        return "first diff row " + std::to_string(i) + " col " + std::to_string(j) + " ref=[" + Clip(rc[j]) + "] accel=[" + Clip(ac[j]) + "] (ref row: " + Clip(ref[i]) + " | accel row: " + Clip(acc[i]) + ")";
  }
  if (ref.size() != acc.size())
    return "row count ref=" + std::to_string(ref.size()) + " accel=" + std::to_string(acc.size()) + " (first extra " +
           (ref.size() > acc.size() ? "ref row " + std::to_string(n) + ": " + Clip(ref[n]) : "accel row " + std::to_string(n) + ": " + Clip(acc[n])) + ")";
  return {};
}
// Ordered compare; rows tied on the ORDER BY keys may legitimately come in
// any order, so a result that only differs in row order passes.
std::string Compare(const std::string &ref, const std::string &acc, double tol) {
  auto r = Rows(ref), a = Rows(acc);
  auto e = DiffRows(r, a, tol);
  if (e.empty()) return e;
  auto rs = r, as = a;
  std::sort(rs.begin(), rs.end());
  std::sort(as.begin(), as.end());
  if (DiffRows(rs, as, tol).empty()) return {};
  return e;
}

// ---- off-spec data for --data-perturb ------------------------------------

std::vector<std::string> PerturbStatements() {
  const std::string li_cols =
      "l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, "
      "l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment";
  auto Clone = [&](const std::string &where, int ln_add, const std::string &qty, const std::string &disc, const std::string &ship,
                   const std::string &commit, const std::string &receipt, const std::string &mode, const std::string &partadd = "0") {
    return "INSERT INTO lineitem SELECT l_orderkey, l_partkey + " + partadd + ", l_suppkey, l_linenumber + " + std::to_string(ln_add) +
           ", " + qty + ", l_extendedprice, " + disc + ", l_tax, l_returnflag, l_linestatus, " + ship + ", " + commit + ", " + receipt +
           ", l_shipinstruct, " + mode + ", l_comment FROM lineitem WHERE " + where;
  };
  std::vector<std::string> s;
  // lineitems with extreme / unusual dates
  s.push_back(Clone("l_orderkey % 50 = 0 AND l_linenumber < 100", 100, "l_quantity", "l_discount", "DATE '1985-06-01'", "DATE '1985-05-01'", "DATE '1985-06-10'", "l_shipmode"));
  s.push_back(Clone("l_orderkey % 50 = 1 AND l_linenumber < 100", 200, "l_quantity", "l_discount", "DATE '2005-01-01'", "DATE '2004-12-01'", "DATE '2005-01-05'", "l_shipmode"));
  // fractional quantity, discount 0.07 / 0.0, month-end and leap days
  s.push_back(Clone("l_orderkey % 50 = 2 AND l_linenumber < 100", 300, "24.5", "0.07", "DATE '1995-03-31'", "DATE '1995-04-02'", "DATE '1995-04-15'", "l_shipmode"));
  s.push_back(Clone("l_orderkey % 50 = 3 AND l_linenumber < 100", 400, "0.5", "0.0", "DATE '1996-02-29'", "DATE '1996-03-01'", "DATE '1996-03-05'", "l_shipmode"));
  s.push_back(Clone("l_orderkey % 50 = 4 AND l_linenumber < 100", 500, "100", "0.1", "DATE '1997-12-31'", "DATE '1998-01-02'", "DATE '1998-01-31'", "'MAIL '"));
  s.push_back(Clone("l_orderkey % 50 = 5 AND l_linenumber < 100", 550, "l_quantity", "l_discount", "DATE '1994-01-31'", "DATE '1994-02-10'", "DATE '1994-02-28'", "l_shipmode"));
  // huge-quantity shipments on the LAST day of the one-year windows starting 1996-01-01 and 1995-03-01
  // (a window that is 365 days instead of "+ 1 year" misses exactly these)
  s.push_back("INSERT INTO lineitem SELECT (SELECT min(o_orderkey) FROM orders), ps_partkey, ps_suppkey, 800 + ROW_NUMBER() OVER (ORDER BY ps_partkey, ps_suppkey), 1000000, 1000, 0.05, 0.02, 'N', 'O', "
              "DATE '1996-12-31', DATE '1996-12-31', DATE '1997-01-02', 'NONE', 'MAIL', 'leap' FROM partsupp WHERE ps_partkey % 8 IN (0, 1)");
  s.push_back("INSERT INTO lineitem SELECT (SELECT min(o_orderkey) FROM orders), ps_partkey, ps_suppkey, 100000 + ROW_NUMBER() OVER (ORDER BY ps_partkey, ps_suppkey), 1000000, 1000, 0.05, 0.02, 'N', 'O', "
              "DATE '1996-02-29', DATE '1996-02-29', DATE '1996-03-02', 'NONE', 'MAIL', 'leap' FROM partsupp WHERE ps_partkey % 8 IN (2, 3)");
  // parts: GREEN uppercase, size 0 / 1000 / 51, odd brand and container, lowercase type
  s.push_back("INSERT INTO part SELECT p_partkey + 1000000, upper(p_name), p_mfgr, CASE WHEN p_partkey % 80 = 0 THEN 'Brand#99' ELSE p_brand END, "
              "p_type, CASE WHEN p_partkey % 80 = 0 THEN 0 ELSE 1000 END, CASE WHEN p_partkey % 80 = 0 THEN 'ZZ BOX' ELSE p_container END, "
              "p_retailprice, p_comment FROM part WHERE p_partkey % 40 = 0");
  s.push_back("INSERT INTO part SELECT p_partkey + 2000000, CONCAT('xgreenx ', p_name), p_mfgr, p_brand, CONCAT(lower(p_type), '  '), 51, p_container, p_retailprice, "
              "p_comment FROM part WHERE p_partkey % 40 = 1");
  s.push_back("INSERT INTO partsupp SELECT ps_partkey + 1000000, ps_suppkey, ps_availqty, ps_supplycost, ps_comment FROM partsupp WHERE ps_partkey % 40 = 0");
  s.push_back("INSERT INTO partsupp SELECT ps_partkey + 2000000, ps_suppkey, ps_availqty, ps_supplycost, ps_comment FROM partsupp WHERE ps_partkey % 40 = 1");
  s.push_back(Clone("l_partkey % 40 = 0 AND l_linenumber < 100", 600, "l_quantity", "l_discount", "l_shipdate", "l_commitdate", "l_receiptdate", "l_shipmode", "1000000"));
  s.push_back(Clone("l_partkey % 40 = 1 AND l_linenumber < 100", 700, "l_quantity", "l_discount", "l_shipdate", "l_commitdate", "l_receiptdate", "l_shipmode", "2000000"));
  // customers: unknown segment, phone codes of 1/2/3 chars; their orders and lineitems
  s.push_back("INSERT INTO customer SELECT c_custkey + 5000000, c_name, c_address, c_nationkey, CONCAT('99', substr(c_phone, 3)), c_acctbal + 100, 'ZZTEST', "
              "c_comment FROM customer WHERE c_custkey % 100 = 0");
  s.push_back("INSERT INTO customer SELECT c_custkey + 5000000, c_name, c_address, c_nationkey, CONCAT('3-', substr(c_phone, 4)), c_acctbal + 100, 'building', "
              "c_comment FROM customer WHERE c_custkey % 100 = 1");
  s.push_back("INSERT INTO customer SELECT c_custkey + 5000000, c_name, c_address, c_nationkey, CONCAT('300-', substr(c_phone, 4)), c_acctbal + 100, 'BUILDING', "
              "c_comment FROM customer WHERE c_custkey % 100 = 2");
  s.push_back("INSERT INTO orders SELECT o_orderkey + 500000000, o_custkey + 5000000, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, "
              "o_shippriority, o_comment FROM orders WHERE o_custkey % 100 = 0");
  s.push_back("INSERT INTO lineitem SELECT l_orderkey + 500000000, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, "
              "l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment FROM lineitem "
              "WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_custkey % 100 = 0 AND o_custkey < 5000000)");
  // order comments with the special words
  s.push_back("UPDATE orders SET o_comment = 'special requests unusual accounts' WHERE o_orderkey % 97 = 0");
  s.push_back("UPDATE orders SET o_comment = 'xunusualaccounts unusual_accounts 100% special  requests' WHERE o_orderkey % 97 = 1");
  s.push_back("UPDATE orders SET o_comment = 'pending deposits unusual accounts unusual accounts' WHERE o_orderkey % 97 = 2");
  s.push_back("UPDATE orders SET o_orderpriority = '5-NONE' WHERE o_orderkey % 97 = 3");
  // suppliers: more complaints, a cheaper clone supplier in another nation
  s.push_back("UPDATE supplier SET s_comment = 'Customer Complaints xx' WHERE s_suppkey % 7 = 3");
  s.push_back("INSERT INTO supplier SELECT s_suppkey + 7000000, CONCAT(s_name, 'X'), s_address, (s_nationkey + 1) % 25, s_phone, s_acctbal, s_comment FROM supplier WHERE s_suppkey % 5 = 0");
  s.push_back("INSERT INTO partsupp SELECT ps_partkey, ps_suppkey + 7000000, ps_availqty + 10, ps_supplycost / 2, ps_comment FROM partsupp "
              "WHERE ps_suppkey % 5 = 0 AND ps_partkey % 7 = 0");
  // updates / deletes of existing rows
  s.push_back("UPDATE part SET p_brand = 'Brand#23', p_container = 'MED PKG' WHERE p_partkey % 61 = 0");
  s.push_back("UPDATE part SET p_brand = 'Brand#23', p_container = 'JUMBO PACK' WHERE p_partkey % 61 = 1");
  s.push_back("UPDATE part SET p_brand = 'Brand#35', p_container = 'SM CAN' WHERE p_partkey % 61 = 2");
  s.push_back("UPDATE part SET p_brand = 'Brand#45', p_container = 'LG DRUM' WHERE p_partkey % 61 = 3");
  s.push_back("UPDATE lineitem SET l_shipdate = l_shipdate + INTERVAL 1 DAY WHERE l_orderkey % 103 = 5 AND l_linenumber < 100");
  s.push_back("UPDATE lineitem SET l_quantity = l_quantity + 40 WHERE l_orderkey % 211 = 9 AND l_linenumber = 1");
  s.push_back("UPDATE partsupp SET ps_availqty = 0 WHERE ps_partkey % 53 = 0 AND ps_partkey < 1000000");
  s.push_back("DELETE FROM lineitem WHERE l_orderkey % 101 = 7 AND l_linenumber = 2");
  return s;
}

std::vector<std::string> WideKeyStatements() {
  const std::string o = "5000000000", sk = "2000000", pk = "40000000", ck = "300000000";
  return {"UPDATE lineitem SET l_orderkey = l_orderkey + " + o + ", l_suppkey = l_suppkey + " + sk + ", l_partkey = l_partkey + " + pk,
          "UPDATE orders SET o_orderkey = o_orderkey + " + o + ", o_custkey = o_custkey + " + ck,
          "UPDATE customer SET c_custkey = c_custkey + " + ck,
          "UPDATE part SET p_partkey = p_partkey + " + pk,
          "UPDATE partsupp SET ps_partkey = ps_partkey + " + pk + ", ps_suppkey = ps_suppkey + " + sk,
          "UPDATE supplier SET s_suppkey = s_suppkey + " + sk,
          "ANALYZE TABLE lineitem, orders, customer, part, partsupp, supplier"};
}

// Every key negated (FK-consistent; 0 stays 0): keys < 0 must be exact too.
std::vector<std::string> NegativeKeyStatements() {
  return {"UPDATE lineitem SET l_orderkey = -l_orderkey, l_suppkey = -l_suppkey, l_partkey = -l_partkey",
          "UPDATE orders SET o_orderkey = -o_orderkey, o_custkey = -o_custkey",
          "UPDATE customer SET c_custkey = -c_custkey, c_nationkey = -c_nationkey",
          "UPDATE part SET p_partkey = -p_partkey",
          "UPDATE partsupp SET ps_partkey = -ps_partkey, ps_suppkey = -ps_suppkey",
          "UPDATE supplier SET s_suppkey = -s_suppkey, s_nationkey = -s_nationkey",
          "UPDATE nation SET n_nationkey = -n_nationkey, n_regionkey = -n_regionkey",
          "UPDATE region SET r_regionkey = -r_regionkey",
          "ANALYZE TABLE lineitem, orders, customer, part, partsupp, supplier, nation, region"};
}

// ---- run -----------------------------------------------------------------

struct Cell {
  int total = 0, fail = 0;
  std::string example;
};
struct Stat {
  int pass = 0, fail = 0, skip = 0;
  std::array<int, 3> path{0, 0, 0}; // hit / rebuilt / fallback calls
  int nopath = 0;
  std::map<char, Cell> cls;
};
struct Report {
  std::map<std::pair<std::string, int>, Stat> stats; // (mode, q)
  int failures = 0;
};

std::array<long, 3> Counts(const Tools &c) {
  std::array<long, 3> out{0, 0, 0};
  auto r = Try(c, "SELECT mysql_accel_get_execute_counts()");
  if (r.ok) std::sscanf(r.text.c_str(), "hit=%ld rebuilt_on_read=%ld fallback=%ld", &out[0], &out[1], &out[2]);
  return out;
}

struct Case {
  char cls;
  tp::Vec params;
};

std::vector<Case> BuildCases(const tp::QueryTemplate &t, const Options &o, bool spec_only, uint64_t mode_salt) {
  std::vector<Case> cases;
  tp::Rng ga(o.seed * 1000003 + t.q * 131 + mode_salt), gb(o.seed * 999983 + t.q * 137 + mode_salt), gc(o.seed * 7919 + t.q);
  for (char c : o.classes) {
    if (spec_only && c != 'a') continue;
    if (c == 'a')
      for (int i = 0; i < o.draws; ++i) cases.push_back({'a', tp::SpecDraw(t.q, ga)});
    else if (c == 'b')
      for (int i = 0; i < o.draws; ++i) cases.push_back({'b', tp::OffSpecDraw(t.q, gb)});
    else if (c == 'c' || c == 'n') {
      auto e = tp::EdgeCases(t.q);
      auto HasNull = [](const tp::Vec &v) { return std::find(v.begin(), v.end(), tp::kNull) != v.end(); };
      std::vector<tp::Vec> variants{e.base}, pick;
      for (const auto &[slot, vals] : e.vary)
        for (const auto &v : vals) {
          auto p = e.base;
          p[static_cast<size_t>(slot)] = v;
          variants.push_back(p);
        }
      variants.push_back(tp::Vec(e.base.size(), tp::kNull));
      std::vector<tp::Vec> sel;
      for (auto &p : variants)
        if (HasNull(p) == (c == 'n')) sel.push_back(p);
      if (c == 'c' && o.max_edge > 0 && static_cast<int>(sel.size()) > o.max_edge) {
        std::shuffle(sel.begin(), sel.end(), gc); // subsample the one-at-a-time variants only
        sel.resize(static_cast<size_t>(o.max_edge));
      }
      pick = sel;
      for (auto &p : e.must) // hand-written combinations always run
        if (HasNull(p) == (c == 'n')) pick.push_back(p);
      for (auto &p : pick) cases.push_back({c, p});
    }
  }
  // drop exact repeats (keep the first class that produced it)
  std::vector<Case> out;
  std::vector<std::string> seen;
  for (auto &c : cases) {
    const auto key = RenderMy(t.call, t.types, c.params);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
    seen.push_back(key);
    out.push_back(std::move(c));
  }
  return out;
}

// Runs every case of query t on `conn`; returns number of failures.
int RunQuery(const Tools &conn, const tp::QueryTemplate &t, const Options &o, const std::string &mode, bool spec_only, uint64_t mode_salt, Report &rep) {
  Stat &st = rep.stats[{mode, t.q}];
  std::map<std::string, Res> refcache;
  int failures = 0;
  for (const auto &c : BuildCases(t, o, spec_only, mode_salt)) {
    std::string ref_sql = RenderMy(t.ref, t.types, c.params);
    if (o.timeout_ms > 0 && ref_sql.rfind("SELECT ", 0) == 0)
      ref_sql.insert(7, "/*+ MAX_EXECUTION_TIME(" + std::to_string(o.timeout_ms) + ") */ ");
    const std::string acc_sql = RenderMy(t.call, t.types, c.params);
    auto it = refcache.find(ref_sql);
    if (it == refcache.end()) it = refcache.emplace(ref_sql, Try(conn, ref_sql)).first;
    const auto &ref = it->second;
    const bool ref_timeout = !ref.ok && ref.err.find("maximum statement execution time") != std::string::npos;
    if (ref_timeout) {
      st.skip++;
      continue;
    }
    const auto before = Counts(conn);
    const auto acc = Try(conn, acc_sql);
    if (acc.crashed) WaitForServer(conn);
    const auto after = acc.crashed ? before : Counts(conn);
    std::string path = acc.crashed ? "CRASH" : "none";
    if (after[2] > before[2]) { st.path[2]++; path = "fallback"; }
    else if (after[1] > before[1]) { st.path[1]++; path = "rebuilt"; }
    else if (after[0] > before[0]) { st.path[0]++; path = "hit"; }
    else st.nopath++;

    std::string err;
    if (acc.crashed) {
      err = "SERVER CRASH (connection lost): " + Clip(acc.err, 100);
    } else if (ref.ok != acc.ok) {
      err = ref.ok ? "accel ERROR but SQL ok: " + Clip(acc.err, 200) : "SQL ERROR (" + Clip(ref.err, 120) + ") but accel returned " + std::to_string(Rows(acc.text).size()) + " rows";
    } else if (ref.ok) {
      err = Compare(ref.text, acc.text, o.strict_text ? -1.0 : o.tol);
      if (!err.empty() && t.limit) err += " [LIMIT query: check for a tie at the cut]";
    }
    auto &cell = st.cls[c.cls];
    cell.total++;
    if (err.empty()) {
      st.pass++;
      continue;
    }
    st.fail++;
    cell.fail++;
    failures++;
    rep.failures++;
    const std::string line = "FAIL [" + mode + "] q" + std::to_string(t.q) + " class=" + c.cls + " params=" + tp::Describe(t.types, c.params) +
                             " path=" + path + " : " + err;
    std::cout << line << std::endl;
    if (cell.example.empty()) cell.example = line;
    if (!o.keep_going) return failures;
  }
  return failures;
}

void PrintSummary(const Report &rep, const std::string &mode, const std::vector<int> &queries) {
  std::cout << "---- summary [" << mode << "] ----" << std::endl;
  for (int q : queries) {
    auto it = rep.stats.find({mode, q});
    if (it == rep.stats.end()) continue;
    const auto &s = it->second;
    std::cout << "SUMMARY [" << mode << "] q" << q << ": pass=" << s.pass << " fail=" << s.fail << " skip=" << s.skip << " | ";
    for (const auto &[c, cell] : s.cls) std::cout << c << ":" << cell.fail << "/" << cell.total << " ";
    std::cout << "| path hit=" << s.path[0] << " rebuilt=" << s.path[1] << " fallback=" << s.path[2] << " none=" << s.nopath << std::endl;
  }
}

void PrintMatrix(const Report &rep) {
  std::cout << "==== FAILURE MATRIX (mode, query, class -> failed/total, first example) ====" << std::endl;
  for (const auto &[key, s] : rep.stats)
    for (const auto &[c, cell] : s.cls)
      if (cell.fail)
        std::cout << "MATRIX [" << key.first << "] q" << key.second << " " << c << ": " << cell.fail << "/" << cell.total << "  e.g. "
                  << Clip(cell.example, 420) << std::endl;
}

std::vector<int> ParseQueries(const std::string &s) {
  std::vector<int> v;
  if (s == "all") {
    for (int q = 1; q <= 22; ++q) v.push_back(q);
    return v;
  }
  std::stringstream ss(s);
  for (std::string it; std::getline(ss, it, ',');) {
    const auto dash = it.find('-');
    if (dash != std::string::npos)
      for (int q = std::stoi(it.substr(0, dash)); q <= std::stoi(it.substr(dash + 1)); ++q) v.push_back(q);
    else v.push_back(std::stoi(it));
  }
  return v;
}

// ---- PostgreSQL template -> MySQL dialect ----------------------------------

// The shared templates are written for PostgreSQL; MySQL differs in: interval
// literals (INTERVAL 3 MONTH), `||` (OR unless PIPES_AS_CONCAT), `NULL::type`
// casts, and the accelerator entry points (a scalar UDF returning the result
// set as text instead of a set-returning function).
std::string ToMysql(std::string sql) {
  sql = std::regex_replace(sql, std::regex(R"(INTERVAL '1' DAY \* (\{\d\}))"), "INTERVAL $1 DAY");
  sql = std::regex_replace(sql, std::regex(R"(INTERVAL '(\d+)' (\w+))"), "INTERVAL $1 $2");
  sql = std::regex_replace(sql, std::regex(R"('%' \|\| (\{\d\}) \|\| '%')"), "CONCAT('%', $1, '%')");
  sql = std::regex_replace(sql, std::regex(R"(SELECT \* FROM accel_tpch_q(\d+)_aux\()"), "SELECT mysql_accel_tpch_q$1_aux(");
  return sql;
}

std::string RenderMy(const std::string &tmpl, const char *types, const tp::Vec &p) {
  std::string out = tp::Render(tmpl, types, p);
  for (const char *ty : {"date", "text", "integer", "bigint", "numeric"}) {
    const std::string from = std::string("NULL::") + ty;
    for (std::size_t pos = 0; (pos = out.find(from, pos)) != std::string::npos;) out.replace(pos, from.size(), "NULL");
  }
  return out;
}

const tp::QueryTemplate &Tmpl(int q) {
  static const std::vector<tp::QueryTemplate> t = [] {
    auto v = tp::Templates();
    for (auto &x : v) {
      x.ref = ToMysql(x.ref);
      x.call = ToMysql(x.call);
    }
    return v;
  }();
  return t.at(static_cast<size_t>(q - 1));
}

} // namespace

int main(int argc, char **argv) {
  try {
    Options o;
    std::string which = "all";
    int pos = 0;
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--keep-going") o.keep_going = true;
      else if (a == "--data-perturb") o.perturb = true;
      else if (a == "--wide-keys") o.wide = true;
      else if (a == "--negative-keys") o.negative = true;
      else if (a == "--plain") o.plain = true;
      else if (a == "--strict-text") o.strict_text = true;
      else if (a.rfind("--max-edge=", 0) == 0) o.max_edge = std::atoi(a.c_str() + 11);
      else if (a.rfind("--tol=", 0) == 0) { o.tol = std::atof(a.c_str() + 6); o.strict_text = false; } // relative-tolerance compare instead
      else if (a.rfind("--timeout-ms=", 0) == 0) o.timeout_ms = std::atoi(a.c_str() + 13);
      else if (a.rfind("--classes=", 0) == 0) { o.classes = a.substr(10); o.classes_explicit = true; }
      else if (a.rfind("--", 0) == 0) { std::cerr << "unknown option " << a << std::endl; return 2; }
      else if (pos == 0) { which = a; pos++; }
      else if (pos == 1) { o.draws = std::atoi(a.c_str()); pos++; }
      else if (pos == 2) { o.seed = std::strtoull(a.c_str(), nullptr, 10); pos++; }
    }
    o.queries = ParseQueries(which);
    const bool run_plain = o.plain || (!o.perturb && !o.wide && !o.negative);

    const Tools tools = ConnectTpch();
    const std::string templ = tools.database;
    const std::string copy = "accel_tpch_params";
    Tools ctools = tools;
    ctools.database = copy;
    Report rep;
    std::vector<std::string> modes;

    auto PointAt = [&](const std::string &db) {
      Exec(tools, "SELECT mysql_accel_set_scan_database('" + db + "')");
      Exec(tools, "SELECT mysql_accel_invalidate_all()");
    };
    // A fresh copy of the template tables (no capture triggers) that the
    // plugin scans from now on.
    auto Fresh = [&] {
      std::string script = "DROP DATABASE IF EXISTS " + copy + ";\nCREATE DATABASE " + copy + ";\n";
      for (const char *t : {"region", "nation", "customer", "supplier", "part", "partsupp", "orders", "lineitem"})
        script += "CREATE TABLE " + copy + "." + t + " LIKE " + templ + "." + t + ";\nINSERT INTO " + copy + "." + t +
                  " SELECT * FROM " + templ + "." + t + ";\n";
      RunMysqlScript(tools, script);
      PointAt(copy);
    };
    auto Guarded = [&](const std::string &mode, auto &&fn) {
      modes.push_back(mode);
      try {
        fn();
      } catch (const std::exception &e) {
        std::cout << "FAIL [" << mode << "] harness error: " << e.what() << std::endl;
        rep.failures++;
      }
    };
    // The statements of one mode run in one session (FOREIGN_KEY_CHECKS etc. are per session).
    auto RunStatements = [&](const std::vector<std::string> &stmts) {
      std::string script;
      for (const auto &s : stmts) script += s + ";\n";
      RunMysqlScript(ctools, script);
    };
    auto Cleanup = [&] {
      try {
        PointAt(templ);
        Exec(tools, "DROP DATABASE IF EXISTS " + copy);
      } catch (const std::exception &e) {
        std::cout << "cleanup failed: " << e.what() << std::endl;
      }
    };

    if (run_plain)
      Guarded("plain", [&] {
        PointAt(templ);
        for (int q : o.queries) {
          RunQuery(tools, Tmpl(q), o, "plain", false, 0, rep);
          if (!o.keep_going && rep.failures) return;
        }
      });

    if (o.perturb) {
      Guarded("perturb-rebuild", [&] {
        Fresh();
        RunStatements(PerturbStatements()); // before the first accelerator call
        for (int q : o.queries) {
          RunQuery(ctools, Tmpl(q), o, "perturb-rebuild", false, 11, rep);
          if (!o.keep_going && rep.failures) return;
        }
      });
      Guarded("perturb-delta", [&] {
        Fresh();
        for (int q : o.queries) { // warm-up: build every accelerator's state on the unperturbed data
          tp::Rng g(o.seed + static_cast<uint64_t>(q));
          const auto &t = Tmpl(q);
          Try(ctools, RenderMy(t.call, t.types, tp::SpecDraw(q, g)));
        }
        EnsureMutationTriggers(ctools); // capture on the copy
        RunStatements(PerturbStatements()); // incremental maintenance
        for (int q : o.queries) {
          RunQuery(ctools, Tmpl(q), o, "perturb-delta", false, 13, rep);
          if (!o.keep_going && rep.failures) return;
        }
      });
    }

    if (o.wide)
      Guarded("wide-keys", [&] {
        Fresh();
        RunStatements(WideKeyStatements());
        PointAt(copy); // the plugin rebuilds its state from the rewritten data
        for (int q : o.queries) {
          RunQuery(ctools, Tmpl(q), o, "wide-keys", true, 17, rep);
          if (!o.keep_going && rep.failures) return;
        }
      });

    if (o.negative)
      Guarded("negative-keys", [&] {
        Fresh();
        RunStatements(NegativeKeyStatements());
        PointAt(copy);
        for (int q : o.queries) {
          RunQuery(ctools, Tmpl(q), o, "negative-keys", true, 19, rep);
          if (!o.keep_going && rep.failures) return;
        }
      });

    Cleanup();
    for (const auto &m : modes) PrintSummary(rep, m, o.queries);
    PrintMatrix(rep);
    std::cout << (rep.failures == 0 ? "ALL PASS" : std::to_string(rep.failures) + " FAILURES") << std::endl;
    return rep.failures == 0 ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "test_mysql_tpch_params failed: " << e.what() << std::endl;
    return 2;
  }
}
