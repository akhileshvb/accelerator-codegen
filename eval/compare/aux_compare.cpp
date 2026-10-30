// Compare aux accelerators -- the C++ ones (accel_tpch_qN) and the Verus kits
// (accel_tpch_qN_vgen) -- against DuckDB, on one TPC-H database.
//
//   aux_compare read       <db> <accel_id> <baseline_kit> <cases.tsv> [reps=5]
//   aux_compare concurrent <db> <accel_id> <baseline_kit> <cases.tsv> <phase> <readers> <seconds>
//                          <writes_per_sec> [seed=1] [warmup_sec=2]
//
// <accel_id>: a Verus kit (tpch_q6_vgen) or a C++ accelerator as tpch:<N>.
// cases.tsv: one case per line, "<accelerator args>\t<baseline args>": the same
// TPC-H parameters as the accelerator's argument list and as the baseline's
// ($1, $2, ... of <baseline_kit>'s SQL: the TPC-H catalog text the Verus kits
// implement).  An empty field is an empty argument list.
//
// read: build the accelerator (timed), then per case the median latency of
//   SELECT * FROM accel_<id>(args) and of the baseline query, and whether the
//   two answers agree.
// concurrent: <readers> reader connections run the cases closed-loop for
//   <seconds> while one writer commits a deterministic TPC-H write stream at
//   <writes_per_sec> (0: none): RF2 / RF1 refresh mini-chunks (an order with its
//   lineitems deleted at the low end of the key range / inserted above the
//   maximum; every fourth RF1 clones a large order, sum(l_quantity) > 300), and every fifth write a point write rotating over supplier,
//   customer, part (+partsupp), supplier (+partsupp), partsupp, lineitem and
//   orders, so every table a TPC-H query reads changes during a run.
//   phase=accel: readers call the accelerator; the writer commits through the
//     accelerator's runtime (captured, maintained).  The store is ACCEL_AUX_STORE
//     (unset: the C++ store; verus_global: the one Verus world store).
//   phase=base: readers run the baseline query; the writer commits plain SQL.
//   Run each phase on its own copy of the database (the writes change it).
//   Reported: reader throughput and latency percentiles (after the warmup),
//   writer commit latency (measured window) and the slowest commit (whole run),
//   how many writes (whole run, warmup included) made the accelerator maintain
//   its state (and how), and for how many cases the answer changed over the run.
//   A commit that outlasts the run is waited for (the writer stops after it).
//
//   aux_compare multi <db> <phase> <spec.tsv> <readers> <seconds> <writes_per_sec>
//                     [seed=1] [warmup_sec=2] [rotate|pinned]
// <writes_per_sec>: a rate or a comma list ("5,10,20,-1"): the accelerators are
// built once, then one run per rate (each with its own warmup), in order, each
// on the database and state the previous left; one JSON line per rate.  < 0:
// uncapped, the writer commits back to back.
// Several accelerators live in one process (a database with many
// accelerators).  spec.tsv: one per line, "<accel_id>\t<baseline_kit>\t<cases.tsv>";
// <cases.tsv> may be "r0.tsv|r1.tsv|...", one parameter draw per repetition
// (the same count for every accelerator): each rate then runs once per
// repetition with that draw, one JSON line per (rate, repetition), rate-major.
//   phase=accel: every accelerator is built; each commit of the writer (the
//     same stream as `concurrent`) is ONE engine transaction maintaining all of
//     them (DuckdbRuntime::ApplyWriteAll).  The C++ store has one lock per
//     accelerator; ACCEL_AUX_STORE=verus_global has one store and lock for all,
//     so here unrelated accelerators' commits and reads meet.
//   phase=base: the same readers on DuckDB, plain SQL writes.
//   rotate: <readers> threads each cycle over the accelerators (and each one's
//   cases); pinned: <readers> threads per accelerator, each running only its
//   cases (so one slow accelerator does not pace the others' reads).
//   Reported per accelerator: reads, latency percentiles, time spent in the
//   store choosing the state (its lock, a pending commit), maintenance and
//   install time per commit, whether its answers changed; the writer's commit
//   latency and where it went; totals of read and commit time; and a per-write
//   log over the measured window.
// ACCEL_RF_ONLY=1: RF1 / RF2 commits only, no dimension point writes.
// ACCEL_RF_BATCH_ORDERS=B (default 1): each RF1 / RF2 commit inserts / deletes B orders with their
// lineitems instead of one (the TPC-H refresh functions move 1500 x SF orders each).
//   aux_compare tpch <db> <phase> <spec.tsv> <streams> <runs> <sf> [check=1]: the TPC-H power and throughput
// tests (see Tpch).
//   aux_compare refresh <db> <accel_id> <pairs> [seed=1]: one accelerator, `pairs` RF2 + RF1 commits of
// ACCEL_RF_BATCH_ORDERS orders each (see Refresh).
// Output: one JSON object on stdout.
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/shared_query_engine.hpp"
#include "backends/duckdb/api/register.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"
#include "substrate/verus/kit/verus_kit.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace engine = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace kit = accelerator_codegen::verus_kit;
using engine::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

void Require(bool ok, const std::string &msg) {
  if (!ok)
    throw std::runtime_error(msg);
}

double Us(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}

std::string Json(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\', out += c;
    else if (c == '\n')
      out += "\\n";
    else if (static_cast<unsigned char>(c) < 0x20)
      out += ' ';
    else
      out += c;
  }
  return out + "\"";
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty())
    return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<std::size_t>(p * (v.size() - 1) + 0.5))];
}

// This process's resident and peak resident memory, MB (/proc/self/status).
std::pair<double, double> RssMb() {
  std::ifstream in("/proc/self/status");
  double rss = 0, hwm = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("VmRSS:", 0) == 0)
      rss = std::stod(line.substr(6)) / 1024;
    else if (line.rfind("VmHWM:", 0) == 0)
      hwm = std::stod(line.substr(6)) / 1024;
  }
  return {rss, hwm};
}

// Resident memory with DuckDB's buffer cache dropped (memory limit lowered,
// then restored) and freed heap returned to the OS: what the process holds
// besides cached table data -- before and after a build, the difference is
// roughly the accelerator's state.
double SettledRssMb(duckdb::Connection &c) {
  auto limit = c.Query("SELECT current_setting('memory_limit')");
  Require(limit && !limit->HasError(), "memory_limit");
  const auto old = limit->GetValue(0, 0).ToString();
  c.Query("SET memory_limit = '256MB'");
  malloc_trim(0);
  const double rss = RssMb().first;
  c.Query("SET memory_limit = '" + old + "'");
  return rss;
}

struct Case {
  std::string accel_args, base_args;
};

std::vector<Case> ReadCases(const std::string &path) {
  std::ifstream in(path);
  Require(in.good(), "cannot read " + path);
  std::vector<Case> cases;
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#')
      continue;
    const auto tab = line.find('\t');
    cases.push_back({line.substr(0, tab), tab == std::string::npos ? "" : line.substr(tab + 1)});
  }
  Require(!cases.empty(), path + ": no cases");
  return cases;
}

// <id>: a Verus kit or an accelerator id; <workload>:<N>: that catalog query's
// C++ accelerator (their ids are not uniform: tpch_q6, tpch_q13_aux, ...).
const engine::SharedQueryCatalogEntry &EntryFor(const std::string &id) {
  if (const auto *e = engine::VerusKitCatalogEntry(id))
    return *e;
  if (const auto colon = id.find(':'); colon != std::string::npos)
    return engine::LookupSharedQuery(id.substr(0, colon), std::stoi(id.substr(colon + 1)));
  const auto *e = engine::FindSharedQueryByAccelerator(id);
  Require(e != nullptr, "no accelerator registered as " + id);
  return *e;
}

// The baseline: <baseline_kit>'s SQL (the TPC-H catalog text, $1..$n), prepared.
struct Baseline {
  std::string sql;
};

duckdb::vector<duckdb::Value> ArgValues(duckdb::Connection &c, const std::string &args) {
  duckdb::vector<duckdb::Value> values;
  if (args.empty())
    return values;
  auto r = c.Query("SELECT " + args);
  Require(r && !r->HasError() && r->RowCount() == 1, "args (" + args + "): " + (r ? r->GetError() : ""));
  for (duckdb::idx_t i = 0; i < r->ColumnCount(); ++i)
    values.push_back(r->GetValue(i, 0));
  return values;
}

// The answer as canonical rows (numbers to 4 decimals), and its row count.
std::vector<std::string> Canon(duckdb::MaterializedQueryResult &r) {
  std::vector<std::string> rows;
  for (duckdb::idx_t i = 0; i < r.RowCount(); ++i) {
    std::string row;
    for (duckdb::idx_t j = 0; j < r.ColumnCount(); ++j) {
      const auto v = r.GetValue(j, i);
      std::string cell;
      if (v.IsNull()) {
        cell = "NULL";
      } else if (v.type().IsNumeric() && !v.type().IsIntegral()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.4f", v.GetValue<double>());
        cell = buf;
      } else {
        cell = v.ToString();
        while (!cell.empty() && cell.back() == ' ')
          cell.pop_back(); // CHAR padding
      }
      row += cell + "|";
    }
    rows.push_back(row);
  }
  return rows;
}

// Whether two sorted canonical answers agree: the same text, or numbers equal up to the noise of summing doubles in
// another order (a SUM of 1e13-sized values differs in its last digits).
bool SameAnswers(const std::vector<std::string> &x, const std::vector<std::string> &y) {
  const auto cells = [](const std::string &row) {
    std::vector<std::string> out;
    std::stringstream in(row);
    for (std::string cell; std::getline(in, cell, '|');)
      out.push_back(cell);
    return out;
  };
  const auto same_cell = [](const std::string &a, const std::string &b) {
    if (a == b)
      return true;
    char *end_a = nullptr, *end_b = nullptr;
    const double u = std::strtod(a.c_str(), &end_a), v = std::strtod(b.c_str(), &end_b);
    return !a.empty() && !b.empty() && !*end_a && !*end_b && std::fabs(u - v) <= 1e-4 + 1e-12 * std::max(std::fabs(u), std::fabs(v));
  };
  if (x.size() != y.size())
    return false;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (x[i] == y[i])
      continue;
    const auto a = cells(x[i]), b = cells(y[i]);
    if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin(), same_cell))
      return false;
  }
  return true;
}

// The baseline's SQL: a Verus kit's query text, or (no such kit built in) the file named by <baseline>,
// e.g. workloads/tpch/q6.sql: the same text with the same $n parameters.
std::string BaselineSql(const std::string &baseline) {
  if (const auto *glue = kit::Find(baseline))
    return glue->query_sql;
  std::ifstream in(baseline);
  Require(in.good(), "no baseline kit or SQL file " + baseline);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::unique_ptr<duckdb::MaterializedQueryResult> RunBaseline(duckdb::Connection &c, const std::string &sql,
                                                             const std::string &args) {
  auto stmt = c.Prepare(sql);
  Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed");
  auto values = ArgValues(c, args);
  auto r = stmt->Execute(values, false);
  Require(r && !r->HasError(), r ? r->GetError() : "baseline failed");
  return duckdb::unique_ptr_cast<duckdb::QueryResult, duckdb::MaterializedQueryResult>(std::move(r));
}

// The accelerator's table function: accel_<id> for a Verus kit; for a C++
// accelerator, the one its catalog entry reads through (accel_<id>_aux).
std::string FunctionFor(const std::string &id) {
  if (kit::Find(id))
    return "accel_" + id;
  const std::string sql = EntryFor(id).duckdb_aux_sql ? EntryFor(id).duckdb_aux_sql : "";
  const auto at = sql.find("accel_");
  Require(at != std::string::npos, id + ": no table function in its catalog entry");
  auto end = at;
  while (end < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[end])) || sql[end] == '_'))
    ++end;
  return sql.substr(at, end - at);
}

// The stateless kernel of catalog query tpch:<N>: accel_tpch_q<N>.
std::string StatelessFunctionFor(const std::string &id) {
  Require(id.rfind("tpch:", 0) == 0, id + ": the stateless kernels are registered for tpch:<N> only");
  return "accel_tpch_q" + id.substr(5);
}

std::unique_ptr<duckdb::MaterializedQueryResult> RunAccel(duckdb::Connection &c, const std::string &fn,
                                                          const std::string &args) {
  auto r = c.Query("SELECT * FROM " + fn + "(" + args + ")");
  Require(r && !r->HasError(), r ? r->GetError() : "accelerator failed");
  return r;
}

// ACCEL_RF_BATCH_ORDERS: orders per RF1 / RF2 commit (default 1, the mini-chunk).  The TPC-H refresh
// functions each move 0.1% of the orders, i.e. 1500 x SF.
std::size_t RfBatchOrders() {
  const char *v = std::getenv("ACCEL_RF_BATCH_ORDERS");
  const long n = v && *v ? std::strtol(v, nullptr, 10) : 1;
  Require(n >= 1, "ACCEL_RF_BATCH_ORDERS must be >= 1");
  return static_cast<std::size_t>(n);
}

