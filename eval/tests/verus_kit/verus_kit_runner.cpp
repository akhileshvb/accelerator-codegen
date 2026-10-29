// Generic test and benchmark runner for Verus accelerators registered through
// the host kit (verus_kit.hpp), on DuckDB.  Query-agnostic: everything about
// the query comes from its registered glue and a test-spec directory.
//
//   verus_kit_runner differential <db> <accel_id> <spec_dir> [every=10]
//   verus_kit_runner snapshots    <db> <accel_id> <spec_dir> [writes=200] [readers=4] [seed=1]
//   verus_kit_runner bench        <db> <accel_id> <spec_dir> [reps=5] [mixes=0.01,0.05,0.1] [ops=200]
//   verus_kit_runner sql          <db> <query> [max_rows=50]   (read-only exploration)
//   verus_kit_runner exec         <db> <statements>              (changes the database: its keys)
//   verus_kit_runner bench_writes <db> <spec_dir> [rounds=3]     (the bench's writes apply on its database)
//   verus_kit_runner record       <db> <accel_id> <spec_dir> <probes_file> <out_dir>
//     (glue contract check: with ACCEL_VERUS_RECORD_DIR=<out_dir>, the C ABI
//     logs what the glue passes; this dumps each probe query (a line
//     "name<TAB>sql") before the build and before / after each transaction,
//     and marks the build and each transaction in <out_dir>/records.jsonl)
//   verus_kit_runner validate     <db> <spec_dir> <query_sql_file>
//     (no accelerator: setup runs, every parameter tuple evaluates and runs
//     the query, every transaction runs and is rolled back)
//
// Spec directory: the test suite of eval/harness/suite.hpp (setup.sql, params.sql, txns/, bench_txns/;
// bench_txns/ are the writes of `bench`, else txns/).
//
// differential: after the build and after every transaction (captured by the
//   engine), the accelerator (accel_<id>(...)) is compared with the query
//   itself at the same snapshot: all parameter tuples every `every`
//   transactions and at the end, two random ones otherwise.
// snapshots: readers in other connections run, inside one transaction, the
//   accelerator, a pause, the query, a pause, the accelerator, while a writer
//   commits; all three must agree.
// bench: per parameter tuple, median latency of the query vs the
//   accelerator; then mixes of queries and writes (fraction of writes per
//   operation), base (plain writes + query) vs accelerated.
// state: the build, then the first N transactions of the spec; reports the store's measured heap
//   after each (state_bytes_*; every mode's JSON also has state_bytes_after_build, build_peak_bytes,
//   state_bytes, peak_bytes: the counting allocator of substrate/verus/heap, 0 for a non-Verus store).
//   The writes come from VERUS_KIT_WRITE_MODE:
//     txns    the spec's bench_txns (default)
//     rf      TPC-H refresh functions in mini-chunks (VERUS_KIT_RF_CHUNK orders,
//             default 1), alternating RF2 (delete the next smallest orders with
//             their lineitems) and RF1 (insert as many orders, cloned with their
//             lineitems from the largest ones, under fresh keys above the maximum)
//     rf_dim  the same shape for the dimension tables, cycling: delete parts with
//             their partsupp rows, insert cloned parts with theirs, then the same
//             for suppliers
//     auto    rf when the glue reads orders or lineitem, else rf_dim
//   In the rf modes the plain pass runs each write and rolls it back, and the
//   accelerated pass replays the same writes and commits them: the accelerator's
//   capture sees only its own connection, so it stays in sync with the database.
//   (The plain side then pays no commit: its write cost is a slight underestimate.)
// Parameter tuples drawn by the pipeline from the query's domain, fresh on every run:
//   VERUS_KIT_PARAMS_FILE=<file>        these tuples replace the spec's (bench, profiles)
//   VERUS_KIT_EXTRA_PARAMS_FILE=<file>  these are checked besides the spec's (differential, snapshots)
// (one tuple per line, as in params.sql; the spec format is eval/harness/suite.hpp)
// VERUS_KIT_BOUNDS_SQL=<file>: the query's bounds query (eval/harness/float_bounds.hpp): its DOUBLE
//   SUM / AVG are rounded by the engine, so the accelerator's answer is accepted iff it is admissible
//   (within the spec's rounding bound of the exact values), not only if it equals the reference.
// The store is the C++ one unless ACCEL_AUX_STORE=verus.  Output: one JSON
// object on stdout.  The database file is modified; pass a copy.
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/verus/kit/verus_kit.hpp"
#include "catalog/query_catalog.hpp"
#include "eval/harness/suite.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using namespace accelerator_codegen::suite;
namespace engine = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace kit = accelerator_codegen::verus_kit;
using engine::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

