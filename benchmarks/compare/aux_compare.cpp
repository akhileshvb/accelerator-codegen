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
//   maximum), and every fifth write a point write rotating over supplier,
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
#include <fstream>
#include <iostream>
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

// A deterministic TPC-H write stream (see the header).
class Writes {
public:
  Writes(duckdb::Connection &c, std::size_t max_writes, uint64_t seed) : rng(seed) {
    const auto n = std::to_string(max_writes + 16);
    low_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " + n);
    template_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey DESC LIMIT 1000");
    mid_orders = Keys(c, "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " + n +
                             " OFFSET (SELECT count(*) // 2 FROM orders)");
    const auto dims = std::to_string(max_writes / 5 + 16);
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
  }

  std::pair<std::string, std::string> Next() {
    const auto i = seq++;
    const auto s = [](int64_t v) { return std::to_string(v); };
    if (i % 5 != 4) { // refresh: RF2 / RF1 alternating
      if (rf++ % 2 == 0) {
        const auto k = s(Take(low_orders, low_at));
        return {"rf2", "DELETE FROM lineitem WHERE l_orderkey = " + k + "; DELETE FROM orders WHERE o_orderkey = " + k};
      }
      const auto from = s(template_orders[rng() % template_orders.size()]);
      const auto k = s(next_order++);
      return {"rf1", "INSERT INTO orders SELECT * REPLACE (" + k + " AS o_orderkey) FROM orders WHERE o_orderkey = " +
                         from + "; INSERT INTO lineitem SELECT * REPLACE (" + k +
                         " AS l_orderkey) FROM lineitem WHERE l_orderkey = " + from};
    }
    static const char *segments[] = {"BUILDING", "AUTOMOBILE", "MACHINERY", "HOUSEHOLD", "FURNITURE"};
    static const char *priorities[] = {"1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"};
    switch (dim++ % 9) {
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

private:
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
  std::vector<int64_t> low_orders, template_orders, mid_orders, low_parts, template_parts, low_supps, template_supps,
      mid_supps, custs, mid_parts;
  std::size_t low_at = 0, mid_order_at = 0, low_part_at = 0, low_supp_at = 0, mid_supp_at = 0, cust_at = 0,
              mid_part_at = 0;
  int64_t next_order = 0, next_part = 0, next_supp = 0;
  uint64_t seq = 0, rf = 0, dim = 0;
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
  const auto b0 = Clock::now();
  rt.Build();
  const double build_ms = Us(b0, Clock::now()) / 1000;
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
            << ",\"build_ms\":" << build_ms << ",\"speedup_geomean\":" << (n ? std::exp(log_sum / n) : 0)
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
  Writes writes(qc, max_writes, seed);
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
  std::map<std::string, uint64_t> maintenance, write_kinds;
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
        maintenance[d ? accelerator_codegen::aux_state::maintenance::MaintenanceActionName(d->staged_action) : "none"]++;
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
  std::string mjson = "{", kjson = "{";
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
            << ",\"writes_per_sec_target\":" << writes_per_sec << ",\"writes\":" << measured_writes
            << ",\"writes_per_sec\":" << measured_writes / seconds << ",\"commit_p50_us\":" << Percentile(commit_us, 0.5)
            << ",\"commit_p99_us\":" << Percentile(commit_us, 0.99)
            << ",\"writes_total\":" << total_writes << ",\"max_commit_us\":" << max_commit_us << ",\"write_errors\":" << write_errors
            << ",\"write_error_example\":" << Json(write_error_example.substr(0, 300)) << ",\"write_kinds\":" << kjson
            << "},\"maintenance\":" << mjson << "},\"invalidating_writes\":" << invalidating
            << ",\"answers_changed\":" << changed << ",\"cases\":" << cases.size() << ",\"hits\":" << counts.hit
            << ",\"fallbacks\":" << counts.fallback << "}" << std::endl;
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 6, "usage: aux_compare read|concurrent <db> <accel_id> <baseline_kit> <cases.tsv> ...");
    const std::string mode = argv[1];
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