// ACCEL_RF_DIR=<dir>: use the TPC-H update sets that the official `dbgen -s SF -U N` wrote there
// (orders.tbl.uK, lineitem.tbl.uK, delete.K, K = 1..N) for the RF1 / RF2 commits, instead of cloning orders.
// Commit 2K-1 is RF1 of set K (insert its orders and lineitems), commit 2K is RF2 of set K (delete its orders
// and their lineitems), the order of the specification.  ACCEL_RF_BATCH_ORDERS=B then uses only the first B
// orders of each set (default: the whole set, 1500 x SF orders = 0.1% of the orders).
std::string RfDir() {
  const char *v = std::getenv("ACCEL_RF_DIR");
  return v && *v ? std::string(v) : std::string();
}
std::size_t RfSetOrders() { // 0: the whole set
  const char *v = std::getenv("ACCEL_RF_BATCH_ORDERS");
  return v && *v ? static_cast<std::size_t>(std::max<long>(1, std::strtol(v, nullptr, 10))) : 0;
}

// ACCEL_RF_ONLY=1: the write stream is RF2 / RF1 only (no dimension point writes).
bool RfOnly() {
  const char *v = std::getenv("ACCEL_RF_ONLY");
  return v && *v && std::string(v) != "0";
}

// ACCEL_DIM_AFTER_RF=1: every RF1 / RF2 commit is followed by one point write of each kind that changes
// the tables RF leaves alone (supplier update, part delete + insert, supplier delete + insert, partsupp update;
// Q2, Q11, Q16 read only part / partsupp / supplier / nation / region), unpaced: the writer's rate is the
// rate of the refreshes.  (Nation and region writes are left out: they rebuild many accelerators and the
// tables are static in practice.)
bool DimAfterRf() {
  const char *v = std::getenv("ACCEL_DIM_AFTER_RF");
  return v && *v && std::string(v) != "0";
}

// A deterministic TPC-H write stream (see the header).
class Writes {
public:
  // batch: orders per RF1 / RF2 commit (1: the mini-chunk).  Dimension point writes are always one row.
  Writes(duckdb::Connection &c, std::size_t max_writes, uint64_t seed, std::size_t batch = 1)
      : rng(seed), batch(std::max<std::size_t>(batch, 1)), rf_only(RfOnly()), dim_after_rf(DimAfterRf()),
        use_sets(!RfDir().empty()), set_orders(RfSetOrders()) {
    const auto n = std::to_string((max_writes + 16) * this->batch);
    low_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " + n);
    template_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey DESC LIMIT 1000");
    // large orders (TPC-H Q18's "large volume customer" orders): every fourth
    // RF1 clones one, so writes also change answers that only such orders reach
    if (!use_sets)
      large_orders = Keys(c, "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING sum(l_quantity) > 300 "
                             "ORDER BY l_orderkey DESC LIMIT 200");
    mid_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " + n +
                             " OFFSET (SELECT count(*) // 2 FROM orders)");
    const auto dims = std::to_string(dim_after_rf ? max_writes + 16 : max_writes / 5 + 16);
    low_parts = Keys(c, "SELECT p_partkey FROM part ORDER BY p_partkey LIMIT " + dims);
    template_parts = Keys(c, "SELECT p_partkey FROM part ORDER BY p_partkey DESC LIMIT 500");
    low_supps = Keys(c, "SELECT s_suppkey FROM supplier ORDER BY s_suppkey LIMIT " + dims);
    template_supps = Keys(c, "SELECT s_suppkey FROM supplier ORDER BY s_suppkey DESC LIMIT 200");
    mid_supps = Keys(c, "SELECT s_suppkey FROM supplier ORDER BY s_suppkey LIMIT " + dims +
                            " OFFSET (SELECT count(*) // 2 FROM supplier)");
    custs = Keys(c, "SELECT c_custkey FROM customer ORDER BY c_custkey LIMIT " + dims +
                        " OFFSET (SELECT count(*) // 3 FROM customer)");
    mid_parts = Keys(c, "SELECT p_partkey FROM part ORDER BY p_partkey LIMIT " + dims +
                            " OFFSET (SELECT count(*) // 2 FROM part)");
    next_order = Keys(c, "SELECT max(o_orderkey) FROM orders")[0] + 1;
    next_part = Keys(c, "SELECT max(p_partkey) FROM part")[0] + 1;
    next_supp = Keys(c, "SELECT max(s_suppkey) FROM supplier")[0] + 1;
    if (use_sets)
      LoadRfSets(c, max_writes);
  }

  bool UsesSets() const { return use_sets; }
  std::size_t SetsLoaded() const { return sets_loaded; }
  // JSON fragment (no braces): where the RF1 / RF2 data came from
  std::string RfInfoJson() const {
    if (!use_sets)
      return "\"rf_source\":\"clone\"";
    return "\"rf_source\":\"tpch_update_sets\",\"rf_sets_loaded\":" + std::to_string(sets_loaded) +
           ",\"rf_orders_per_set\":" + std::to_string(set_orders_total) + ",\"rf_orders_used\":" +
           std::to_string(set_orders ? std::min(set_orders, set_orders_total) : set_orders_total);
  }

  std::pair<std::string, std::string> Next() {
    const auto s = [](int64_t v) { return std::to_string(v); };
    int dim_kind = -1; // the point write to make, or -1: a refresh
    if (dim_after_rf) {
      const auto pos = seq++ % (1 + sizeof(kRfDimKinds) / sizeof(kRfDimKinds[0]));
      if (pos > 0)
        dim_kind = kRfDimKinds[pos - 1];
    } else {
      const auto i = seq++;
      if (!(rf_only || i % 5 != 4))
        dim_kind = static_cast<int>(dim++ % 9);
    }
    if (dim_kind < 0 && use_sets)
      return NextRfFromSets();
    if (dim_kind < 0) { // refresh: RF2 / RF1 alternating (ACCEL_RF_ONLY: no point writes)
      if (rf++ % 2 == 0) {
        if (batch == 1) {
          const auto k = s(Take(low_orders, low_at));
          return {"rf2", "DELETE FROM lineitem WHERE l_orderkey = " + k + "; DELETE FROM orders WHERE o_orderkey = " + k};
        }
        std::string keys;
        for (std::size_t j = 0; j < batch; ++j)
          keys += (j ? "," : "") + s(Take(low_orders, low_at));
        return {"rf2", "DELETE FROM lineitem WHERE l_orderkey IN (" + keys + "); DELETE FROM orders WHERE o_orderkey IN (" +
                           keys + ")"};
      }
      // RF1: B new orders (with their lineitems), each a clone of a template order; every fourth order
      // (as before) clones a large one, so writes also change the answers only such orders reach
      std::string src, dst;
      bool any_large = false;
      for (std::size_t j = 0; j < batch; ++j) {
        const bool large = rf1++ % 4 == 3 && !large_orders.empty();
        any_large |= large;
        const auto &pool = large ? large_orders : template_orders;
        src += (j ? "," : "") + s(pool[rng() % pool.size()]);
        dst += (j ? "," : "") + s(next_order++);
      }
      const std::string kind = batch == 1 ? (any_large ? "rf1 large order" : "rf1") : "rf1";
      if (batch == 1)
        return {kind, "INSERT INTO orders SELECT * REPLACE (" + dst + " AS o_orderkey) FROM orders WHERE o_orderkey = " + src +
                          "; INSERT INTO lineitem SELECT * REPLACE (" + dst + " AS l_orderkey) FROM lineitem WHERE l_orderkey = " + src};
      const std::string m = "(SELECT unnest([" + src + "]::BIGINT[]) AS src, unnest([" + dst + "]::BIGINT[]) AS dst) m";
      return {kind, "INSERT INTO orders SELECT o.* REPLACE (m.dst AS o_orderkey) FROM orders o JOIN " + m +
                        " ON o.o_orderkey = m.src; INSERT INTO lineitem SELECT l.* REPLACE (m.dst AS l_orderkey) FROM lineitem l JOIN " +
                        m + " ON l.l_orderkey = m.src"};
    }
    static const char *segments[] = {"BUILDING", "AUTOMOBILE", "MACHINERY", "HOUSEHOLD", "FURNITURE"};
    static const char *priorities[] = {"1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"};
    switch (dim_kind) {
    case 0:
      return {"supplier update", "UPDATE supplier SET s_acctbal = s_acctbal + 1, s_comment = s_comment || ' ' "
                                 "WHERE s_suppkey = " + s(Take(mid_supps, mid_supp_at))};
    case 1:
      return {"customer update", std::string("UPDATE customer SET c_acctbal = c_acctbal + 1, c_mktsegment = '") +
                                     segments[rng() % 5] + "' WHERE c_custkey = " + s(Take(custs, cust_at))};
    case 2: {
      const auto k = s(Take(low_parts, low_part_at));
      return {"part delete", "DELETE FROM partsupp WHERE ps_partkey = " + k + "; DELETE FROM part WHERE p_partkey = " + k};
    }
    case 3: {
      const auto from = s(template_parts[rng() % template_parts.size()]);
      const auto k = s(next_part++);
      return {"part insert", "INSERT INTO part SELECT * REPLACE (" + k + " AS p_partkey) FROM part WHERE p_partkey = " +
                                 from + "; INSERT INTO partsupp SELECT * REPLACE (" + k +
                                 " AS ps_partkey) FROM partsupp WHERE ps_partkey = " + from};
    }
    case 4: {
      const auto k = s(Take(low_supps, low_supp_at));
      return {"supplier delete",
              "DELETE FROM partsupp WHERE ps_suppkey = " + k + "; DELETE FROM supplier WHERE s_suppkey = " + k};
    }
    case 5: {
      const auto from = s(template_supps[rng() % template_supps.size()]);
      const auto k = s(next_supp++);
      return {"supplier insert", "INSERT INTO supplier SELECT * REPLACE (" + k +
                                     " AS s_suppkey) FROM supplier WHERE s_suppkey = " + from +
                                     "; INSERT INTO partsupp SELECT * REPLACE (" + k +
                                     " AS ps_suppkey) FROM partsupp WHERE ps_suppkey = " + from};
    }
    case 6:
      return {"partsupp update", "UPDATE partsupp SET ps_supplycost = ps_supplycost + 1, ps_availqty = ps_availqty + 1 "
                                 "WHERE ps_partkey = " + s(Take(mid_parts, mid_part_at))};
    case 7:
      return {"lineitem update", "UPDATE lineitem SET l_quantity = l_quantity + 1, l_discount = 0.04 "
                                 "WHERE l_orderkey = " + s(Take(mid_orders, mid_order_at))};
    default:
      return {"orders update", std::string("UPDATE orders SET o_comment = o_comment || ' unusual accounts', "
                                           "o_orderpriority = '") + priorities[rng() % 5] +
                                   "' WHERE o_orderkey = " + s(Take(mid_orders, mid_order_at))};
    }
  }

  // Whether the next write starts a refresh slot (paced by the writer's rate) or follows one (a point write).
  bool NextIsPaced() const { return !dim_after_rf || seq % (1 + sizeof(kRfDimKinds) / sizeof(kRfDimKinds[0])) == 0; }