// An accelerator call that answered with the SQL fallback: why (the host could not convert an
// argument, or the kernel returned None).
Rows RunAccelRecorded(Tally &tally, duckdb::Connection &c, const std::string &id, const std::string &where,
                      const std::string &tuple) {
  kit::ClearArgFailure();
  const auto before = core::ReadAuxExecuteCounts().fallback;
  Rows r = RunAccel(c, "accel_" + id, tuple);
  if (core::ReadAuxExecuteCounts().fallback > before) {
    const auto arg = kit::LastArgFailure();
    const char *kind = arg.what.empty() ? "kernel_none" : arg.outside_type ? "param_outside_type" : "host_conversion";
    tally.NoteFallback(where, tuple, kind,
                       arg.what.empty() ? "kernel returned None" : "host arg conversion failed: " + arg.what);
  }
  return r;
}

// Timed runs (bench): the same statements, the result materialized but not rendered as
// text -- per-cell Value::ToString is this harness's comparison format, not query work, and
// on a large answer it dominates both sides alike.
std::size_t TimeQuery(duckdb::Connection &c, const std::string &query_sql, const std::string &tuple) {
  duckdb::vector<duckdb::Value> values;
  if (!tuple.empty()) {
    auto t = Q(c, "SELECT " + tuple);
    for (duckdb::idx_t i = 0; i < t->ColumnCount(); ++i)
      values.push_back(t->GetValue(i, 0));
  }
  auto stmt = c.Prepare(query_sql);
  Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed");
  auto r = stmt->Execute(values, false);
  Require(r && !r->HasError(), r ? r->GetError() : "query failed");
  return dynamic_cast<duckdb::MaterializedQueryResult &>(*r).RowCount();
}

std::size_t TimeAccel(duckdb::Connection &c, const std::string &id, const std::string &tuple) {
  auto r = Q(c, "SELECT * FROM accel_" + id + "(" + tuple + ")");
  return dynamic_cast<duckdb::MaterializedQueryResult &>(*r).RowCount();
}

// Tuples the Verus pipeline draws from the query's domain, fresh on every run: VERUS_KIT_PARAMS_FILE
// replaces the spec's (bench, profiles), VERUS_KIT_EXTRA_PARAMS_FILE adds to them (differential, snapshots).
Spec LoadKitSpec(const fs::path &dir) { return LoadSpec(dir, "VERUS_KIT_PARAMS_FILE", "VERUS_KIT_EXTRA_PARAMS_FILE"); }

struct Setup {
  const kit::Glue *glue;
  const engine::SharedQueryCatalogEntry *entry;
  Ordering ordering;
};

// with_setup=false (bench): measure the real data, without the tests' setup
// rows (edge cases for correctness, not a workload).
Setup Prepare(duckdb::Connection &setup, const std::string &id, const Spec &spec, bool with_setup = true) {
  Setup s{kit::Find(id), engine::VerusKitCatalogEntry(id)};
  Require(s.glue && s.entry, "no Verus accelerator registered as " + id);
  if (const auto schema = SchemaSql("ACCEL_VERUS_SCHEMA_SQL"); !schema.empty())
    Q(setup, schema);
  if (with_setup && !spec.setup.empty())
    Q(setup, spec.setup);
  accelerator_codegen::RegisterAcceleratorFunctions(setup);
  s.ordering = MakeOrdering(setup, s.glue->query_sql, spec.params);
  if (const char *bounds = std::getenv("VERUS_KIT_BOUNDS_SQL"); bounds && *bounds) {
    s.ordering.bounds_sql = ReadFile(bounds); // rounded DOUBLE aggregates: any admissible answer
    s.ordering.query_sql = s.glue->query_sql;
  }
  return s;
}

// Why queries fell back ("causes": runtime, store and kit reasons) and what the maintenance
// did that can explain it ("events"): cause name -> count.
std::string CauseFields() {
  std::string causes, events;
  for (const auto &[name, n] : core::ReadAuxFallbackCauses()) {
    auto &out = name.rfind("event:", 0) == 0 ? events : causes;
    out += (out.empty() ? "" : ",") + Json(name) + ":" + std::to_string(n);
  }
  return "\"causes\":{" + causes + "},\"events\":{" + events + "}";
}

// The Verus store's measured heap (counting allocator, substrate/verus/heap) at the end of the
// build: the state of one version plus whatever the build left; its peak during the build.  The
// peak is then reset, so the end-of-run peak covers the writes and reads alone.
struct BuildHeap {
  std::size_t bytes = 0, peak = 0;
};

