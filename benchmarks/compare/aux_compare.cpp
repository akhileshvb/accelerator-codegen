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
// Output: one JSON object on stdout.
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/verus/verus_kit.hpp"
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

// ACCEL_RF_ONLY=1: the write stream is RF2 / RF1 only (no dimension point writes).
bool RfOnly() {
  const char *v = std::getenv("ACCEL_RF_ONLY");
  return v && *v && std::string(v) != "0";
}

// ACCEL_DIM_AFTER_RF=1: every RF1 / RF2 commit is followed by one point write of each kind that changes
// the tables RF leaves alone (part, partsupp, supplier, nation, region; Q2, Q11, Q16 read only those),
// unpaced: the writer's rate is the rate of the refreshes.
bool DimAfterRf() {
  const char *v = std::getenv("ACCEL_DIM_AFTER_RF");
  return v && *v && std::string(v) != "0";
}

// A deterministic TPC-H write stream (see the header).
class Writes {
public:
  // batch: orders per RF1 / RF2 commit (1: the mini-chunk).  Dimension point writes are always one row.
  Writes(duckdb::Connection &c, std::size_t max_writes, uint64_t seed, std::size_t batch = 1)
      : rng(seed), batch(std::max<std::size_t>(batch, 1)), rf_only(RfOnly()), dim_after_rf(DimAfterRf()) {
    const auto n = std::to_string((max_writes + 16) * this->batch);
    low_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " + n);
    template_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey DESC LIMIT 1000");
    // large orders (TPC-H Q18's "large volume customer" orders): every fourth
    // RF1 clones one, so writes also change answers that only such orders reach
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
    nations = Keys(c, "SELECT n_nationkey FROM nation ORDER BY n_nationkey");
    regions = Keys(c, "SELECT r_regionkey FROM region ORDER BY r_regionkey");
    next_order = Keys(c, "SELECT max(o_orderkey) FROM orders")[0] + 1;
    next_part = Keys(c, "SELECT max(p_partkey) FROM part")[0] + 1;
    next_supp = Keys(c, "SELECT max(s_suppkey) FROM supplier")[0] + 1;
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
    case 9:
      return {"nation update", "UPDATE nation SET n_comment = reverse(n_comment) WHERE n_nationkey = " +
                                   s(nations[nation_at++ % nations.size()])};
    case 10:
      return {"region update", "UPDATE region SET r_comment = reverse(r_comment) WHERE r_regionkey = " +
                                   s(regions[region_at++ % regions.size()])};
    default:
      return {"orders update", std::string("UPDATE orders SET o_comment = o_comment || ' unusual accounts', "
                                           "o_orderpriority = '") + priorities[rng() % 5] +
                                   "' WHERE o_orderkey = " + s(Take(mid_orders, mid_order_at))};
    }
  }

  // Whether the next write starts a refresh slot (paced by the writer's rate) or follows one (a point write).
  bool NextIsPaced() const { return !dim_after_rf || seq % (1 + sizeof(kRfDimKinds) / sizeof(kRfDimKinds[0])) == 0; }

private:
  // supplier update, part delete, part insert, supplier delete, supplier insert, partsupp update, nation update, region update
  static constexpr int kRfDimKinds[] = {0, 2, 3, 4, 5, 6, 9, 10};
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
  std::vector<int64_t> nations, regions;
  std::size_t nation_at = 0, region_at = 0;
  std::vector<int64_t> low_orders, template_orders, large_orders, mid_orders, low_parts, template_parts, low_supps, template_supps,
      mid_supps, custs, mid_parts;
  std::size_t low_at = 0, mid_order_at = 0, low_part_at = 0, low_supp_at = 0, mid_supp_at = 0, cust_at = 0,
              mid_part_at = 0;
  int64_t next_order = 0, next_part = 0, next_supp = 0;
  uint64_t seq = 0, rf = 0, rf1 = 0, dim = 0;
};