private:
  // Stage every update set the run can use (at most max_writes / 2 + 2) in tables of the working database, with the
  // schema of the base tables: rf_o_K (orders), rf_l_K (lineitem), rf_d_K (the keys RF2 deletes).
  void LoadRfSets(duckdb::Connection &c, std::size_t max_writes) {
    const std::string dir = RfDir();
    const auto exists = [&](const std::string &f) { return std::ifstream(f).good(); };
    const auto columns = [&](const std::string &table) {
      auto r = c.Query("SELECT column_name, data_type FROM duckdb_columns() WHERE table_name = '" + table +
                       "' ORDER BY column_index");
      Require(r && !r->HasError() && r->RowCount() > 0, "no columns of " + table);
      std::vector<std::pair<std::string, std::string>> cols;
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
        cols.emplace_back(r->GetValue(0, i).ToString(), r->GetValue(1, i).ToString());
      return cols;
    };
    // the .tbl files end every line with '|': one extra (empty) column
    const auto stage = [&](const std::string &name, const std::string &file,
                           const std::vector<std::pair<std::string, std::string>> &cols) {
      std::string sel, rc;
      for (std::size_t j = 0; j < cols.size() + 1; ++j) {
        rc += (j ? "," : "") + std::string("'c") + std::to_string(j) + "':'VARCHAR'";
        if (j < cols.size())
          sel += (j ? ", " : "") + std::string("CAST(c") + std::to_string(j) + " AS " + cols[j].second + ") AS " + cols[j].first;
      }
      auto r = c.Query("CREATE OR REPLACE TABLE " + name + " AS SELECT " + sel + " FROM read_csv('" + file +
                       "', delim='|', header=false, columns={" + rc + "})");
      Require(r && !r->HasError(), r ? r->GetError() : "staging " + name);
    };
    const auto orders_cols = columns("orders"), lineitem_cols = columns("lineitem");
    const std::size_t want = max_writes / 2 + 2;
    while (sets_loaded < want && exists(dir + "/orders.tbl.u" + std::to_string(sets_loaded + 1)) &&
           exists(dir + "/lineitem.tbl.u" + std::to_string(sets_loaded + 1)) &&
           exists(dir + "/delete." + std::to_string(sets_loaded + 1))) {
      const auto k = std::to_string(++sets_loaded);
      stage("rf_o_" + k, dir + "/orders.tbl.u" + k, orders_cols);
      stage("rf_l_" + k, dir + "/lineitem.tbl.u" + k, lineitem_cols);
      stage("rf_d_" + k, dir + "/delete." + k, {{"k", "BIGINT"}});
    }
    Require(sets_loaded > 0, "no TPC-H update sets in " + dir + " (scripts/gen_tpch_refresh_sets.sh)");
    set_orders_total = Keys(c, "SELECT count(*) FROM rf_o_1")[0];
  }

  // RF1 of set K for commit 2K-1, RF2 of set K for commit 2K.
  std::pair<std::string, std::string> NextRfFromSets() {
    const auto idx = rf++;
    const auto set = idx / 2 + 1;
    Require(set <= sets_loaded, "TPC-H update sets exhausted after " + std::to_string(sets_loaded) +
                                     " (generate more with dbgen -U, or lower the rates / seconds)");
    const auto k = std::to_string(set);
    const auto lim = std::to_string(set_orders);
    if (idx % 2 == 0) { // RF1: insert the orders of set K and their lineitems
      if (!set_orders)
        return {"rf1", "INSERT INTO orders SELECT * FROM rf_o_" + k + "; INSERT INTO lineitem SELECT * FROM rf_l_" + k};
      const std::string keys = "SELECT o_orderkey FROM rf_o_" + k + " ORDER BY o_orderkey LIMIT " + lim;
      return {"rf1", "INSERT INTO orders SELECT * FROM rf_o_" + k + " WHERE o_orderkey IN (" + keys +
                         "); INSERT INTO lineitem SELECT * FROM rf_l_" + k + " WHERE l_orderkey IN (" + keys + ")"};
    }
    // RF2: delete the orders listed in delete.K, with their lineitems
    const std::string keys = set_orders ? "SELECT k FROM rf_d_" + k + " ORDER BY k LIMIT " + lim : "SELECT k FROM rf_d_" + k;
    return {"rf2", "DELETE FROM lineitem WHERE l_orderkey IN (" + keys + "); DELETE FROM orders WHERE o_orderkey IN (" + keys + ")"};
  }

  // supplier update, part delete, part insert, supplier delete, supplier insert, partsupp update
  static constexpr int kRfDimKinds[] = {0, 2, 3, 4, 5, 6};
  static std::vector<int64_t> Keys(duckdb::Connection &c, const std::string &sql) {
    auto r = c.Query(sql);
    Require(r && !r->HasError(), r ? r->GetError() : sql);
    std::vector<int64_t> keys;
    for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
      keys.push_back(r->GetValue(0, i).GetValue<int64_t>());
    Require(!keys.empty(), "no keys: " + sql);
    return keys;
  }
  static int64_t Take(const std::vector<int64_t> &keys, std::size_t &at) {
    Require(at < keys.size(), "write stream: out of keys (raise the key pools)");
    return keys[at++];
  }

  std::mt19937_64 rng;
  std::size_t batch;
  bool rf_only;
  bool dim_after_rf;
  bool use_sets;
  std::size_t set_orders; // orders used from each update set (0: all)
  std::size_t sets_loaded = 0, set_orders_total = 0;
  std::vector<int64_t> low_orders, template_orders, large_orders, mid_orders, low_parts, template_parts, low_supps, template_supps,
      mid_supps, custs, mid_parts;
  std::size_t low_at = 0, mid_order_at = 0, low_part_at = 0, low_supp_at = 0, mid_supp_at = 0, cust_at = 0,
              mid_part_at = 0;
  int64_t next_order = 0, next_part = 0, next_supp = 0;
  uint64_t seq = 0, rf = 0, rf1 = 0, dim = 0;
};

// One plain-SQL write transaction (the baseline's commit): the statements of `sql` one by one in a single
// transaction, so each INSERT / UPDATE / DELETE reports its row count; returns the rows touched.
uint64_t PlainWrite(duckdb::Connection &c, const std::string &sql) {
  c.BeginTransaction();
  uint64_t rows = 0;
  try {
    for (auto &statement : c.ExtractStatements(sql)) {
      auto r = c.Query(std::move(statement));
      if (!r || r->HasError())
        throw std::runtime_error(r ? r->GetError() : "null result");
      if (r->ColumnCount() == 1 && r->RowCount() == 1 && r->names[0] == "Count")
        rows += static_cast<uint64_t>(r->GetValue(0, 0).GetValue<int64_t>());
    }
  } catch (...) {
    if (c.HasActiveTransaction())
      c.Rollback();
    throw;
  }
  c.Commit();
  return rows;
}

// ACCEL_COMPARE_DUCKDB_THREADS=N: DuckDB's thread count (default: its own, all cores).  A process-wide setting: it
// limits the baseline queries and also the base-table scans the accelerators' builds and maintenance run through
// DuckDB.  (The accelerators' own read kernels are all single-threaded.)
void ApplyDuckdbThreads(duckdb::Connection &c) {
  if (const char *n = std::getenv("ACCEL_COMPARE_DUCKDB_THREADS"); n && *n) {
    auto r = c.Query(std::string("SET threads = ") + n);
    Require(r && !r->HasError(), r ? r->GetError() : "SET threads");
  }
}

// ACCEL_COMPARE_DUCKDB_BUILD_THREADS=N (multi): DuckDB's thread count while the accelerators are built, outside the
// measured runs; ACCEL_COMPARE_DUCKDB_THREADS applies again once they are built.
void ApplyDuckdbBuildThreads(duckdb::Connection &c) {
  if (const char *n = std::getenv("ACCEL_COMPARE_DUCKDB_BUILD_THREADS"); n && *n) {
    auto r = c.Query(std::string("SET threads = ") + n);
    Require(r && !r->HasError(), r ? r->GetError() : "SET threads");
  }
}

int Read(const std::string &path, const std::string &id, const std::string &base_kit,
         const std::vector<Case> &cases, int reps) {
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  ApplyDuckdbThreads(qc);
  const auto base_sql = BaselineSql(base_kit);
  const auto fn = FunctionFor(id);
  std::optional<DuckdbRuntime> rt(std::in_place, wc, EntryFor(id), 10.0);
  const auto b0 = Clock::now();
  rt->Build();
  const double build_ms = Us(b0, Clock::now()) / 1000;
  const double rss_peak = RssMb().second, rss_after = SettledRssMb(qc);
  core::ResetAuxExecuteCounts();
  std::string per = "[";
  double log_sum = 0;
  int n = 0, mismatches = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const auto &c = cases[i];
    auto base = RunBaseline(qc, base_sql, c.base_args); // warm, and the reference answer
    auto acc = RunAccel(qc, fn, c.accel_args);
    const auto br = Canon(*base), ar = Canon(*acc);
    auto bs = br, as = ar;
    std::sort(bs.begin(), bs.end());
    std::sort(as.begin(), as.end());
    const bool match = SameAnswers(bs, as);
    mismatches += match ? 0 : 1;
    std::vector<double> tb, ta;
    for (int r = 0; r < reps; ++r) {
      auto t0 = Clock::now();
      RunBaseline(qc, base_sql, c.base_args);
      tb.push_back(Us(t0, Clock::now()));
      t0 = Clock::now();
      RunAccel(qc, fn, c.accel_args);
      ta.push_back(Us(t0, Clock::now()));
    }
    const double mb = Percentile(tb, 0.5), ma = Percentile(ta, 0.5);
    log_sum += std::log(mb / std::max(ma, 1.0));
    ++n;
    per += std::string(i ? "," : "") + "{\"args\":" + Json(c.accel_args) + ",\"base_us\":" + std::to_string(mb) +
           ",\"accel_us\":" + std::to_string(ma) + ",\"base_rows\":" + std::to_string(br.size()) +
           ",\"accel_rows\":" + std::to_string(ar.size()) + ",\"match\":" + (match ? "true" : "false") + "}";
  }
  const auto counts = core::ReadAuxExecuteCounts();
  const bool verus = rt->UsesVerusStore();
  // the state is what the process gives back when the accelerator goes (scan buffers etc. stay in both)
  rt.reset();
  const double rss_freed = SettledRssMb(qc);
  std::cout << "{\"mode\":\"read\",\"accel\":" << Json(id) << ",\"function\":" << Json(fn) << ",\"store\":" << Json(verus ? "verus" : "cpp")
            << ",\"build_ms\":" << build_ms << ",\"state_mb\":" << rss_after - rss_freed
            << ",\"rss_mb_settled_after\":" << rss_after << ",\"rss_mb_settled_freed\":" << rss_freed
            << ",\"rss_mb_peak_build\":" << rss_peak << ",\"speedup_geomean\":" << (n ? std::exp(log_sum / n) : 0)
            << ",\"mismatches\":" << mismatches << ",\"hits\":" << counts.hit << ",\"fallbacks\":" << counts.fallback
            << ",\"cases\":" << per << "]}" << std::endl;
  return 0;
}

double Mean(const std::vector<double> &v); // defined below