BuildHeap BuildHeapAfter(const DuckdbRuntime &rt) {
  BuildHeap h{rt.StateHeapBytes(), rt.StateHeapPeakBytes()};
  rt.ResetHeapPeak();
  return h;
}

std::string StoreFields(DuckdbRuntime &rt, const BuildHeap &build) {
  const auto counts = core::ReadAuxExecuteCounts();
  return "\"store\":" + Json(rt.UsesVerusStore() ? "verus" : "cpp") + ",\"versions\":" +
         std::to_string(rt.StateVersionCount()) + ",\"state_bytes_after_build\":" + std::to_string(build.bytes) +
         ",\"build_peak_bytes\":" + std::to_string(build.peak) + ",\"state_bytes\":" +
         std::to_string(rt.StateHeapBytes()) + ",\"peak_bytes\":" + std::to_string(rt.StateHeapPeakBytes()) +
         ",\"stale_installs\":" + std::to_string(rt.StaleInstallCount()) +
         ",\"executes\":{\"hit\":" + std::to_string(counts.hit) + ",\"fallback\":" + std::to_string(counts.fallback) +
         "},\"reader_fallbacks\":" + std::to_string(rt.reader_fallbacks.load()) + "," + CauseFields();
}

int Differential(const std::string &path, const std::string &id, const Spec &spec, int every) {
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  const auto s = Prepare(qc, id, spec);
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  core::ResetAuxExecuteCounts();
  const auto build_start = Clock::now();
  rt.Build();
  const auto build_heap = BuildHeapAfter(rt);
  const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
  Tally tally;
  const auto run = RunDifferentialLoop(
      tally, spec, s.glue->query_sql, s.ordering, qc, every, 7,
      [&](const std::string &where, const std::string &t) { return RunAccelRecorded(tally, qc, id, where, t); },
      [&](const std::string &sql) { rt.ApplyWrite(sql); });
  std::string acts = "{";
  for (const auto &[k, v] : run.actions)
    acts += (acts.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
  std::cout << "{\"mode\":\"differential\",\"txns\":" << spec.txns.size() << ",\"write_errors\":" << run.write_errors
            << ",\"write_error_examples\":" << run.write_error_examples << "]" << ",\"build_ms\":" << build_ms << "," << tally.Fields() << ",\"staged_actions\":" << acts
            << "},\"staged_action_names\":{\"1\":\"ApplyDelta\",\"3\":\"RebuildAll\"}," << StoreFields(rt, build_heap) << "}"
            << std::endl;
  // skipped comparisons (reference errors) must stay a minority: a run that compared
  // little or nothing proves little
  const bool compared = tally.checks > 0 && tally.reference_errors <= tally.checks;
  return compared && run.write_errors == 0 && tally.mismatches + tally.order_mismatches == 0 && rt.StaleInstallCount() == 0 ? 0 : 1;
}

int Snapshots(const std::string &path, const std::string &id, const Spec &spec, int writes, int readers,
              uint64_t seed) {
  Require(!spec.txns.empty(), "snapshots needs txns/");
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), setup(db);
  const auto s = Prepare(setup, id, spec);
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  rt.Build();
  const auto build_heap = BuildHeapAfter(rt);
  std::atomic<bool> done{false};
  std::atomic<int> checks{0}, mismatches{0}, failures{0}, reference_errors{0}, oracle_retries{0};
  std::mutex ex_mutex;
  std::vector<std::string> examples;
  std::vector<std::thread> threads;
  for (int t = 0; t < readers; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(seed * 1000003 + static_cast<uint64_t>(t));
      try {
        duckdb::Connection rc(db);
        while (!done.load()) {
          const auto &p = spec.params[rng() % spec.params.size()];
          Q(rc, "BEGIN TRANSACTION");
          const auto a = RunAccel(rc, "accel_" + id, p);
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          Rows b;
          try {
            b = RunQuery(rc, s.glue->query_sql, p);
          } catch (const std::exception &) {
            // DuckDB itself cannot evaluate the reference here (e.g. a decimal overflow on extreme
            // test values): nothing to compare; skipped and counted, as in differential
            Q(rc, "ROLLBACK");
            ++reference_errors;
            continue;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
          const auto c = RunAccel(rc, "accel_" + id, p);
          const auto full = [&] { return RunQuery(rc, s.ordering.unlimited_sql, p); }; // same snapshot
          const auto probe = [&] { return RunProbe(rc, s.ordering.bounds_sql, p); };
          const auto disagree = [&](const Rows &reference) {
            return CompareOrdered(s.ordering, a, reference, full, probe) != 0 || CompareOrdered(s.ordering, c, reference, full, probe) != 0;
          };
          bool wrong = disagree(b);
          // DuckDB (1.5.4 and 2.0.0 alike) can show a reader the rows of a bulk INSERT ... SELECT while it commits, to
          // transactions that started before it: the reference itself is torn, the accelerator answers at the snapshot.
          // A disagreement counts when it survives a fresh evaluation of the reference (the answers do not change).
          for (int again = 0; wrong && again < 3; ++again) {
            try {
              const auto b2 = RunQuery(rc, s.glue->query_sql, p);
              ++oracle_retries;
              wrong = disagree(b2);
            } catch (const std::exception &) {
              break;
            }
          }
          Q(rc, "COMMIT");
          ++checks;
          if (wrong) {
            ++mismatches;
            std::lock_guard<std::mutex> lock(ex_mutex);
            if (examples.size() < 5)
              examples.push_back("{\"reader\":" + std::to_string(t) + ",\"params\":" + Json(p) + ",\"accel\":" +
                                 RowsJson(a) + ",\"query\":" + RowsJson(b) + ",\"accel_again\":" + RowsJson(c) + "}");
          }
        }
      } catch (const std::exception &e) {
        if (++failures <= 3)
          std::cerr << "reader " << t << ": " << e.what() << "\n";
      }
    });
  }
  std::mt19937_64 rng(seed);
  int write_errors = 0;
  std::string write_error_example;
  for (int i = 0; i < writes; ++i) {
    try {
      rt.ApplyWrite(spec.txns[rng() % spec.txns.size()]);
    } catch (const std::exception &e) {
      // random order: a transaction can legitimately fail here (its rows gone)
      if (++write_errors == 1)
        write_error_example = std::string(e.what()).substr(0, 600);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
  }
  done = true;
  for (auto &th : threads)
    th.join();
  std::string ex = "[";
  for (std::size_t i = 0; i < examples.size(); ++i)
    ex += (i ? "," : "") + examples[i];
  std::cout << "{\"mode\":\"snapshots\",\"writes\":" << writes << ",\"write_errors\":" << write_errors
            << ",\"write_error_example\":" << Json(write_error_example) << ",\"readers\":" << readers << ",\"checks\":" << checks << ",\"reference_errors\":" << reference_errors
            << ",\"mismatches\":" << mismatches << ",\"oracle_retries\":" << oracle_retries << ",\"reader_failures\":" << failures << ",\"examples\":" << ex << "]," << StoreFields(rt, build_heap) << "}"
            << std::endl;
  // some writes must commit, or the readers saw no concurrent maintenance
  return mismatches == 0 && failures == 0 && write_errors < writes && checks > 0 && reference_errors <= checks &&
                 rt.StaleInstallCount() == 0
             ? 0
             : 1;
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
}