int Read(const std::string &path, const std::string &id, const std::string &base_kit,
         const std::vector<Case> &cases, int reps) {
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  const auto *glue = kit::Find(base_kit);
  Require(glue != nullptr, "no baseline kit " + base_kit);
  const auto fn = FunctionFor(id);
  DuckdbRuntime rt(wc, EntryFor(id), 10.0);
  rt.EnableMutationCapture();
  const double rss_before = SettledRssMb(qc);
  const auto b0 = Clock::now();
  rt.Build();
  const double build_ms = Us(b0, Clock::now()) / 1000;
  const double rss_peak = RssMb().second, rss_after = SettledRssMb(qc);
  core::ResetAuxExecuteCounts();
  std::string per = "[";
  double log_sum = 0;
  int n = 0, mismatches = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const auto &c = cases[i];
    auto base = RunBaseline(qc, glue->query_sql, c.base_args); // warm, and the reference answer
    auto acc = RunAccel(qc, fn, c.accel_args);
    const auto br = Canon(*base), ar = Canon(*acc);
    auto bs = br, as = ar;
    std::sort(bs.begin(), bs.end());
    std::sort(as.begin(), as.end());
    const bool match = bs == as;
    mismatches += match ? 0 : 1;
    std::vector<double> tb, ta;
    for (int r = 0; r < reps; ++r) {
      auto t0 = Clock::now();
      RunBaseline(qc, glue->query_sql, c.base_args);
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
  std::cout << "{\"mode\":\"read\",\"accel\":" << Json(id) << ",\"function\":" << Json(fn) << ",\"store\":" << Json(rt.UsesVerusStore() ? "verus" : "cpp")
            << ",\"build_ms\":" << build_ms << ",\"state_mb\":" << rss_after - rss_before
            << ",\"rss_mb_settled_before\":" << rss_before << ",\"rss_mb_settled_after\":" << rss_after
            << ",\"rss_mb_peak_build\":" << rss_peak << ",\"speedup_geomean\":" << (n ? std::exp(log_sum / n) : 0)
            << ",\"mismatches\":" << mismatches << ",\"hits\":" << counts.hit << ",\"fallbacks\":" << counts.fallback
            << ",\"cases\":" << per << "]}" << std::endl;
  return 0;
}

int Concurrent(const std::string &path, const std::string &id, const std::string &base_kit,
               const std::vector<Case> &cases, const std::string &phase, int readers, double seconds,
               double writes_per_sec, uint64_t seed, double warmup) {
  Require(phase == "accel" || phase == "base", "phase must be accel or base");
  const bool accel = phase == "accel";
  const auto fn = accel ? FunctionFor(id) : std::string();
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  accelerator_codegen::RegisterAcceleratorFunctions(qc);
  const auto *glue = kit::Find(base_kit);
  Require(glue != nullptr, "no baseline kit " + base_kit);
  // answers before the run (to count the cases whose answer the writes change)
  std::vector<std::vector<std::string>> before;
  for (const auto &c : cases)
    before.push_back(Canon(*RunBaseline(qc, glue->query_sql, c.base_args)));
  const std::size_t max_writes = static_cast<std::size_t>(writes_per_sec * (seconds + warmup) * 1.2) + 32;
  Writes writes(qc, max_writes, seed, RfBatchOrders());
  std::unique_ptr<DuckdbRuntime> rt;
  double build_ms = 0;
  if (accel) {
    rt = std::make_unique<DuckdbRuntime>(wc, EntryFor(id), 10.0);
    rt->EnableMutationCapture();
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
          if (accel)
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
  std::map<std::string, uint64_t> maintenance, write_kinds, maintenance_by_kind;
  std::string slowest; // the slowest commit's write kind and maintenance action
  double slowest_us = 0;
  uint64_t write_errors = 0, measured_writes = 0, total_writes = 0;
  double max_commit_us = 0;
  std::string write_error_example;
  if (writes_per_sec > 0) {
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / writes_per_sec));
    auto next = start;
    while (Clock::now() < deadline) {
      std::this_thread::sleep_until(next);
      next = std::max(next + interval, Clock::now()); // open loop, but no catch-up burst after a slow commit
      if (Clock::now() >= deadline)
        break;
      const auto [kind, sql] = writes.Next();
      const auto t0 = Clock::now();
      core::ClearMaintenanceDecisionLog();
      try {
        if (accel) {
          rt->ApplyWrite(sql);
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
  std::cout << "{\"mode\":\"concurrent\",\"phase\":" << Json(phase) << ",\"accel\":" << Json(id)
            << ",\"store\":" << Json(!accel ? "none" : rt->UsesVerusStore() ? "verus" : "cpp")
            << ",\"readers\":" << readers << ",\"seconds\":" << seconds << ",\"build_ms\":" << build_ms
            << ",\"reads\":" << latencies.size() << ",\"qps\":" << latencies.size() / seconds
            << ",\"read_p50_us\":" << Percentile(latencies, 0.5) << ",\"read_p95_us\":" << Percentile(latencies, 0.95)
            << ",\"read_p99_us\":" << Percentile(latencies, 0.99) << ",\"read_errors\":" << read_errors
            << ",\"read_error_example\":" << Json(read_error_example.substr(0, 300))
            << ",\"rf_batch_orders\":" << RfBatchOrders() << ",\"rf_only\":" << (RfOnly() ? "true" : "false") << ",\"dim_after_rf\":" << (DimAfterRf() ? "true" : "false") << ",\"writes_per_sec_target\":" << writes_per_sec << ",\"writes\":" << measured_writes
            << ",\"writes_per_sec\":" << measured_writes / seconds << ",\"commit_p50_us\":" << Percentile(commit_us, 0.5)
            << ",\"commit_p99_us\":" << Percentile(commit_us, 0.99)
            << ",\"writes_total\":" << total_writes << ",\"max_commit_us\":" << max_commit_us << ",\"write_errors\":" << write_errors
            << ",\"write_error_example\":" << Json(write_error_example.substr(0, 300)) << ",\"write_kinds\":" << kjson
            << "},\"maintenance\":" << mjson << "},\"maintenance_by_kind\":" << bjson
            << "},\"slowest_commit\":" << Json(slowest) << ",\"invalidating_writes\":" << invalidating
            << ",\"answers_changed\":" << changed << ",\"cases\":" << cases.size() << ",\"hits\":" << counts.hit
            << ",\"fallbacks\":" << counts.fallback << "}" << std::endl;
  return 0;
}

struct MultiSpec {
  std::string id, base_kit, fn;
  const kit::Glue *glue = nullptr;
  std::vector<Case> cases;                   // the current repetition's
  std::vector<std::vector<Case>> rep_cases;  // one parameter draw per repetition
};

std::string KindsJson(const std::map<std::string, uint64_t> &m) {
  std::string j = "{";
  for (const auto &[k, v] : m)
    j += (j.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
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
  Require(phase == "accel" || phase == "base", "phase must be accel or base");
  Require(reader_mode == "rotate" || reader_mode == "pinned", "reader mode must be rotate or pinned");
  const bool pinned = reader_mode == "pinned";
  const bool accel = phase == "accel";
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
      m.glue = kit::Find(m.base_kit);
      Require(m.glue != nullptr, "no baseline kit " + m.base_kit);
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
  std::vector<std::unique_ptr<DuckdbRuntime>> runtimes;
  std::vector<DuckdbRuntime *> live;
  std::string builds = "{", build_rss = "{";
  double build_ms_total = 0;
  if (accel) {
    for (auto &m : specs) {
      m.fn = FunctionFor(m.id);
      runtimes.push_back(std::make_unique<DuckdbRuntime>(wc, EntryFor(m.id), 10.0));
      runtimes.back()->EnableMutationCapture();
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
      before[q].push_back(Canon(*RunBaseline(qc, specs[q].glue->query_sql, specs[q].cases[0].base_args)));
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
          if (accel)
            RunAccel(c, m.fn, cs.accel_args);
          else
            RunBaseline(c, m.glue->query_sql, cs.base_args);
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
      changed += Canon(*RunBaseline(qc, specs[q].glue->query_sql, specs[q].cases[i].base_args)) != before[q][i] ? 1 : 0;
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
            << ",\"rf_batch_orders\":" << RfBatchOrders() << ",\"rf_only\":" << (RfOnly() ? "true" : "false") << ",\"dim_after_rf\":" << (DimAfterRf() ? "true" : "false") << ",\"writes_per_sec_target\":" << writes_per_sec << ",\"writes\":" << measured_writes
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

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 6, "usage: aux_compare read|concurrent|multi <db> ...");
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
    const auto cases = ReadCases(argv[5]);
    if (mode == "read")
      return Read(argv[2], argv[3], argv[4], cases, argc > 6 ? std::stoi(argv[6]) : 5);
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