int Concurrent(const std::string &path, const std::string &id, const std::string &base_kit,
               const std::vector<Case> &cases, const std::string &phase, int readers, double seconds,
               double writes_per_sec, uint64_t seed, double warmup) {
  Require(phase == "accel" || phase == "base" || phase == "stateless", "phase must be accel, base or stateless");
  const bool accel = phase == "accel";
  // stateless: the query's stateless kernel (accel_tpch_qN, single-threaded) reading the base tables, no maintained
  // state, so its commits are plain SQL ones as in the base phase
  const bool stateless = phase == "stateless";
  const auto fn = accel ? FunctionFor(id) : stateless ? StatelessFunctionFor(id) : std::string();
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  ApplyDuckdbThreads(qc);
  const auto *glue = kit::Find(base_kit);
  Require(glue != nullptr, "no baseline kit " + base_kit);
  // answers before the run (to count the cases whose answer the writes change)
  std::vector<std::vector<std::string>> before;
  for (const auto &c : cases)
    before.push_back(Canon(*RunBaseline(qc, glue->query_sql, c.base_args)));
  if (stateless) // the kernel must answer every case as the baseline does before it is timed
    for (std::size_t i = 0; i < cases.size(); ++i) {
      auto a = Canon(*RunAccel(qc, fn, cases[i].accel_args)), b = before[i];
      std::sort(a.begin(), a.end());
      std::sort(b.begin(), b.end());
      Require(a == b, fn + "(" + cases[i].accel_args + "): answer differs from the baseline");
    }
  const std::size_t max_writes = static_cast<std::size_t>(writes_per_sec * (seconds + warmup) * 1.2) + 32;
  Writes writes(qc, max_writes, seed, RfBatchOrders());
  std::unique_ptr<DuckdbRuntime> rt;
  double build_ms = 0;
  if (accel) {
    rt = std::make_unique<DuckdbRuntime>(wc, EntryFor(id), 10.0);
    const auto b0 = Clock::now();
    rt->Build();
    build_ms = Us(b0, Clock::now()) / 1000;
  }
  core::ResetAuxExecuteCounts();

  std::atomic<bool> stop{false};
  const auto start = Clock::now();
  const auto measure_from = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(warmup));
  const auto deadline =
      measure_from + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
  std::mutex mu;
  std::vector<double> latencies;
  uint64_t read_errors = 0;
  std::string read_error_example;
  std::vector<std::thread> threads;
  for (int r = 0; r < readers; ++r) {
    threads.emplace_back([&, r] {
      duckdb::Connection c(db);
      std::vector<double> mine;
      uint64_t errors = 0;
      std::string example;
      for (std::size_t i = r; !stop.load(std::memory_order_relaxed); ++i) {
        const auto &cs = cases[i % cases.size()];
        const auto t0 = Clock::now();
        try {
          if (accel || stateless)
            RunAccel(c, fn, cs.accel_args);
          else
            RunBaseline(c, glue->query_sql, cs.base_args);
        } catch (const std::exception &e) {
          if (++errors == 1)
            example = e.what();
          continue;
        }
        const auto t1 = Clock::now();
        if (t0 >= measure_from && t1 <= deadline)
          mine.push_back(Us(t0, t1));
      }
      std::lock_guard<std::mutex> lock(mu);
      latencies.insert(latencies.end(), mine.begin(), mine.end());
      read_errors += errors;
      if (read_error_example.empty())
        read_error_example = example;
    });
  }
  // the writer: this thread
  std::vector<double> commit_us;
  std::map<std::string, uint64_t> maintenance, write_kinds, maintenance_by_kind, rows_by_kind;
  std::string slowest; // the slowest commit's write kind and maintenance action
  double slowest_us = 0;
  uint64_t write_errors = 0, measured_writes = 0, total_writes = 0, measured_rows = 0, total_rows = 0,
           measured_slots = 0;
  double max_commit_us = 0;
  std::string write_error_example;
  if (writes_per_sec > 0) {
    // writes_per_sec paces refresh SLOTS: with ACCEL_DIM_AFTER_RF a slot is an RF commit plus the point writes
    // after it (unpaced, completed after the deadline), otherwise every write is a slot
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / writes_per_sec));
    auto next = start;
    while (Clock::now() < deadline || !writes.NextIsPaced()) {
      const bool slot_start = writes.NextIsPaced();
      if (slot_start) {
        std::this_thread::sleep_until(next);
        next = std::max(next + interval, Clock::now()); // open loop, but no catch-up burst after a slow commit
        if (Clock::now() >= deadline)
          break;
      }
      const auto [kind, sql] = writes.Next();
      const auto t0 = Clock::now();
      core::ClearMaintenanceDecisionLog();
      uint64_t rows = 0;
      try {
        if (accel) {
          rows = rt->ApplyWrite(sql);
        } else {
          rows = PlainWrite(wc, sql);
        }
      } catch (const std::exception &e) {
        if (++write_errors == 1)
          write_error_example = kind + ": " + e.what();
        continue;
      }
      const auto t1 = Clock::now();
      total_rows += rows;
      rows_by_kind[kind] += rows;
      if (slot_start && t0 >= measure_from)
        ++measured_slots;
      if (t0 >= measure_from)
        measured_rows += rows;
      // every write (warmup too) counts for what the writes did to the state;
      // latency and rate only over the measured window
      ++total_writes;
      max_commit_us = std::max(max_commit_us, Us(t0, t1));
      write_kinds[kind]++;
      if (accel) {
        const auto d = core::LastMaintenanceDecision();
        const std::string action =
            d ? accelerator_codegen::aux_state::maintenance::MaintenanceActionName(d->staged_action) : "none";
        maintenance[action]++;
        maintenance_by_kind[kind + ": " + action]++;
        if (Us(t0, t1) > slowest_us) {
          slowest_us = Us(t0, t1);
          slowest = kind + ": " + action;
        }
      }
      if (t0 >= measure_from) {
        commit_us.push_back(Us(t0, t1));
        ++measured_writes;
      }
    }
  } else {
    std::this_thread::sleep_until(deadline);
  }
  stop = true;
  for (auto &t : threads)
    t.join();
  const auto counts = core::ReadAuxExecuteCounts();
  int changed = 0;
  for (std::size_t i = 0; i < cases.size(); ++i)
    changed += Canon(*RunBaseline(qc, glue->query_sql, cases[i].base_args)) != before[i] ? 1 : 0;
  uint64_t invalidating = 0;
  std::string mjson = "{", kjson = "{", bjson = "{";
  for (const auto &[k, v] : maintenance_by_kind)
    bjson += (bjson.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  for (const auto &[k, v] : maintenance) {
    mjson += (mjson.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
    if (k != "none" && k != "NoOp")
      invalidating += v;
  }
  for (const auto &[k, v] : write_kinds)
    kjson += (kjson.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  std::string rjson = "{"; // rows touched per write kind (the whole run)
  for (const auto &[k, v] : rows_by_kind)
    rjson += (rjson.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  std::cout << "{\"mode\":\"concurrent\",\"phase\":" << Json(phase) << ",\"accel\":" << Json(id)
            << ",\"store\":" << Json(!accel ? "none" : rt->UsesVerusStore() ? "verus" : "cpp")
            << ",\"readers\":" << readers << ",\"seconds\":" << seconds << ",\"build_ms\":" << build_ms
            << ",\"reads\":" << latencies.size() << ",\"qps\":" << latencies.size() / seconds
            << ",\"read_p50_us\":" << Percentile(latencies, 0.5) << ",\"read_p95_us\":" << Percentile(latencies, 0.95)
            << ",\"read_p99_us\":" << Percentile(latencies, 0.99) << ",\"read_mean_us\":" << Mean(latencies)
            << ",\"read_errors\":" << read_errors
            << ",\"read_error_example\":" << Json(read_error_example.substr(0, 300))
            << ",\"rf_batch_orders\":" << RfBatchOrders() << "," << writes.RfInfoJson() << ",\"rf_only\":" << (RfOnly() ? "true" : "false") << ",\"dim_after_rf\":" << (DimAfterRf() ? "true" : "false") << ",\"writes_per_sec_target\":" << writes_per_sec << ",\"writes\":" << measured_writes
            << ",\"writes_per_sec\":" << measured_writes / seconds << ",\"slots\":" << measured_slots
            << ",\"slots_per_sec\":" << measured_slots / seconds << ",\"rows_touched\":" << measured_rows
            << ",\"rows_touched_per_sec\":" << measured_rows / seconds << ",\"rows_touched_total\":" << total_rows
            << ",\"commit_p50_us\":" << Percentile(commit_us, 0.5)
            << ",\"commit_p99_us\":" << Percentile(commit_us, 0.99) << ",\"commit_mean_us\":" << Mean(commit_us)
            << ",\"commit_sum_us\":" << Mean(commit_us) * static_cast<double>(commit_us.size())
            << ",\"writes_total\":" << total_writes << ",\"max_commit_us\":" << max_commit_us << ",\"write_errors\":" << write_errors
            << ",\"write_error_example\":" << Json(write_error_example.substr(0, 300)) << ",\"write_kinds\":" << kjson << "},\"rows_by_kind\":" << rjson
            << "},\"maintenance\":" << mjson << "},\"maintenance_by_kind\":" << bjson
            << "},\"slowest_commit\":" << Json(slowest) << ",\"invalidating_writes\":" << invalidating
            << ",\"answers_changed\":" << changed << ",\"cases\":" << cases.size() << ",\"hits\":" << counts.hit
            << ",\"fallbacks\":" << counts.fallback << "}" << std::endl;
  return 0;
}

struct MultiSpec {
  std::string id, base_kit, fn;
  std::string sql; // the baseline query text
  std::vector<Case> cases;                   // the current repetition's
  std::vector<std::vector<Case>> rep_cases;  // one parameter draw per repetition
};

std::string KindsJson(const std::map<std::string, uint64_t> &m) {
  std::string j = "{";
  for (const auto &[k, v] : m)
    j += (j.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  return j + "}";
}

// What the maintenance policy chose per commit for one accelerator (DuckdbRuntime::WriteAllTiming::action: NoOp,
// ApplyDelta, RebuildPartition, RebuildAll, InvalidateAndRebuildOnRead), with the time its PRE_COMMIT took.
struct ActionStat {
  uint64_t n = 0;
  double total_us = 0, max_us = 0;
};
std::string ActionsJson(const std::map<std::string, ActionStat> &m) {
  std::string j = "{";
  for (const auto &[k, v] : m)
    j += (j.size() > 1 ? "," : "") + Json(k) + ":{\"n\":" + std::to_string(v.n) + ",\"mean_us\":" +
         std::to_string(v.n ? v.total_us / static_cast<double>(v.n) : 0.0) + ",\"max_us\":" + std::to_string(v.max_us) + "}";
  return j + "}";
}
// The policy's decision per accelerator and write kind, means over the writes: what it saw (rows, partitions), what it
// predicted each option would cost, and what the maintenance actually took.
struct DecisionStat {
  uint64_t n = 0;
  double rows = 0, partitions = 0, pred_delta = 0, pred_partition = 0, pred_all = 0, pred_invalidate = 0, actual = 0;
};
std::string DecisionsJson(const std::map<std::string, DecisionStat> &m) {
  std::string j = "{";
  for (const auto &[kind, d] : m) {
    const double n = static_cast<double>(std::max<uint64_t>(d.n, 1));
    j += (j.size() > 1 ? "," : "") + Json(kind) + ":{\"n\":" + std::to_string(d.n) + ",\"rows_touched\":" + std::to_string(d.rows / n) +
         ",\"partitions\":" + std::to_string(d.partitions / n) + ",\"pred_apply_delta_us\":" + std::to_string(d.pred_delta / n) +
         ",\"pred_rebuild_partition_us\":" + std::to_string(d.pred_partition / n) + ",\"pred_rebuild_all_us\":" +
         std::to_string(d.pred_all / n) + ",\"pred_invalidate_us\":" + std::to_string(d.pred_invalidate / n) +
         ",\"actual_us\":" + std::to_string(d.actual / n) + "}";
  }
  return j + "}";
}
std::string ActionsByKindJson(const std::map<std::string, std::map<std::string, uint64_t>> &m) {
  std::string j = "{";
  for (const auto &[kind, actions] : m)
    j += (j.size() > 1 ? "," : "") + Json(kind) + ":" + KindsJson(actions);
  return j + "}";
}

double Mean(const std::vector<double> &v) {
  double sum = 0;
  for (const double x : v)
    sum += x;
  return v.empty() ? 0 : sum / v.size();
}

int Multi(const std::string &path, const std::string &phase, const std::string &spec_path, int readers,
          double seconds, const std::vector<double> &rates, uint64_t seed, double warmup,
          const std::string &reader_mode) {
  Require(phase == "accel" || phase == "base" || phase == "stateless", "phase must be accel, base or stateless");
  Require(reader_mode == "rotate" || reader_mode == "pinned", "reader mode must be rotate or pinned");
  const bool pinned = reader_mode == "pinned";
  const bool accel = phase == "accel";
  // stateless: every query's stateless kernel (accel_tpch_qN, single-threaded), no maintained state, so the commits
  // are plain SQL ones as in the base phase
  const bool stateless = phase == "stateless";
  std::vector<MultiSpec> specs;
  {
    std::ifstream in(spec_path);
    Require(in.good(), "cannot read " + spec_path);
    for (std::string line; std::getline(in, line);) {
      if (line.empty() || line[0] == '#')
        continue;
      std::istringstream fields(line);
      MultiSpec m;
      std::string cases_path;
      Require(std::getline(fields, m.id, '\t') && std::getline(fields, m.base_kit, '\t') &&
                  std::getline(fields, cases_path),
              spec_path + ": want <accel_id>\t<baseline_kit>\t<cases.tsv>");
      m.sql = BaselineSql(m.base_kit);
      // "a.tsv|b.tsv|...": one cases file (parameter draw) per repetition
      std::stringstream paths(cases_path);
      for (std::string one; std::getline(paths, one, '|');)
        m.rep_cases.push_back(ReadCases(one));
      Require(specs.empty() || m.rep_cases.size() == specs.front().rep_cases.size(),
              spec_path + ": every accelerator needs the same number of repetitions");
      specs.push_back(std::move(m));
    }
  }
  Require(!specs.empty(), spec_path + ": no accelerators");
  const std::size_t reps = specs.front().rep_cases.size();
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  ApplyDuckdbThreads(qc);
  // ACCEL_COMPARE_DUCKDB_MEMORY: cap DuckDB's buffer cache (default: 80% of
  // RAM), so many accelerators' state and the cache fit in one process
  if (const char *limit = std::getenv("ACCEL_COMPARE_DUCKDB_MEMORY")) {
    auto r = qc.Query(std::string("SET memory_limit = '") + limit + "'");
    Require(r && !r->HasError(), r ? r->GetError() : "memory_limit");
  }
  // one write stream across every rate; uncapped (< 0): key pools for up to 200 writes/s
  double pool_writes = 0;
  for (const double w : rates)
    pool_writes += (w < 0 ? 200 : w) * (seconds + warmup) * 1.2 * reps;
  if (const char *mc = std::getenv("ACCEL_WRITE_MAX_COMMITS"); mc && *mc) // calibration: the run ends after N commits
    pool_writes = std::min(pool_writes, std::strtod(mc, nullptr));
  Writes writes(qc, static_cast<std::size_t>(pool_writes) + 32 * rates.size() * reps, seed, RfBatchOrders());
  if (writes.UsesSets()) { // fail now, not hours in, if the runs need more update sets than were generated
    double commits = 0;
    for (const double w : rates)
      if (w > 0)
        commits += (w * (seconds + warmup) + 2) * reps;
    Require(2 * writes.SetsLoaded() >= commits, "the runs need about " + std::to_string(static_cast<long>(commits)) +
                                                    " RF commits but only " + std::to_string(writes.SetsLoaded()) +
                                                    " update sets (2 commits each) are available: generate more "
                                                    "(scripts/gen_tpch_refresh_sets.sh) or shorten the runs");
  }
  std::vector<std::unique_ptr<DuckdbRuntime>> runtimes;
  std::vector<DuckdbRuntime *> live;
  std::string builds = "{", build_rss = "{";
  double build_ms_total = 0;
  if (accel) {
    ApplyDuckdbBuildThreads(qc);
    for (auto &m : specs) {
      m.fn = FunctionFor(m.id);
      runtimes.push_back(std::make_unique<DuckdbRuntime>(wc, EntryFor(m.id), 10.0));
      const auto b0 = Clock::now();
      runtimes.back()->Build();
      const double ms = Us(b0, Clock::now()) / 1000;
      build_ms_total += ms;
      builds += (builds.size() > 1 ? "," : "") + Json(m.id) + ":" + std::to_string(ms);
      // resident memory after each build: the growth is roughly that accelerator's state
      const auto [rss, peak] = RssMb();
      build_rss += (build_rss.size() > 1 ? "," : "") + Json(m.id) + ":[" + std::to_string(rss) + "," +
                   std::to_string(peak) + "]";
      std::cerr << "built " << m.id << " in " << ms / 1000 << "s, rss " << rss / 1024 << " GB (peak "
                << peak / 1024 << " GB)" << std::endl;
      live.push_back(runtimes.back().get());
    }
    ApplyDuckdbThreads(qc);
  }
  builds += "}";
  build_rss += "}";
  // the accelerators are built once; each (rate, repetition) is its own run
  // (warmup, then the measured window) on the state the previous one left, with
  // that repetition's parameter draw; one JSON line each, rate-major
  for (const double writes_per_sec : rates) {
  for (std::size_t rep = 0; rep < reps; ++rep) {
  for (auto &m : specs)
    m.cases = m.rep_cases[rep];
  // whether the writes changed an answer: checked on each accelerator's first
  // case only (every case, around every window, costs minutes at SF20)
  std::vector<std::vector<std::vector<std::string>>> before(specs.size());
  for (std::size_t q = 0; q < specs.size(); ++q)
    if (!specs[q].cases.empty())
      before[q].push_back(Canon(*RunBaseline(qc, specs[q].sql, specs[q].cases[0].base_args)));
  if (stateless) // every kernel must answer every case of every repetition as the baseline does before it is timed
    for (auto &m : specs) {
      m.fn = StatelessFunctionFor(m.id);
      for (const auto &rep : m.rep_cases)
        for (const auto &cs : rep) {
          auto x = Canon(*RunAccel(qc, m.fn, cs.accel_args));
          auto y = Canon(*RunBaseline(qc, m.sql, cs.base_args));
          std::sort(x.begin(), x.end());
          std::sort(y.begin(), y.end());
          Require(x == y, m.fn + "(" + cs.accel_args + "): answer differs from the baseline");
        }
    }
  core::ResetAuxExecuteCounts();

  std::atomic<bool> stop{false};
  const auto start = Clock::now();
  const auto measure_from = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(warmup));
  const auto deadline =
      measure_from + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
  std::mutex mu;
  // per query: read latency, and the part of it spent in the store choosing
  // the state (its lock, waiting for a pending commit)
  std::vector<std::vector<double>> latencies(specs.size()), waits(specs.size());
  uint64_t read_errors = 0;
  std::string read_error_example;
  std::vector<std::thread> threads;
  // rotate: `readers` threads, each cycling over every query; pinned: `readers`
  // threads per query, each running only its query's cases
  const std::size_t n_threads = pinned ? static_cast<std::size_t>(readers) * specs.size() : readers;
  for (std::size_t r = 0; r < n_threads; ++r) {
    threads.emplace_back([&, r] {
      duckdb::Connection c(db);
      std::vector<std::vector<double>> mine(specs.size()), mine_waits(specs.size());
      uint64_t errors = 0;
      std::string example;
      for (std::size_t i = r; !stop.load(std::memory_order_relaxed); ++i) {
        const auto q = pinned ? r % specs.size() : i % specs.size();
        const auto &m = specs[q];
        const auto &cs = pinned ? m.cases[(i / specs.size() + r / specs.size()) % m.cases.size()]
                                : m.cases[(i / specs.size()) % m.cases.size()];
        const double w0 = core::ThreadStoreResolveMicros();
        const auto t0 = Clock::now();
        try {
          if (accel || stateless)
            RunAccel(c, m.fn, cs.accel_args);
          else
            RunBaseline(c, m.sql, cs.base_args);
        } catch (const std::exception &e) {
          if (++errors == 1)
            example = m.id + ": " + e.what();
          continue;
        }
        const auto t1 = Clock::now();
        if (t0 >= measure_from && t1 <= deadline) {
          mine[q].push_back(Us(t0, t1));
          mine_waits[q].push_back(core::ThreadStoreResolveMicros() - w0);
        }
      }
      std::lock_guard<std::mutex> lock(mu);
      for (std::size_t q = 0; q < specs.size(); ++q) {
        latencies[q].insert(latencies[q].end(), mine[q].begin(), mine[q].end());
        waits[q].insert(waits[q].end(), mine_waits[q].begin(), mine_waits[q].end());
      }
      read_errors += errors;
      if (read_error_example.empty())
        read_error_example = example;
    });
  }
  std::vector<double> commit_us;
  std::map<std::string, uint64_t> write_kinds;
  uint64_t write_errors = 0, measured_writes = 0, total_writes = 0;
  // accel: where each measured commit's time went (DuckdbRuntime::ApplyWriteAll),
  // and per accelerator how many writes (all, warmup too) made it maintain
  DuckdbRuntime::WriteAllTiming timing;
  std::vector<double> statement_us, engine_commit_us, pre_commit_total_us, install_total_us;
  std::vector<double> pre_commit_sum(specs.size(), 0), install_sum(specs.size(), 0), pre_commit_max(specs.size(), 0);
  std::vector<uint64_t> maintained(specs.size(), 0);
  std::vector<std::map<std::string, uint64_t>> maintained_by_kind(specs.size()); // write kind -> writes
  std::vector<std::map<std::string, ActionStat>> action_stats(specs.size());     // maintenance action -> count, time
  std::vector<std::map<std::string, DecisionStat>> decision_stats(specs.size()); // write kind -> the policy's decision
  std::vector<std::map<std::string, std::map<std::string, uint64_t>>> actions_by_kind(specs.size()); // kind -> action -> n
  std::map<std::string, uint64_t> accels_maintained_hist; // writes by how many accelerators they maintained
  uint64_t writes_maintaining_none = 0;
  double max_commit_us = 0;
  std::string write_error_example;
  // one entry per measured write: [start s (from the measured window), kind,
  // commit us, maintenance us (all PRE_COMMITs), install us (all COMMITs),
  // accelerators maintained]
  std::string write_log = "[";
  if (writes_per_sec != 0) {
    // < 0: uncapped, the writer commits back to back
    const auto interval = writes_per_sec < 0 ? Clock::duration::zero()
                                             : std::chrono::duration_cast<Clock::duration>(
                                                   std::chrono::duration<double>(1.0 / writes_per_sec));
    auto next = start;
    // ACCEL_WRITE_MAX_COMMITS=N: the writer (and so the run) ends after N commits (calibration)
    const char *max_env = std::getenv("ACCEL_WRITE_MAX_COMMITS");
    const uint64_t max_commits = max_env && *max_env ? std::strtoull(max_env, nullptr, 10) : 0;
    // (a refresh slot's point writes, ACCEL_DIM_AFTER_RF, run unpaced and complete after the deadline)
    while (Clock::now() < deadline || !writes.NextIsPaced()) {
      if (max_commits && total_writes >= max_commits)
        break;
      if (writes.NextIsPaced()) {
        std::this_thread::sleep_until(next);
        next = std::max(next + interval, Clock::now());
        if (Clock::now() >= deadline)
          break;
      }
      const auto [kind, sql] = writes.Next();
      const auto t0 = Clock::now();
      int last_maintained = 0;
      try {
        if (accel) {
          DuckdbRuntime::ApplyWriteAll(live, sql, &timing);
        } else {
          auto res = wc.Query("BEGIN; " + sql + "; COMMIT;");
          if (res && res->HasError()) {
            wc.Query("ROLLBACK");
            throw std::runtime_error(res->GetError());
          }
        }
      } catch (const std::exception &e) {
        if (++write_errors == 1)
          write_error_example = kind + ": " + e.what();
        continue;
      }
      const auto t1 = Clock::now();
      ++total_writes;
      max_commit_us = std::max(max_commit_us, Us(t0, t1));
      write_kinds[kind]++;
      if (accel) {
        int n = 0;
        for (std::size_t q = 0; q < specs.size(); ++q) {
          const auto &a = timing.action[q];
          if (a != "none") {
            auto &st = action_stats[q][a];
            ++st.n;
            st.total_us += timing.pre_commit_us[q];
            st.max_us = std::max(st.max_us, timing.pre_commit_us[q]);
            actions_by_kind[q][kind][a]++;
            if (a != "NoOp") {
              const auto &d = timing.decision[q];
              auto &ds = decision_stats[q][kind];
              ++ds.n;
              ds.rows += static_cast<double>(d.rows_touched);
              ds.partitions += static_cast<double>(d.affected_partition_count);
              ds.pred_delta += d.predicted_apply_delta_us;
              ds.pred_partition += d.predicted_rebuild_partition_us;
              ds.pred_all += d.predicted_rebuild_all_us;
              ds.pred_invalidate += d.predicted_invalidate_us;
              ds.actual += timing.pre_commit_us[q];
            }
          }
          if (a != "none" && a != "NoOp") {
            ++maintained[q];
            maintained_by_kind[q][kind]++;
            ++n;
          }
        }
        accels_maintained_hist[std::to_string(n)]++;
        writes_maintaining_none += n == 0 ? 1 : 0;
        last_maintained = n;
      }
      if (t0 >= measure_from) {
        commit_us.push_back(Us(t0, t1));
        ++measured_writes;
        double pre = 0, inst = 0;
        if (accel) {
          for (std::size_t q = 0; q < specs.size(); ++q) {
            pre += timing.pre_commit_us[q];
            inst += timing.commit_us[q];
            pre_commit_sum[q] += timing.pre_commit_us[q];
            install_sum[q] += timing.commit_us[q];
            pre_commit_max[q] = std::max(pre_commit_max[q], timing.pre_commit_us[q]);
          }
          statement_us.push_back(timing.statement_us);
          engine_commit_us.push_back(timing.engine_commit_us);
          pre_commit_total_us.push_back(pre);
          install_total_us.push_back(inst);
        }
        write_log += (write_log.size() > 1 ? ",[" : "[") + std::to_string(Us(measure_from, t0) / 1e6) + "," +
                     Json(kind) + "," + std::to_string(Us(t0, t1)) + "," + std::to_string(pre) + "," +
                     std::to_string(inst) + "," + std::to_string(last_maintained) + "]";
      }
    }
  } else {
    std::this_thread::sleep_until(deadline);
  }
  stop = true;
  for (auto &t : threads)
    t.join();
  const auto counts = core::ReadAuxExecuteCounts();
  std::string per = "[";
  uint64_t reads = 0;
  for (std::size_t q = 0; q < specs.size(); ++q) {
    int changed = 0;
    for (std::size_t i = 0; i < before[q].size(); ++i)
      changed += Canon(*RunBaseline(qc, specs[q].sql, specs[q].cases[i].base_args)) != before[q][i] ? 1 : 0;
    reads += latencies[q].size();
    uint64_t waited = 0; // reads that spent over 1 ms in the store
    for (const double w : waits[q])
      waited += w > 1000 ? 1 : 0;
    const double n_measured = static_cast<double>(std::max<std::size_t>(measured_writes, 1));
    per += std::string(q ? "," : "") + "{\"accel\":" + Json(specs[q].id) + ",\"reads\":" +
           std::to_string(latencies[q].size()) + ",\"read_p50_us\":" + std::to_string(Percentile(latencies[q], 0.5)) +
           ",\"read_p99_us\":" + std::to_string(Percentile(latencies[q], 0.99)) +
           ",\"read_max_us\":" + std::to_string(Percentile(latencies[q], 1.0)) +
           ",\"read_mean_us\":" + std::to_string(Mean(latencies[q])) +
           ",\"store_wait_mean_us\":" + std::to_string(Mean(waits[q])) +
           ",\"store_wait_p99_us\":" + std::to_string(Percentile(waits[q], 0.99)) +
           ",\"store_wait_max_us\":" + std::to_string(Percentile(waits[q], 1.0)) +
           ",\"reads_waited_over_1ms\":" + std::to_string(waited) +
           (accel ? ",\"maintained_writes\":" + std::to_string(maintained[q]) +
                        ",\"maintained_by_kind\":" + KindsJson(maintained_by_kind[q]) +
                        ",\"actions\":" + ActionsJson(action_stats[q]) +
                        ",\"actions_by_kind\":" + ActionsByKindJson(actions_by_kind[q]) +
                        ",\"decisions_by_kind\":" + DecisionsJson(decision_stats[q]) +
                        ",\"pre_commit_mean_us\":" + std::to_string(pre_commit_sum[q] / n_measured) +
                        ",\"pre_commit_max_us\":" + std::to_string(pre_commit_max[q]) +
                        ",\"install_mean_us\":" + std::to_string(install_sum[q] / n_measured)
                  : std::string()) +
           ",\"answers_changed\":" + std::to_string(changed) + ",\"answers_checked\":" + std::to_string(before[q].size()) +
           ",\"cases\":" + std::to_string(specs[q].cases.size()) +
           "}";
  }
  std::string kjson = "{", hjson = "{";
  for (const auto &[k, v] : write_kinds)
    kjson += (kjson.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  for (const auto &[k, v] : accels_maintained_hist)
    hjson += (hjson.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  hjson += "}";
  const std::string breakdown =
      accel ? ",\"commit_breakdown_mean_us\":{\"statement\":" + std::to_string(Mean(statement_us)) +
                  ",\"pre_commit_all\":" + std::to_string(Mean(pre_commit_total_us)) +
                  ",\"engine_commit\":" + std::to_string(Mean(engine_commit_us)) +
                  ",\"install_all\":" + std::to_string(Mean(install_total_us)) +
                  "},\"writes_by_accels_maintained\":" + hjson +
                  ",\"writes_maintaining_none\":" + std::to_string(writes_maintaining_none)
            : std::string();
  double read_total_us = 0, commit_total_us = 0;
  for (const auto &l : latencies)
    for (const double x : l)
      read_total_us += x;
  for (const double x : commit_us)
    commit_total_us += x;
  std::cout << "{\"mode\":\"multi\",\"phase\":" << Json(phase) << ",\"accelerators\":" << specs.size()
            << ",\"store\":" << Json(!accel ? "none" : live.front()->UsesVerusStore() ? "verus" : "cpp")
            << ",\"reader_mode\":" << Json(reader_mode) << ",\"readers\":" << readers
            << ",\"reader_threads\":" << n_threads << ",\"seconds\":" << seconds << ",\"build_ms\":" << build_ms_total
            << ",\"build_ms_by_accel\":" << builds << ",\"rss_mb_after_build\":" << build_rss
            << ",\"rss_mb_end\":" << RssMb().first << ",\"rss_mb_peak\":" << RssMb().second << ",\"reads\":" << reads << ",\"qps\":" << reads / seconds
            << ",\"read_total_us\":" << read_total_us << ",\"read_errors\":" << read_errors
            << ",\"read_error_example\":" << Json(read_error_example.substr(0, 300))
            << ",\"rf_batch_orders\":" << RfBatchOrders() << "," << writes.RfInfoJson() << ",\"rf_only\":" << (RfOnly() ? "true" : "false") << ",\"dim_after_rf\":" << (DimAfterRf() ? "true" : "false") << ",\"writes_per_sec_target\":" << writes_per_sec << ",\"writes\":" << measured_writes
            << ",\"writes_total\":" << total_writes << ",\"writes_per_sec\":" << measured_writes / seconds
            << ",\"commit_p50_us\":" << Percentile(commit_us, 0.5) << ",\"commit_p99_us\":" << Percentile(commit_us, 0.99)
            << ",\"commit_total_us\":" << commit_total_us << ",\"max_commit_us\":" << max_commit_us
            << ",\"write_errors\":" << write_errors << ",\"write_error_example\":" << Json(write_error_example.substr(0, 300))
            << ",\"write_kinds\":" << kjson << "}" << breakdown << ",\"write_log\":" << write_log << "]"
            << ",\"hits\":" << counts.hit << ",\"fallbacks\":" << counts.fallback << ",\"rep\":" << rep
            << ",\"per_accel\":" << per << "]}" << std::endl;
  }
  }
  return 0;
}

// The query order of each TPC-H query stream (qgen's permute.h, spec Appendix A): stream 0 is the power test's,
// streams 1..S the throughput test's.
constexpr int kStreamOrder[41][22] = {
    {14, 2, 9, 20, 6, 17, 18, 8, 21, 13, 3, 22, 16, 4, 11, 15, 1, 10, 19, 5, 7, 12},
    {21, 3, 18, 5, 11, 7, 6, 20, 17, 12, 16, 15, 13, 10, 2, 8, 14, 19, 9, 22, 1, 4},
    {6, 17, 14, 16, 19, 10, 9, 2, 15, 8, 5, 22, 12, 7, 13, 18, 1, 4, 20, 3, 11, 21},
    {8, 5, 4, 6, 17, 7, 1, 18, 22, 14, 9, 10, 15, 11, 20, 2, 21, 19, 13, 16, 12, 3},
    {5, 21, 14, 19, 15, 17, 12, 6, 4, 9, 8, 16, 11, 2, 10, 18, 1, 13, 7, 22, 3, 20},
    {21, 15, 4, 6, 7, 16, 19, 18, 14, 22, 11, 13, 3, 1, 2, 5, 8, 20, 12, 17, 10, 9},
    {10, 3, 15, 13, 6, 8, 9, 7, 4, 11, 22, 18, 12, 1, 5, 16, 2, 14, 19, 20, 17, 21},
    {18, 8, 20, 21, 2, 4, 22, 17, 1, 11, 9, 19, 3, 13, 5, 7, 10, 16, 6, 14, 15, 12},
    {19, 1, 15, 17, 5, 8, 9, 12, 14, 7, 4, 3, 20, 16, 6, 22, 10, 13, 2, 21, 18, 11},
    {8, 13, 2, 20, 17, 3, 6, 21, 18, 11, 19, 10, 15, 4, 22, 1, 7, 12, 9, 14, 5, 16},
    {6, 15, 18, 17, 12, 1, 7, 2, 22, 13, 21, 10, 14, 9, 3, 16, 20, 19, 11, 4, 8, 5},
    {15, 14, 18, 17, 10, 20, 16, 11, 1, 8, 4, 22, 5, 12, 3, 9, 21, 2, 13, 6, 19, 7},
    {1, 7, 16, 17, 18, 22, 12, 6, 8, 9, 11, 4, 2, 5, 20, 21, 13, 10, 19, 3, 14, 15},
    {21, 17, 7, 3, 1, 10, 12, 22, 9, 16, 6, 11, 2, 4, 5, 14, 8, 20, 13, 18, 15, 19},
    {2, 9, 5, 4, 18, 1, 20, 15, 16, 17, 7, 21, 13, 14, 19, 8, 22, 11, 10, 3, 12, 6},
    {16, 9, 17, 8, 14, 11, 10, 12, 6, 21, 7, 3, 15, 5, 22, 20, 1, 13, 19, 2, 4, 18},
    {1, 3, 6, 5, 2, 16, 14, 22, 17, 20, 4, 9, 10, 11, 15, 8, 12, 19, 18, 13, 7, 21},
    {3, 16, 5, 11, 21, 9, 2, 15, 10, 18, 17, 7, 8, 19, 14, 13, 1, 4, 22, 20, 6, 12},
    {14, 4, 13, 5, 21, 11, 8, 6, 3, 17, 2, 20, 1, 19, 10, 9, 12, 18, 15, 7, 22, 16},
    {4, 12, 22, 14, 5, 15, 16, 2, 8, 10, 17, 9, 21, 7, 3, 6, 13, 18, 11, 20, 19, 1},
    {16, 15, 14, 13, 4, 22, 18, 19, 7, 1, 12, 17, 5, 10, 20, 3, 9, 21, 11, 2, 6, 8},
    {20, 14, 21, 12, 15, 17, 4, 19, 13, 10, 11, 1, 16, 5, 18, 7, 8, 22, 9, 6, 3, 2},
    {16, 14, 13, 2, 21, 10, 11, 4, 1, 22, 18, 12, 19, 5, 7, 8, 6, 3, 15, 20, 9, 17},
    {18, 15, 9, 14, 12, 2, 8, 11, 22, 21, 16, 1, 6, 17, 5, 10, 19, 4, 20, 13, 3, 7},
    {7, 3, 10, 14, 13, 21, 18, 6, 20, 4, 9, 8, 22, 15, 2, 1, 5, 12, 19, 17, 11, 16},
    {18, 1, 13, 7, 16, 10, 14, 2, 19, 5, 21, 11, 22, 15, 8, 17, 20, 3, 4, 12, 6, 9},
    {13, 2, 22, 5, 11, 21, 20, 14, 7, 10, 4, 9, 19, 18, 6, 3, 1, 8, 15, 12, 17, 16},
    {14, 17, 21, 8, 2, 9, 6, 4, 5, 13, 22, 7, 15, 3, 1, 18, 16, 11, 10, 12, 20, 19},
    {10, 22, 1, 12, 13, 18, 21, 20, 2, 14, 16, 7, 15, 3, 4, 17, 5, 19, 6, 8, 9, 11},
    {10, 8, 9, 18, 12, 6, 1, 5, 20, 11, 17, 22, 16, 3, 13, 2, 15, 21, 14, 19, 7, 4},
    {7, 17, 22, 5, 3, 10, 13, 18, 9, 1, 14, 15, 21, 19, 16, 12, 8, 6, 11, 20, 4, 2},
    {2, 9, 21, 3, 4, 7, 1, 11, 16, 5, 20, 19, 18, 8, 17, 13, 10, 12, 15, 6, 14, 22},
    {15, 12, 8, 4, 22, 13, 16, 17, 18, 3, 7, 5, 6, 1, 9, 11, 21, 10, 14, 20, 19, 2},
    {15, 16, 2, 11, 17, 7, 5, 14, 20, 4, 21, 3, 10, 9, 12, 8, 13, 6, 18, 19, 22, 1},
    {1, 13, 11, 3, 4, 21, 6, 14, 15, 22, 18, 9, 7, 5, 10, 20, 12, 16, 17, 8, 19, 2},
    {14, 17, 22, 20, 8, 16, 5, 10, 1, 13, 2, 21, 12, 9, 4, 18, 3, 7, 6, 19, 15, 11},
    {9, 17, 7, 4, 5, 13, 21, 18, 11, 3, 22, 1, 6, 16, 20, 14, 15, 10, 8, 2, 12, 19},
    {13, 14, 5, 22, 19, 11, 9, 6, 18, 15, 8, 10, 7, 4, 17, 16, 3, 1, 12, 2, 21, 20},
    {20, 5, 4, 14, 11, 1, 6, 16, 8, 22, 7, 3, 2, 12, 21, 19, 17, 13, 10, 15, 18, 9},
    {3, 7, 14, 15, 6, 5, 21, 20, 18, 10, 4, 16, 19, 1, 13, 9, 8, 17, 11, 12, 22, 2},
    {13, 15, 17, 1, 22, 11, 3, 4, 7, 20, 14, 21, 9, 8, 2, 18, 16, 6, 10, 12, 5, 19}};

std::string DoublesJson(const std::vector<double> &v) {
  std::string j = "[";
  for (std::size_t i = 0; i < v.size(); ++i)
    j += (i ? "," : "") + std::to_string(v[i]);
  return j + "]";
}

// aux_compare tpch <db> <phase> <spec.tsv> <streams> <runs> <sf> [check=1]
// The TPC-H performance test (spec 5.3) on one process holding all 22 accelerators: <runs> runs back to back on the
// same database and state, each a power test then a throughput test, as the specification's Run 1, Run 2.
//   power test:      RF1, then the 22 queries of stream 0 in its order, one at a time, then RF2.
//   throughput test: <streams> query streams (1..S, each its own order and parameters) concurrently, each on its own
//                    connection, and one refresh stream applying S RF1 / RF2 pairs back to back.
// spec.tsv: 22 lines, query 1..22 in order, "<accel_id>\t<baseline_kit>\t<cases.tsv>"; case run * (S + 1) + s is
// the parameter set of stream s in run `run`.  The refresh functions are the dbgen update sets (ACCEL_RF_DIR,
// ACCEL_RF_ONLY=1); run r uses sets r * (S + 1) + 1 .. (r + 1) * (S + 1): one for the power test, S for the
// throughput test.  phase=accel: every commit maintains all 22 accelerators in one engine transaction; check=1: in the
// power test each accelerator answer is compared (untimed) to DuckDB's on the same, unchanging state.
// Metrics (spec 5.4), timing intervals in seconds:
//   Power@Size      = 3600 * SF / (prod_i QI(i,0) * RI(1,0) * RI(2,0)) ^ (1/24), where a query interval below
//                     max_i QI(i,0) / 1000 counts as max / 1000 (5.4.1.4); the unadjusted value is reported too
//   Throughput@Size = S * 22 * 3600 / Ts * SF, Ts from the first query's start to the end of the last query stream or
//                     the refresh stream, whichever is later
//   QphH@Size       = sqrt(Power@Size * Throughput@Size)
// One JSON line per run.
int Tpch(const std::string &path, const std::string &phase, const std::string &spec_path, int streams, int runs,
         double sf, bool check) {
  Require(phase == "accel" || phase == "base" || phase == "stateless", "phase must be accel, base or stateless");
  Require(streams >= 1 && streams <= 40, "streams must be 1..40 (the specification's permutations)");
  Require(RfOnly() && !RfDir().empty(), "tpch needs the TPC-H update sets and no point writes (ACCEL_RF_DIR, ACCEL_RF_ONLY=1)");
  const bool accel = phase == "accel", stateless = phase == "stateless";
  const std::size_t per_run = static_cast<std::size_t>(streams) + 1;
  std::vector<MultiSpec> specs;
  {
    std::ifstream in(spec_path);
    Require(in.good(), "cannot read " + spec_path);
    for (std::string line; std::getline(in, line);) {
      if (line.empty() || line[0] == '#')
        continue;
      std::istringstream fields(line);
      MultiSpec m;
      std::string cases_path;
      Require(std::getline(fields, m.id, '\t') && std::getline(fields, m.base_kit, '\t') && std::getline(fields, cases_path),
              spec_path + ": want <accel_id>\t<baseline_kit>\t<cases.tsv>");
      m.sql = BaselineSql(m.base_kit);
      m.cases = ReadCases(cases_path);
      Require(m.cases.size() >= per_run * runs, cases_path + ": want a parameter set per (run, stream)");
      m.fn = accel ? FunctionFor(m.id) : stateless ? StatelessFunctionFor(m.id) : std::string();
      specs.push_back(std::move(m));
    }
  }
  Require(specs.size() == 22, spec_path + ": want the 22 TPC-H queries, 1..22 in order");
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  ApplyDuckdbThreads(qc);
  if (const char *limit = std::getenv("ACCEL_COMPARE_DUCKDB_MEMORY"); limit && *limit) {
    auto r = qc.Query(std::string("SET memory_limit = '") + limit + "'");
    Require(r && !r->HasError(), r ? r->GetError() : "memory_limit");
  }
  const std::size_t pairs = per_run * runs;
  Writes writes(qc, 2 * pairs, 1, RfBatchOrders());
  Require(writes.SetsLoaded() >= pairs, "the runs need " + std::to_string(pairs) + " update sets but only " +
                                            std::to_string(writes.SetsLoaded()) + " are available (scripts/gen_tpch_refresh_sets.sh)");
  std::vector<std::unique_ptr<DuckdbRuntime>> runtimes;
  std::vector<DuckdbRuntime *> live;
  std::string builds = "{";
  double build_ms_total = 0;
  if (accel) {
    ApplyDuckdbBuildThreads(qc);
    for (auto &m : specs) {
      runtimes.push_back(std::make_unique<DuckdbRuntime>(wc, EntryFor(m.id), 10.0));
      const auto b0 = Clock::now();
      runtimes.back()->Build();
      const double ms = Us(b0, Clock::now()) / 1000;
      build_ms_total += ms;
      builds += (builds.size() > 1 ? "," : "") + Json(m.id) + ":" + std::to_string(ms);
      std::cerr << "built " << m.id << " in " << ms / 1000 << "s, rss " << RssMb().first / 1024 << " GB" << std::endl;
      live.push_back(runtimes.back().get());
    }
    ApplyDuckdbThreads(qc);
  }
  builds += "}";

  // one refresh function: its kind (rf1 / rf2), commit time, and in the accel phase the maintenance actions it caused
  // ("RebuildAll:3,ApplyDelta:17": accelerators per action, NoOp left out)
  struct Refresh {
    std::string kind, actions;
    double us = 0, statement_us = 0, engine_commit_us = 0;
    std::vector<double> apply_us; // per accelerator, the maintenance of this commit
  };
  const auto refresh = [&]() {
    const auto [kind, sql] = writes.Next();
    Refresh r{kind, "", 0};
    DuckdbRuntime::WriteAllTiming timing;
    const auto t0 = Clock::now();
    if (accel) {
      DuckdbRuntime::ApplyWriteAll(live, sql, &timing);
    } else {
      auto res = wc.Query("BEGIN; " + sql + "; COMMIT;");
      if (res && res->HasError()) {
        wc.Query("ROLLBACK");
        throw std::runtime_error(kind + ": " + res->GetError());
      }
    }
    r.us = Us(t0, Clock::now());
    if (accel) {
      r.statement_us = timing.statement_us;
      r.engine_commit_us = timing.engine_commit_us;
      r.apply_us = timing.pre_commit_us;
      std::map<std::string, int> n;
      for (const auto &a : timing.action)
        if (a != "none" && a != "NoOp")
          ++n[a];
      for (const auto &[a, k] : n)
        r.actions += (r.actions.empty() ? "" : ",") + a + ":" + std::to_string(k);
    }
    return r;
  };
  const auto query = [&](duckdb::Connection &c, const MultiSpec &m, const Case &cs) {
    const auto t0 = Clock::now();
    if (accel || stateless)
      RunAccel(c, m.fn, cs.accel_args);
    else
      RunBaseline(c, m.sql, cs.base_args);
    return Us(t0, Clock::now());
  };
  const auto refresh_json = [](const std::vector<Refresh> &rs) {
    std::string j = "[";
    for (std::size_t i = 0; i < rs.size(); ++i)
      j += std::string(i ? "," : "") + "{\"kind\":" + Json(rs[i].kind) + ",\"us\":" + std::to_string(rs[i].us) +
           ",\"actions\":" + Json(rs[i].actions) +
           ",\"statement_us\":" + std::to_string(rs[i].statement_us) +
           ",\"engine_commit_us\":" + std::to_string(rs[i].engine_commit_us) +
           ",\"apply_us\":" + DoublesJson(rs[i].apply_us) + "}";
    return j + "]";
  };

  // each accelerator's runtime counters so far (cumulative over the runs)
  const auto accel_stats_json = [&]() {
    std::string j = "[";
    for (std::size_t i = 0; i < live.size(); ++i) {
      const auto st = live[i]->Stats();
      j += std::string(i ? "," : "") + "{\"id\":" + Json(specs[i].id) + ",\"reads_hit\":" + std::to_string(st.reads_hit) +
           ",\"reads_waited\":" + std::to_string(st.reads_waited) + ",\"reads_fallback\":" + std::to_string(st.reads_fallback) +
           ",\"builds\":" + std::to_string(st.builds) + ",\"build_us\":" + std::to_string(st.build_us) +
           ",\"catch_up_commits\":" + std::to_string(st.catch_up_commits) + ",\"commits_applied\":" + std::to_string(st.commits_applied) +
           ",\"apply_us\":" + std::to_string(st.apply_us) + ",\"gaps\":" + std::to_string(st.gaps) + ",\"last_action\":" +
           Json(st.last_decision ? accelerator_codegen::aux_state::maintenance::MaintenanceActionName(st.last_decision->staged_action) : "none") + "}";
    }
    return j + "]";
  };

  for (int run = 0; run < runs; ++run) {
    // ---- power test: RF1, stream 0, RF2
    std::vector<Refresh> power_rf;
    std::vector<double> qi(22, 0); // by query number - 1
    std::string mismatched;
    std::vector<double> fallbacks_by_query(22, 0); // accelerator reads that fell back to DuckDB, per power query
    power_rf.push_back(refresh());
    Require(power_rf.back().kind == "rf1", "power test: the first refresh must be RF1");
    core::ResetAuxExecuteCounts();
    for (const int q : kStreamOrder[0]) {
      const auto &m = specs[q - 1];
      const auto &cs = m.cases[run * per_run];
      qi[q - 1] = query(qc, m, cs);
      fallbacks_by_query[q - 1] = static_cast<double>(core::ReadAuxExecuteCounts().fallback);
      if (check && (accel || stateless)) {
        auto a = Canon(*RunAccel(qc, m.fn, cs.accel_args)), b = Canon(*RunBaseline(qc, m.sql, cs.base_args));
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a != b)
          mismatched += (mismatched.empty() ? "" : ",") + std::to_string(q);
      }
      core::ResetAuxExecuteCounts(); // per query: only its timed read counts
    }
    power_rf.push_back(refresh());
    const double qmax = *std::max_element(qi.begin(), qi.end());
    double log_raw = 0, log_adj = 0;
    int adjusted = 0;
    for (const double us : qi) {
      log_raw += std::log(us / 1e6);
      log_adj += std::log(std::max(us, qmax / 1000) / 1e6);
      adjusted += us < qmax / 1000 ? 1 : 0;
    }
    for (const auto &r : power_rf) {
      log_raw += std::log(r.us / 1e6);
      log_adj += std::log(r.us / 1e6);
    }
    const double power = 3600 * sf / std::exp(log_adj / 24), power_raw = 3600 * sf / std::exp(log_raw / 24);

    // ---- throughput test: streams 1..S and the refresh stream, concurrently
    std::vector<std::vector<double>> stream_qi(streams, std::vector<double>(22, 0));
    std::vector<double> stream_s(streams, 0);
    std::vector<std::string> stream_errors(streams);
    std::vector<Refresh> tput_rf;
    std::string refresh_error;
    double refresh_s = 0;
    core::ResetAuxExecuteCounts();
    const auto t_start = Clock::now();
    std::vector<std::thread> threads;
    for (int s = 1; s <= streams; ++s)
      threads.emplace_back([&, s] {
        try {
          duckdb::Connection c(db);
          for (const int q : kStreamOrder[s])
            stream_qi[s - 1][q - 1] = query(c, specs[q - 1], specs[q - 1].cases[run * per_run + s]);
        } catch (const std::exception &e) {
          stream_errors[s - 1] = e.what();
        }
        stream_s[s - 1] = Us(t_start, Clock::now()) / 1e6;
      });
    threads.emplace_back([&] {
      try {
        for (int k = 0; k < 2 * streams; ++k)
          tput_rf.push_back(refresh());
      } catch (const std::exception &e) {
        refresh_error = e.what();
      }
      refresh_s = Us(t_start, Clock::now()) / 1e6;
    });
    for (auto &t : threads)
      t.join();
    const auto tput_counts = core::ReadAuxExecuteCounts();
    const double queries_s = *std::max_element(stream_s.begin(), stream_s.end());
    const double ts = std::max(queries_s, refresh_s);
    const double throughput = streams * 22 * 3600 / ts * sf;
    std::string errors;
    for (int s = 0; s < streams; ++s)
      if (!stream_errors[s].empty())
        errors += (errors.empty() ? "" : "; ") + ("stream " + std::to_string(s + 1) + ": " + stream_errors[s]);
    if (!refresh_error.empty())
      errors += (errors.empty() ? "" : "; ") + ("refresh stream: " + refresh_error);
    std::string sq = "[";
    for (int s = 0; s < streams; ++s)
      sq += (s ? "," : "") + DoublesJson(stream_qi[s]);
    sq += "]";
    std::cout << "{\"mode\":\"tpch\",\"phase\":" << Json(phase) << ",\"run\":" << run << ",\"sf\":" << sf
              << ",\"streams\":" << streams << ",\"store\":" << Json(!accel ? "none" : live.front()->UsesVerusStore() ? "verus" : "cpp")
              << ",\"build_ms\":" << build_ms_total << ",\"build_ms_by_accel\":" << builds << "," << writes.RfInfoJson()
              << ",\"power_at_size\":" << power << ",\"power_at_size_unadjusted\":" << power_raw
              << ",\"power_queries_adjusted\":" << adjusted << ",\"power_query_us\":" << DoublesJson(qi)
              << ",\"power_fallbacks_by_query\":" << DoublesJson(fallbacks_by_query)
              << ",\"power_refresh\":" << refresh_json(power_rf) << ",\"power_mismatches\":" << Json(mismatched)
              << ",\"power_checked\":" << (check && (accel || stateless) ? "true" : "false")
              << ",\"throughput_at_size\":" << throughput << ",\"ts_s\":" << ts << ",\"query_streams_s\":" << queries_s
              << ",\"refresh_stream_s\":" << refresh_s << ",\"stream_s\":" << DoublesJson(stream_s)
              << ",\"stream_query_us\":" << sq << ",\"throughput_hits\":" << tput_counts.hit
              << ",\"throughput_fallbacks\":" << tput_counts.fallback << ",\"throughput_refresh\":" << refresh_json(tput_rf)
              << ",\"accel_stats\":" << accel_stats_json() << ",\"qphh_at_size\":" << std::sqrt(power * throughput) << ",\"errors\":" << Json(errors.substr(0, 600))
              << ",\"rss_mb_end\":" << RssMb().first << ",\"rss_mb_peak\":" << RssMb().second << "}" << std::endl;
  }
  return 0;
}

} // namespace

// aux_compare cycle <db> <accel_id> <baseline_kit> <cases.tsv> <phase> <pairs> <burst> [seed=1]
// One accelerator alone in the process (as in `read`) and the TPC-H refresh functions applied one commit at a time:
// RF1 of update set 1, RF2 of set 1, RF1 of set 2, ... (<pairs> pairs), each followed by the point writes of the concurrent run
// (ACCEL_RF_DIR and ACCEL_DIM_AFTER_RF=1 are required: a slot = an RF commit + its point writes).
// After a first burst before any write, and after every commit, <burst> reads of the query (the cases in turn) are
// timed.  phase=base: plain SQL commits, baseline reads.  phase=accel: the commits go through the accelerator's runtime
// (capture, maintenance, install), the reads call the accelerator, and after every burst the first case is also
// answered by the baseline and compared.  Per commit: its time (and where it went), the action the policy chose with
// the cost it predicted for each option, the first read after it and the burst's latencies.  Nothing here knows an
// interval: the speedup at any write interval is computed from these pieces (plot_cycle.py).
int Cycle(const std::string &path, const std::string &id, const std::string &base_kit, const std::vector<Case> &cases,
          const std::string &phase, int pairs, int burst, uint64_t seed) {
  Require(phase == "accel" || phase == "base", "phase must be accel or base");
  const bool accel = phase == "accel";
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  ApplyDuckdbThreads(qc);
  if (const char *limit = std::getenv("ACCEL_COMPARE_DUCKDB_MEMORY"); limit && *limit) {
    auto r = qc.Query(std::string("SET memory_limit = '") + limit + "'");
    Require(r && !r->HasError(), r ? r->GetError() : "memory_limit");
  }
  const auto *glue = kit::Find(base_kit);
  Require(glue != nullptr, "no baseline kit " + base_kit);
  const auto fn = accel ? FunctionFor(id) : std::string();
  Writes writes(qc, static_cast<std::size_t>(2 * pairs + 4), seed, RfBatchOrders());
  Require(writes.UsesSets(), "cycle needs the TPC-H update sets (ACCEL_RF_DIR)");
  std::unique_ptr<DuckdbRuntime> rt;
  double build_ms = 0, rss_after = 0;
  if (accel) {
    rt = std::make_unique<DuckdbRuntime>(wc, EntryFor(id), 10.0);
    const auto b0 = Clock::now();
    rt->Build();
    build_ms = Us(b0, Clock::now()) / 1000;
    rss_after = RssMb().second;
  }
  std::size_t next_case = 0;
  const auto run_burst = [&](std::vector<double> &lat, int count) {
    for (int j = 0; j < count; ++j) {
      const auto &cs = cases[next_case++ % cases.size()];
      const auto t0 = Clock::now();
      if (accel)
        RunAccel(qc, fn, cs.accel_args);
      else
        RunBaseline(qc, glue->query_sql, cs.base_args);
      lat.push_back(Us(t0, Clock::now()));
    }
  };
  const auto burst_json = [](const std::vector<double> &lat) {
    return "\"first_read_us\":" + std::to_string(lat.empty() ? 0.0 : lat.front()) + ",\"read_p50_us\":" +
           std::to_string(Percentile(lat, 0.5)) + ",\"read_p99_us\":" + std::to_string(Percentile(lat, 0.99)) +
           ",\"read_mean_us\":" + std::to_string(Mean(lat)) + ",\"reads\":" + std::to_string(lat.size());
  };
  std::vector<double> pre;
  run_burst(pre, burst);
  std::string out = "{\"mode\":\"cycle\",\"phase\":" + Json(phase) + ",\"accel\":" + Json(id) + ",\"build_ms\":" +
                    std::to_string(build_ms) + ",\"rss_mb_after_build\":" + std::to_string(rss_after) + ",\"pairs\":" +
                    std::to_string(pairs) + ",\"burst\":" + std::to_string(burst) + "," + writes.RfInfoJson() +
                    ",\"before\":{" + burst_json(pre) + "},\"writes\":[";
  int mismatches = 0;
  // A slot is one refresh: an RF commit followed by the point writes of ACCEL_DIM_AFTER_RF (the same stream as the concurrent
  // run).  2 * pairs slots: RF1 + its point writes, RF2 + its point writes, ...  A slot ends when the next write is an RF.
  for (int slots = 0, w = 0; slots < 2 * pairs; ++w) {
    const auto [kind, sql] = writes.Next();
    DuckdbRuntime::WriteAllTiming timing;
    const auto t0 = Clock::now();
    if (accel) {
      DuckdbRuntime::ApplyWriteAll({rt.get()}, sql, &timing);
    } else {
      auto res = wc.Query("BEGIN; " + sql + "; COMMIT;");
      if (res && res->HasError()) {
        wc.Query("ROLLBACK");
        throw std::runtime_error(res->GetError());
      }
    }
    const double commit_us = Us(t0, Clock::now());
    const bool is_rf = kind.rfind("rf", 0) == 0;
    const bool slot_end = writes.NextIsPaced();
    // reads after every commit (a lazily rebuilt state shows on the first); a full burst after an RF commit, 3 after a
    // point write (the baseline: none, its reads are slow and a point write does not change them)
    std::vector<double> lat;
    run_burst(lat, is_rf ? burst : (accel ? std::min(burst, 3) : 0));
    std::string extra;
    if (accel) {
      const auto &d = timing.decision.empty() ? core::MaintenanceDecisionLogEntry{} : timing.decision[0];
      if (is_rf || slot_end) { // answers against the baseline after the RF commit and at the end of every slot
        auto base = RunBaseline(qc, glue->query_sql, cases[0].base_args);
        auto acc = RunAccel(qc, fn, cases[0].accel_args);
        auto bs = Canon(*base), as = Canon(*acc);
        std::sort(bs.begin(), bs.end());
        std::sort(as.begin(), as.end());
        const bool match = bs == as;
        mismatches += match ? 0 : 1;
        extra += ",\"match\":" + std::string(match ? "true" : "false");
      }
      extra += ",\"statement_us\":" + std::to_string(timing.statement_us) +
               ",\"pre_commit_us\":" + std::to_string(timing.pre_commit_us.empty() ? 0.0 : timing.pre_commit_us[0]) +
               ",\"engine_commit_us\":" + std::to_string(timing.engine_commit_us) + ",\"install_us\":" +
               std::to_string(timing.commit_us.empty() ? 0.0 : timing.commit_us[0]) + ",\"action\":" +
               Json(timing.action.empty() ? "none" : timing.action[0]) + ",\"rows_touched\":" + std::to_string(d.rows_touched) +
               ",\"partitions\":" + std::to_string(d.affected_partition_count) + ",\"pred_apply_delta_us\":" +
               std::to_string(d.predicted_apply_delta_us) + ",\"pred_rebuild_partition_us\":" +
               std::to_string(d.predicted_rebuild_partition_us) + ",\"pred_rebuild_all_us\":" + std::to_string(d.predicted_rebuild_all_us) +
               ",\"pred_invalidate_us\":" + std::to_string(d.predicted_invalidate_us);
    }
    out += std::string(w ? "," : "") + "{\"kind\":" + Json(kind) + ",\"slot_end\":" + (slot_end ? "true" : "false") +
           ",\"commit_us\":" + std::to_string(commit_us) + "," + burst_json(lat) + extra + "}";
    if (slot_end)
      ++slots;
  }
  out += "],\"mismatches\":" + std::to_string(mismatches) + "}";
  std::cout << out << std::endl;
  return 0;
}

// aux_compare refresh <db> <accel_id> <pairs> [seed=1]
// One accelerator alone in the process, built, then `pairs` RF2 + RF1 commits (ACCEL_RF_BATCH_ORDERS orders each, clones of
// existing orders) through its runtime.  Reported: the build's time and settled resident memory (as in `read`: the state's
// size, roughly), per commit the statement, the accelerator's maintenance (PRE_COMMIT), the engine commit and the action
// chosen, and the settled memory again after the last commit (state growth, old versions).
int Refresh(const std::string &path, const std::string &id, int pairs, uint64_t seed) {
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  ApplyDuckdbThreads(qc);
  Writes writes(qc, static_cast<std::size_t>(2 * pairs + 4), seed, RfBatchOrders());
  std::optional<DuckdbRuntime> rt(std::in_place, wc, EntryFor(id), 10.0);
  const auto b0 = Clock::now();
  rt->Build();
  const double build_ms = Us(b0, Clock::now()) / 1000;
  const double rss_peak = RssMb().second, rss_built = SettledRssMb(qc);
  std::string commits;
  for (int i = 0; i < 2 * pairs; ++i) {
    const auto [kind, sql] = writes.Next();
    DuckdbRuntime::WriteAllTiming timing;
    DuckdbRuntime::ApplyWriteAll({&*rt}, sql, &timing);
    commits += std::string(i ? "," : "") + "{\"kind\":" + Json(kind) + ",\"statement_us\":" + std::to_string(timing.statement_us) +
               ",\"pre_commit_us\":" + std::to_string(timing.pre_commit_us.empty() ? 0.0 : timing.pre_commit_us[0]) +
               ",\"engine_commit_us\":" + std::to_string(timing.engine_commit_us) + ",\"action\":" +
               Json(timing.action.empty() ? "none" : timing.action[0]) + ",\"builds\":" + std::to_string(rt->Stats().builds) +
               ",\"commits_applied\":" + std::to_string(rt->Stats().commits_applied) + "}";
  }
  const double rss_final = SettledRssMb(qc);
  rt.reset();
  const double rss_freed = SettledRssMb(qc);
  std::cout << "{\"mode\":\"refresh\",\"accel\":" << Json(id) << ",\"batch_orders\":" << RfBatchOrders()
            << ",\"build_ms\":" << build_ms << ",\"state_mb\":" << rss_built - rss_freed
            << ",\"rss_mb_peak_build\":" << rss_peak << ",\"rss_mb_settled_built\":" << rss_built
            << ",\"rss_mb_settled_freed\":" << rss_freed << ",\"state_mb_after_commits\":" << rss_final - rss_freed
            << ",\"commits\":[" << commits << "]}" << std::endl;
  return 0;
}

int main(int argc, char **argv) {
  try {
    Require(argc >= 5, "usage: aux_compare read|concurrent|multi|refresh <db> ...");
    const std::string mode = argv[1];
    if (mode == "multi") {
      Require(argc >= 8, "multi: <db> <phase> <spec.tsv> <readers> <seconds> <writes_per_sec> [seed] [warmup_sec] "
                         "[rotate|pinned]");
      std::vector<double> rates; // "5,10,-1": one run per rate, built once
      std::stringstream list(argv[7]);
      for (std::string w; std::getline(list, w, ',');)
        rates.push_back(std::stod(w));
      Require(!rates.empty(), "multi: no writes_per_sec");
      return Multi(argv[2], argv[3], argv[4], std::stoi(argv[5]), std::stod(argv[6]), rates,
                   argc > 8 ? std::stoull(argv[8]) : 1, argc > 9 ? std::stod(argv[9]) : 2,
                   argc > 10 ? argv[10] : "rotate");
    }
    if (mode == "tpch") {
      Require(argc >= 8, "tpch: <db> <phase> <spec.tsv> <streams> <runs> <sf> [check=1]");
      return Tpch(argv[2], argv[3], argv[4], std::stoi(argv[5]), std::stoi(argv[6]), std::stod(argv[7]),
                  argc > 8 ? std::string(argv[8]) != "0" : true);
    }
    if (mode == "refresh")
      return Refresh(argv[2], argv[3], std::stoi(argv[4]), argc > 5 ? std::stoull(argv[5]) : 1);
    const auto cases = ReadCases(argv[5]);
    if (mode == "read")
      return Read(argv[2], argv[3], argv[4], cases, argc > 6 ? std::stoi(argv[6]) : 5);
    if (mode == "cycle") {
      Require(argc >= 9, "cycle: <db> <accel_id> <baseline_kit> <cases.tsv> <phase> <pairs> <burst> [seed]");
      return Cycle(argv[2], argv[3], argv[4], cases, argv[6], std::stoi(argv[7]), std::stoi(argv[8]),
                   argc > 9 ? std::stoull(argv[9]) : 1);
    }
    if (mode == "concurrent") {
      Require(argc >= 10, "concurrent: <phase> <readers> <seconds> <writes_per_sec> [seed] [warmup_sec]");
      return Concurrent(argv[2], argv[3], argv[4], cases, argv[6], std::stoi(argv[7]), std::stod(argv[8]),
                        std::stod(argv[9]), argc > 10 ? std::stoull(argv[10]) : 1, argc > 11 ? std::stod(argv[11]) : 2);
    }
    Require(false, "unknown mode " + mode);
  } catch (const std::exception &e) {
    std::cerr << "aux_compare: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