// VERUS_KIT_ACCEL_ONLY=1: only the accelerator's side runs (for profiling it).
// Profiling hooks: VERUS_KIT_PERF_CTL / _ACK are perf's control FIFOs
// (perf record -D -1 --control fifo:ctl,ack): profiling starts after the build,
// so a profile shows the steady state (reads, write maintenance), not the
// one-time build.
void PerfEnable() {
  const char *ctl = std::getenv("VERUS_KIT_PERF_CTL");
  if (!ctl || !*ctl)
    return;
  if (FILE *f = std::fopen(ctl, "w")) {
    std::fputs("enable\n", f);
    std::fclose(f);
  }
  if (const char *ack = std::getenv("VERUS_KIT_PERF_ACK"); ack && *ack) {
    if (FILE *f = std::fopen(ack, "r")) {
      char buf[16];
      (void)std::fgets(buf, sizeof buf, f);
      std::fclose(f);
    }
  }
}

std::vector<int64_t> Keys(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), r ? r->GetError() : sql);
  std::vector<int64_t> keys;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
    keys.push_back(r->GetValue(0, i).GetValue<int64_t>());
  return keys;
}

// Deterministic refresh-style writes (see VERUS_KIT_WRITE_MODE above); a copy
// replays the same sequence.
struct RfWrites {
  struct Entity {
    std::string table, key, partsupp_key; // partsupp_key empty: orders (with lineitem)
    std::vector<int64_t> victims, templates;
    int64_t next_key = 0;
    std::size_t victim_at = 0, template_at = 0;
  };
  std::vector<Entity> entities; // rf: orders; rf_dim: part, supplier
  int chunk = 1;
  long seq = 0;

  static Entity Load(duckdb::Connection &c, const std::string &table, const std::string &key,
                     const std::string &partsupp_key, std::size_t victims) {
    Entity e{table, key, partsupp_key, {}, {}, 0, 0, 0};
    e.victims = Keys(c, "SELECT " + key + " FROM " + table + " ORDER BY " + key + " LIMIT " + std::to_string(victims));
    e.templates = Keys(c, "SELECT " + key + " FROM " + table + " ORDER BY " + key + " DESC LIMIT 1000");
    e.next_key = Keys(c, "SELECT max(" + key + ") FROM " + table)[0] + 1;
    Require(!e.templates.empty(), "rf: empty table " + table);
    return e;
  }

  std::string Next() {
    const long op = seq++;
    auto &e = entities[(op / 2) % entities.size()];
    std::ostringstream sql;
    if (op % 2 == 0) { // delete the next victims
      Require(e.victim_at + chunk <= e.victims.size(), "rf: out of " + e.table + " keys to delete");
      std::string in = "(";
      for (int j = 0; j < chunk; ++j)
        in += (j ? "," : "") + std::to_string(e.victims[e.victim_at++]);
      in += ")";
      if (e.partsupp_key.empty())
        sql << "DELETE FROM lineitem WHERE l_orderkey IN " << in << "; ";
      else
        sql << "DELETE FROM partsupp WHERE " << e.partsupp_key << " IN " << in << "; ";
      sql << "DELETE FROM " << e.table << " WHERE " << e.key << " IN " << in;
    } else { // insert clones under fresh keys
      for (int j = 0; j < chunk; ++j) {
        const auto from = std::to_string(e.templates[e.template_at++ % e.templates.size()]);
        const auto key = std::to_string(e.next_key++);
        sql << (j ? "; " : "") << "INSERT INTO " << e.table << " SELECT * REPLACE (" << key << " AS " << e.key
            << ") FROM " << e.table << " WHERE " << e.key << " = " << from << "; ";
        if (e.partsupp_key.empty())
          sql << "INSERT INTO lineitem SELECT * REPLACE (" << key << " AS l_orderkey) FROM lineitem WHERE l_orderkey = "
              << from;
        else
          sql << "INSERT INTO partsupp SELECT * REPLACE (" << key << " AS " << e.partsupp_key
              << ") FROM partsupp WHERE " << e.partsupp_key << " = " << from;
      }
    }
    return sql.str();
  }
};

std::string WriteMode(const kit::Glue &glue) {
  const char *env = std::getenv("VERUS_KIT_WRITE_MODE");
  std::string mode = env && *env ? env : "txns";
  if (mode == "auto") {
    mode = "rf_dim";
    for (const auto &t : glue.tables)
      if (std::string(t.name) == "orders" || std::string(t.name) == "lineitem")
        mode = "rf";
  }
  Require(mode == "txns" || mode == "rf" || mode == "rf_dim",
          "VERUS_KIT_WRITE_MODE must be txns, rf, rf_dim or auto");
  return mode;
}

int Bench(const std::string &path, const std::string &id, const Spec &spec, int reps, std::vector<double> mixes,
          int ops) {
  // VERUS_KIT_BENCH_PHASE=reads|writes: only that part (for per-axis profiles)
  const char *phase_env = std::getenv("VERUS_KIT_BENCH_PHASE");
  const std::string phase = phase_env ? phase_env : "";
  if (phase == "reads")
    mixes.clear();
  const bool accel_only = std::getenv("VERUS_KIT_ACCEL_ONLY") != nullptr;
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db), plain(db);
  const auto s = Prepare(qc, id, spec, /*with_setup=*/false);
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  const auto build_start = Clock::now();
  rt.Build();
  const auto build_heap = BuildHeapAfter(rt);
  const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
  // a timed-out run says how far it got (the plain query can take seconds at scale)
  std::cout << "bench: built build_ms=" << build_ms << " state_bytes=" << build_heap.bytes << " build_peak_bytes=" << build_heap.peak << std::endl;
  // the parameter tuples the plain query can evaluate on this database (the tests' extreme tuples can
  // overflow it at scale): those are what is timed, as many as the reads time
  constexpr std::size_t kTimedTuples = 12;
  std::vector<std::string> usable;
  for (const auto &t : spec.params) {
    if (usable.size() == kTimedTuples)
      break;
    try {
      RunQuery(qc, s.glue->query_sql, t);
      usable.push_back(t);
    } catch (const std::exception &) {
    }
  }
  Require(!usable.empty(), "bench: the plain query fails for every parameter tuple on this database");
  const std::string write_mode = WriteMode(*s.glue);
  const char *chunk_env = std::getenv("VERUS_KIT_RF_CHUNK");
  const int rf_chunk = chunk_env && *chunk_env ? std::max(1, std::atoi(chunk_env)) : 1;
  std::optional<RfWrites> rf;
  if (write_mode != "txns") {
    rf.emplace();
    rf->chunk = rf_chunk;
    const std::size_t victims = static_cast<std::size_t>(ops) * std::max<std::size_t>(mixes.size(), 1) * rf_chunk + 16;
    if (write_mode == "rf")
      rf->entities.push_back(RfWrites::Load(qc, "orders", "o_orderkey", "", victims));
    else {
      rf->entities.push_back(RfWrites::Load(qc, "part", "p_partkey", "ps_partkey", victims));
      rf->entities.push_back(RfWrites::Load(qc, "supplier", "s_suppkey", "ps_suppkey", victims));
    }
  }
  const bool have_writes = rf.has_value() || !spec.bench_txns.empty();
  PerfEnable();
  auto us = [](Clock::time_point a) { return std::chrono::duration<double, std::micro>(Clock::now() - a).count(); };
  // read-only: per tuple, medians of the query and of the accelerator
  std::string per = "[";
  double log_sum = 0;
  int n = 0;
  std::size_t tuples = phase == "writes" ? 0 : usable.size();
  for (std::size_t i = 0; i < tuples; ++i) {
    const auto &t = usable[i];
    std::vector<double> q, a;
    TimeAccel(qc, id, t); // warm
    for (int r = 0; r < reps; ++r) {
      auto t0 = Clock::now();
      if (!accel_only)
        TimeQuery(qc, s.glue->query_sql, t);
      q.push_back(us(t0));
      t0 = Clock::now();
      TimeAccel(qc, id, t);
      a.push_back(us(t0));
    }
    const double mq = Median(q), ma = Median(a);
    log_sum += std::log(mq / std::max(ma, 1.0));
    ++n;
    per += (i ? "," : "") + std::string("{\"params\":") + Json(t) + ",\"query_us\":" + std::to_string(mq) +
           ",\"accel_us\":" + std::to_string(ma) + "}";
  }
  per += "]";
  const double read_speedup = n ? std::exp(log_sum / n) : 0.0;
  // mixes: ops operations, each a write with probability f, else a query
  std::string mix_json = "[";
  double maint_us_sum = 0, plain_write_us_sum = 0;
  int maint_n = 0, plain_write_n = 0, write_errors = 0; // failed writes time nothing: reported
  for (std::size_t m = 0; m < mixes.size(); ++m) {
    const double f = mixes[m];
    double base_us = 0, accel_us = 0;
    std::optional<RfWrites> replay = rf; // the plain pass replays the writes the accelerated pass commits
    for (int pass = accel_only ? 1 : 0; pass < 2; ++pass) {
      std::mt19937_64 rng(1234 + m);
      const auto t0 = Clock::now();
      for (int i = 0; i < ops; ++i) {
        const bool write = have_writes && std::uniform_real_distribution<double>(0, 1)(rng) < f;
        if (write) {
          const std::string sql = rf ? (pass == 0 ? replay : rf)->Next()
                                     : spec.bench_txns[rng() % spec.bench_txns.size()];
          if (pass == 0) {
            const auto w0 = Clock::now();
            auto r = plain.Query("BEGIN; " + sql + (rf ? "; ROLLBACK;" : "; COMMIT;"));
            if (r && r->HasError()) {
              plain.Query("ROLLBACK");
              ++write_errors;
            }
            plain_write_us_sum += us(w0);
            ++plain_write_n;
          } else {
            const auto w0 = Clock::now();
            try {
              rt.ApplyWrite(sql);
            } catch (const std::exception &) {
              ++write_errors;
            }
            maint_us_sum += us(w0);
            ++maint_n;
          }
        } else {
          const auto &t = usable[rng() % usable.size()];
          try { // a write can push a value past the plain query's range: skip that read in both passes
            if (pass == 0)
              TimeQuery(qc, s.glue->query_sql, t);
            else
              TimeAccel(qc, id, t);
          } catch (const std::exception &) {
          }
        }
      }
      (pass == 0 ? base_us : accel_us) = us(t0);
    }
    mix_json += (m ? "," : "") + std::string("{\"write_fraction\":") + std::to_string(f) +
                ",\"base_us\":" + std::to_string(base_us) + ",\"accel_us\":" + std::to_string(accel_us) +
                ",\"speedup\":" + std::to_string(base_us / std::max(accel_us, 1.0)) + "}";
  }
  mix_json += "]";
  std::cout << "{\"mode\":\"bench\",\"build_ms\":" << build_ms << ",\"read_speedup\":" << read_speedup
            << ",\"per_params\":" << per << ",\"mixes\":" << mix_json << ",\"write_us_mean\":"
            << (maint_n ? maint_us_sum / maint_n : 0.0) << ",\"plain_write_us_mean\":"
            << (plain_write_n ? plain_write_us_sum / plain_write_n : 0.0) << ",\"write_errors\":" << write_errors
            << ",\"writes\":" << plain_write_n + maint_n << ",\"write_mode\":" << Json(write_mode)
            << ",\"rf_chunk\":" << rf_chunk << "," << StoreFields(rt, build_heap) << "}" << std::endl;
  return 0;
}

// state <db> <accel_id> <spec_dir> [writes]: the measured heap of the Verus store after the build and
// after the first `writes` transactions of the spec (bench_txns, else txns), one by one.
int StateBytes(const std::string &path, const std::string &id, const Spec &spec, std::size_t writes) {
  Require(spec.bench_txns.size() >= writes, "state: the spec has fewer transactions than writes");
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  const auto s = Prepare(qc, id, spec, /*with_setup=*/false);
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  const auto build_start = Clock::now();
  rt.Build();
  const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
  const auto build_heap = BuildHeapAfter(rt);
  std::string series;
  std::size_t errors = 0;
  for (std::size_t i = 0; i < writes; ++i) {
    try {
      rt.ApplyWrite(spec.bench_txns[i]);
    } catch (const std::exception &) {
      ++errors;
    }
    series += (i ? "," : "") + std::to_string(rt.StateHeapBytes());
  }
  std::cout << "{\"mode\":\"state\",\"build_ms\":" << build_ms << ",\"writes\":" << writes
            << ",\"write_errors\":" << errors << ",\"state_bytes_after_each_write\":[" << series << "],"
            << StoreFields(rt, build_heap) << "}" << std::endl;
  return errors == 0 ? 0 : 1;
}

} // namespace

int Record(const std::string &path, const std::string &id, const Spec &spec, const std::string &probes_path,
           const std::string &out_dir) {
  std::vector<std::pair<std::string, std::string>> probes;
  {
    std::istringstream in(ReadFile(probes_path));
    for (std::string line; std::getline(in, line);) {
      const auto tab = line.find('\t');
      if (tab != std::string::npos)
        probes.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
  }
  duckdb::DuckDB db(path);
  duckdb::Connection wc(db), qc(db);
  const auto s = Prepare(qc, id, spec);
  auto mark = [&](const std::string &json) {
    std::ofstream(fs::path(out_dir) / "records.jsonl", std::ios::app) << json << "\n";
  };
  auto dump = [&](const std::string &label) {
    for (const auto &[name, sql] : probes) {
      auto r = Q(qc, sql);
      const auto rows = Collect(*r);
      std::string cols = "[";
      for (duckdb::idx_t i = 0; i < r->ColumnCount(); ++i)
        cols += (i ? "," : "") + Json(r->ColumnName(i));
      std::ofstream(fs::path(out_dir) / ("probe_" + label + "_" + name + ".json"))
          << "{\"columns\":" << cols << "],\"rows\":" << RowsJson(rows, rows.size()) << "}";
    }
  };
  DuckdbRuntime rt(wc, *s.entry, 10.0);
  dump("build");
  mark("{\"marker\":\"build\"}");
  rt.Build();
  int write_errors = 0;
  for (std::size_t i = 0; i < spec.txns.size(); ++i) {
    dump("before_" + std::to_string(i));
    mark("{\"marker\":\"txn\",\"i\":" + std::to_string(i) + "}");
    try {
      rt.ApplyWrite(spec.txns[i]);
    } catch (const std::exception &e) {
      ++write_errors;
      mark("{\"marker\":\"txn_error\",\"i\":" + std::to_string(i) + ",\"error\":" + Json(e.what()) + "}");
    }
    dump("after_" + std::to_string(i));
  }
  std::cout << "{\"mode\":\"record\",\"txns\":" << spec.txns.size() << ",\"write_errors\":" << write_errors << "}"
            << std::endl;
  return 0;
}

// A read-only query (rolled back), for exploring the data.
int Sql(const std::string &path, const std::string &sql, std::size_t max_rows) {
  duckdb::DBConfig config;
  config.options.access_mode = duckdb::AccessMode::READ_ONLY;
  duckdb::DuckDB db(path, &config);
  duckdb::Connection c(db);
  auto r = Q(c, sql);
  const auto rows = Collect(*r);
  std::string cols = "[";
  for (duckdb::idx_t i = 0; i < r->ColumnCount(); ++i)
    cols += (i ? "," : "") + Json(r->ColumnName(i));
  std::cout << "{\"columns\":" << cols << "],\"row_count\":" << rows.size() << ",\"rows\":" << RowsJson(rows, max_rows)
            << "}" << std::endl;
  return 0;
}

// The benchmark's writes on its own database (no accelerator): every bench transaction applied `rounds`
// times, round-robin and committed, as the mixes replay them -- so a write that holds on the test data but
// not on the benchmark's (a key that exists there, a row a subquery no longer finds) fails here, not
// silently inside a measurement.
int BenchWrites(const std::string &path, const Spec &spec, int rounds) {
  duckdb::DuckDB db(path);
  duckdb::Connection c(db);
  if (const auto schema = SchemaSql("ACCEL_VERUS_SCHEMA_SQL"); !schema.empty())
    Q(c, schema);
  std::vector<std::string> errors;
  int failed = 0;
  for (int round = 0; round < rounds; ++round)
    for (std::size_t i = 0; i < spec.bench_txns.size(); ++i) {
      auto r = c.Query("BEGIN; " + spec.bench_txns[i] + "; COMMIT;");
      if (r && !r->HasError())
        continue;
      c.Query("ROLLBACK");
      ++failed;
      if (errors.size() < 5)
        errors.push_back("write " + std::to_string(i + 1) + " of bench_txns, application " + std::to_string(round + 1) +
                         ": " + (r ? r->GetError() : std::string("failed")));
    }
  std::string ej = "[";
  for (std::size_t i = 0; i < errors.size(); ++i)
    ej += (i ? "," : "") + Json(errors[i]);
  std::cout << "{\"mode\":\"bench_writes\",\"writes\":" << spec.bench_txns.size() * rounds << ",\"failed\":" << failed
            << ",\"errors\":" << ej << "]}" << std::endl;
  return failed ? 1 : 0;
}

// Statements that change the database itself (the pipeline aligns a test database's keys with it).
int Exec(const std::string &path, const std::string &sql) {
  duckdb::DuckDB db(path);
  duckdb::Connection c(db);
  Q(c, sql);
  std::cout << "{\"ok\":true}" << std::endl;
  return 0;
}

int main(int argc, char **argv) {
  try {
    if (argc >= 7 && std::string(argv[1]) == "record")
      return Record(argv[2], argv[3], LoadKitSpec(argv[4]), argv[5], argv[6]);
    if (argc >= 5 && std::string(argv[1]) == "validate")
      return Validate(argv[2], LoadKitSpec(argv[3]), ReadFile(argv[4]), SchemaSql("ACCEL_VERUS_SCHEMA_SQL"));
    if (argc >= 4 && std::string(argv[1]) == "sql")
      return Sql(argv[2], argv[3], argc > 4 ? std::stoul(argv[4]) : 50);
    if (argc >= 4 && std::string(argv[1]) == "exec")
      return Exec(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "bench_writes")
      return BenchWrites(argv[2], LoadKitSpec(argv[3]), argc > 4 ? std::stoi(argv[4]) : 3);
    if (argc >= 5 && std::string(argv[1]) == "state")
      return StateBytes(argv[2], argv[3], LoadKitSpec(argv[4]), argc > 5 ? std::stoul(argv[5]) : 0);
    Require(argc >= 5, "usage: verus_kit_runner differential|snapshots|bench|state <db> <accel_id> <spec_dir> [...]");
    const std::string mode = argv[1], db = argv[2], id = argv[3];
    const Spec spec = LoadKitSpec(argv[4]);
    auto arg = [&](int i, const char *def) { return std::string(argc > i ? argv[i] : def); };
    if (mode == "differential")
      return Differential(db, id, spec, std::stoi(arg(5, "10")));
    if (mode == "snapshots")
      return Snapshots(db, id, spec, std::stoi(arg(5, "200")), std::stoi(arg(6, "4")), std::stoull(arg(7, "1")));
    if (mode == "bench") {
      std::vector<double> mixes;
      std::stringstream ss(arg(6, "0.01,0.05,0.1"));
      for (std::string x; std::getline(ss, x, ',');)
        if (!x.empty())
          mixes.push_back(std::stod(x));
      return Bench(db, id, spec, std::stoi(arg(5, "5")), mixes, std::stoi(arg(7, "200")));
    }
    Require(false, "unknown mode " + mode);
  } catch (const std::exception &e) {
    std::cout << "{\"error\":" << Json(e.what()) << "}" << std::endl;
    return 2;
  }
  return 2;
}
