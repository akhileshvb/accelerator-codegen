// Maintenance-action choice under different writes, for one accelerator on
// DuckDB with engine capture.
//
// For each write shape (point to bulk, few to many partitions) and each
// expected number of reads per write, it commits the write and reports the
// action the cost model chose with its predicted cost of every action, and
// what the same request would choose on an engine whose readers can rebuild
// (InvalidateAndRebuildOnRead offered; DuckDB cannot). Then it forces each
// action on the same write and times the commit, to compare the choice with
// the measured cheapest action. Each write is applied as +1 then -1 (same
// shape), so the data does not drift.
//
// (Calibration from real writes: core::CalibrateFromWrites, run by
// calibrate_shared_aux_cost_model with the module's CalibrationWrites.)
//
// Usage: maintenance_choice_demo <tpch.duckdb (copied first)> [query=1]
//        maintenance_choice_demo <tpch.duckdb> 1 phases <base copy.duckdb> <out.csv>
//        maintenance_choice_demo <tpch.duckdb> 1 trace <base copy.duckdb> <out.csv>
//
// Trace mode: 600 queries in four phases (A: a point write every 10 queries,
// B: a bulk write (1 year of rows) every 16, C: a point write before every
// query, D: as A), under the cost model, always ApplyDelta, always
// RebuildAll, and plain DuckDB. Per query and strategy: ms for the write
// before it (if any, with maintenance) plus the query.
//
// Phases mode: the write shapes above in sequence; per shape, 4 writes, each
// followed by 10 accelerator reads, under each strategy (the cost model's
// choice, then each action forced), and on plain DuckDB (a separate copy, no
// accelerator: the same writes, the reference query). Per (phase, strategy):
// mean write ms (statement + commit, with maintenance) and mean read ms.
// Env: ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL (cost model JSON),
// DEMO_WRITE_COLUMN (lineitem column the writes shift; default l_quantity),
// PHASES_WRITES (writes per shape in phases mode; default 4),
// PHASES_SKIP_PARTITION=1 (phases mode without forced RebuildPartition),
// TRACE_SCHEDULE=contrast (trace mode: the phases described at the trace).
#include <algorithm>
#include <tuple>
#include <utility>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "eval/harness/cost_calibration.hpp"
#include "eval/tests/test_support/cross_accel_helpers.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

using accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime;
namespace engine = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace mnt = accelerator_codegen::aux_state::maintenance;
using Clock = std::chrono::steady_clock;

struct Shape {
  const char *name;
  std::string where; // lineitem rows the write touches
};

void Exec(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql + ": " + (r ? r->GetError() : "null"));
  }
}

int64_t Scalar(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql + ": " + (r ? r->GetError() : "null"));
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

// The lineitem column every write shifts by +-1 (DEMO_WRITE_COLUMN, default
// l_quantity: pick one the accelerator depends on).
std::string WriteColumn() {
  const char *col = std::getenv("DEMO_WRITE_COLUMN");
  return col && *col ? col : "l_quantity";
}

std::string UpdateSql(int delta, const std::string &where) {
  const auto col = WriteColumn();
  return "UPDATE lineitem SET " + col + " = " + col + " + (" + std::to_string(delta) + ") WHERE " + where;
}

// One write of `shape` (+delta) as a captured transaction; returns its commit
// time in ms.
double Write(DuckdbRuntime &runtime, duckdb::Connection &c, const Shape &shape, int delta) {
  runtime.BeginWrite();
  Exec(c, UpdateSql(delta, shape.where));
  const auto start = Clock::now();
  runtime.CommitWrite();
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

const char *Name(mnt::MaintenanceAction a) { return mnt::MaintenanceActionName(a); }

// The action the last commit's maintenance took. The decision is logged per
// commit into the runtime's stats (not the global log); a commit that logged
// none never consulted the cost model (a module's small-batch path applies
// deltas directly): "small-batch".
std::string CommitAction(const DuckdbRuntime &runtime) {
  const auto decision = runtime.Stats().last_decision;
  return decision && decision->valid ? Name(decision->selected) : "small-batch";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: maintenance_choice_demo <tpch.duckdb> [query=1]\n";
    return 2;
  }
  const int q = argc > 2 ? std::atoi(argv[2]) : 1;
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection c(db);
  accelerator_codegen::RegisterAcceleratorFunctions(c);
  const auto &entry = engine::LookupSharedQuery("tpch", q);
  DuckdbRuntime runtime(c, entry, 10.0);
  runtime.Build();

  const std::vector<Shape> shapes = {
      {"point (1 row)", "l_orderkey = 1 AND l_linenumber = 1"},
      {"1 day", "l_shipdate = DATE '1995-06-17'"},
      {"1 week", "l_shipdate BETWEEN DATE '1995-06-11' AND DATE '1995-06-17'"},
      {"1 month", "l_shipdate BETWEEN DATE '1995-06-01' AND DATE '1995-06-30'"},
      {"1 quarter", "l_shipdate BETWEEN DATE '1995-04-01' AND DATE '1995-06-30'"},
      {"1 year", "l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1995-12-31'"},
      {"3 years", "l_shipdate BETWEEN DATE '1994-01-01' AND DATE '1996-12-31'"},
      {"all rows", "TRUE"},
  };
  const std::vector<double> reads_per_write = {0.1, 1.0, 10.0};
  const auto params = core::LoadAuxCostModelJson(
      accelerator_codegen::aux_state::harness::SharedCostModelConfigPath(entry));
  const std::vector<mnt::MaintenanceAction> actions = {
      mnt::MaintenanceAction::ApplyDelta, mnt::MaintenanceAction::RebuildPartition,
      mnt::MaintenanceAction::RebuildAll};

  if (argc > 5 && std::string(argv[3]) == "trace") {
    const std::string aux_sql = accelerator_codegen::test_support::CrossAccelAuxSql(q);
    // The plain-DuckDB side runs SQL; Q6's shared reference is the stateless
    // accelerator, so use the TPC-H SQL with the same parameters instead.
    const std::string ref_sql =
        q == 6 ? "SELECT sum(l_extendedprice * l_discount) AS revenue FROM lineitem "
                 "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
                 "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25"
               : accelerator_codegen::test_support::CrossAccelReferenceSql(q);
    const std::string point = "l_orderkey = 1 AND l_linenumber = 1";
    // TRACE_SCHEDULE=contrast: each fixed action loses one phase (A: a point
    // write before every query; B: a 1-quarter bulk write every 40; C: both
    // mixed, a point write every 2 and a bulk every 40; D: read-heavy, a point
    // write every 25). Bulk is 1 quarter there, 1 year in the default schedule.
    const char *schedule_env = std::getenv("TRACE_SCHEDULE");
    const bool contrast = schedule_env && std::string(schedule_env) == "contrast";
    const std::string bulk = contrast
                                 ? "l_shipdate BETWEEN DATE '1995-04-01' AND DATE '1995-06-30'"
                                 : "l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1995-12-31'";
    struct Step {
      char phase;
      const char *kind; // "", "point", "bulk"
    };
    std::vector<Step> steps;
    if (contrast) {
      // Even counts per kind, so the +1/-1 writes leave the data as it was.
      for (int i = 0; i < 150; ++i)
        steps.push_back({'A', "point"});
      for (int i = 0; i < 160; ++i)
        steps.push_back({'B', i % 40 == 20 ? "bulk" : ""});
      for (int i = 0; i < 160; ++i)
        steps.push_back({'C', i % 40 == 20 ? "bulk" : i % 2 ? "point" : ""});
      for (int i = 0; i < 150; ++i)
        steps.push_back({'D', i % 25 == 12 ? "point" : ""});
    } else {
      for (int i = 0; i < 150; ++i)
        steps.push_back({'A', i % 10 == 9 ? "point" : ""});
      for (int i = 0; i < 150; ++i)
        steps.push_back({'B', i % 16 == 15 ? "bulk" : ""});
      for (int i = 0; i < 150; ++i)
        steps.push_back({'C', "point"});
      for (int i = 0; i < 150; ++i)
        steps.push_back({'D', i % 10 == 9 ? "point" : ""});
    }
    auto ms_since = [](Clock::time_point t) {
      return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    auto query = [&](duckdb::Connection &conn, const std::string &sql) {
      auto r = conn.Query(sql);
      if (!r || r->HasError()) {
        throw std::runtime_error(sql + ": " + (r ? r->GetError() : "null"));
      }
    };
    FILE *out = std::fopen(argv[5], "w");
    std::fprintf(out, "index,phase,write,strategy,ms,action,round,read_ms\n");
    // TRACE_ROUNDS (default 1): repeat the whole trace, rotating which
    // strategy runs first each round, so no strategy always runs on a colder
    // process (the plot takes the per-query median over rounds).
    const char *rounds_env = std::getenv("TRACE_ROUNDS");
    const int rounds = rounds_env && *rounds_env ? std::max(1, std::atoi(rounds_env)) : 1;
    int round = 0;
    double read_ms = 0; // the query alone, without the write before it
    // Each strategy writes +1 then -1 alternately per shape: the data returns.
    auto run = [&](const std::string &label, auto &&write_and_read) {
      int sign[2] = {1, 1};
      for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto &step = steps[i];
        std::string where;
        int delta = 0;
        if (*step.kind) {
          const int k = std::string(step.kind) == "bulk";
          where = k ? bulk : point;
          delta = sign[k];
          sign[k] = -sign[k];
        }
        std::string action;
        const double ms = write_and_read(where, delta, action);
        std::fprintf(out, "%zu,%c,%s,%s,%.3f,%s,%d,%.3f\n", i, step.phase, step.kind, label.c_str(), ms,
                     action.c_str(), round, read_ms);
      }
      std::fflush(out);
    };
    for (round = 0; round < rounds; ++round) {
      {
        duckdb::DuckDB base_db(argv[4]);
        duckdb::Connection b(base_db);
        query(b, ref_sql); // warm
        run("base", [&](const std::string &where, int delta, std::string &) {
          const auto t = Clock::now();
          if (!where.empty())
            Exec(b, UpdateSql(delta, where));
          const auto r = Clock::now();
          query(b, ref_sql);
          read_ms = ms_since(r);
          return ms_since(t);
        });
      }
      runtime.SetExpectedQueriesBeforeNextWrite(10);
      const std::vector<std::pair<std::string, std::string>> strategies = {
          {"selected", ""}, {"ApplyDelta", "ApplyDelta"}, {"RebuildAll", "RebuildAll"}};
      for (std::size_t n = 0; n < strategies.size(); ++n) {
        const auto &[label, force] = strategies[(n + round) % strategies.size()];
        core::SetForcedMaintenanceActionName(force);
        query(c, aux_sql); // warm
        run(label, [&](const std::string &where, int delta, std::string &action) {
          const auto t = Clock::now();
          if (!where.empty()) {
            runtime.ApplyWrite(UpdateSql(delta, where));
            action = CommitAction(runtime);
          }
          const auto r = Clock::now();
          query(c, aux_sql);
          read_ms = ms_since(r);
          return ms_since(t);
        });
        accelerator_codegen::test_support::RequireCrossAccelQueryCorrectness(c, q, "Q" + std::to_string(q));
      }
    }
    core::SetForcedMaintenanceActionName("");
    std::fclose(out);
    return 0;
  }

  if (argc > 5 && std::string(argv[3]) == "phases") {
    // PHASES_WRITES (default 4): writes per shape and strategy.
    const char *writes_env = std::getenv("PHASES_WRITES");
    const int kWrites = writes_env && *writes_env ? std::max(1, std::atoi(writes_env)) : 4;
    constexpr int kReadsPerWrite = 10;
    const std::string aux_sql = accelerator_codegen::test_support::CrossAccelAuxSql(q);
    const std::string ref_sql = accelerator_codegen::test_support::CrossAccelReferenceSql(q);
    auto ms_since = [](Clock::time_point t) {
      return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    auto read = [&](duckdb::Connection &conn, const std::string &sql) {
      const auto t = Clock::now();
      auto r = conn.Query(sql);
      if (!r || r->HasError()) {
        throw std::runtime_error(sql + ": " + (r ? r->GetError() : "null"));
      }
      return ms_since(t);
    };
    FILE *out = std::fopen(argv[5], "w");
    std::fprintf(out, "phase,rows,days,strategy,write_ms,read_ms,chosen\n");
    // Plain DuckDB on its own copy.
    {
      duckdb::DuckDB base_db(argv[4]);
      duckdb::Connection b(base_db);
      for (const auto &shape : shapes) {
        const auto rows = Scalar(b, "SELECT count(*) FROM lineitem WHERE " + shape.where);
        const auto days = Scalar(b, "SELECT count(DISTINCT l_shipdate) FROM lineitem WHERE " + shape.where);
        double w = 0, rd = 0;
        for (int i = 0; i < kWrites; ++i) {
          const auto t = Clock::now();
          Exec(b, UpdateSql(i % 2 ? -1 : 1, shape.where));
          w += ms_since(t);
          for (int k = 0; k < kReadsPerWrite; ++k)
            rd += read(b, ref_sql);
        }
        std::fprintf(out, "%s,%lld,%lld,base,%.3f,%.3f,\n", shape.name, (long long)rows, (long long)days,
                     w / kWrites, rd / (kWrites * kReadsPerWrite));
        std::fflush(out);
      }
    }
    std::vector<std::pair<std::string, std::string>> strategies = {
        {"selected", ""}, {"ApplyDelta", "ApplyDelta"},
        {"RebuildPartition", "RebuildPartition"}, {"RebuildAll", "RebuildAll"}};
    // PHASES_SKIP_PARTITION=1: leave out forced RebuildPartition.
    if (const char *skip = std::getenv("PHASES_SKIP_PARTITION"); skip && *skip == '1')
      strategies.erase(strategies.begin() + 2);
    runtime.SetExpectedQueriesBeforeNextWrite(kReadsPerWrite);
    // Warm-up (unrecorded): every phase once, so no strategy runs cold.
    for (const auto &shape : shapes) {
      for (int i = 0; i < 2; ++i) {
        runtime.BeginWrite();
        Exec(c, UpdateSql(i % 2 ? -1 : 1, shape.where));
        runtime.CommitWrite();
        read(c, aux_sql);
      }
    }
    for (const auto &[label, force] : strategies) {
      core::SetForcedMaintenanceActionName(force);
      for (const auto &shape : shapes) {
        const auto rows = Scalar(c, "SELECT count(*) FROM lineitem WHERE " + shape.where);
        const auto days = Scalar(c, "SELECT count(DISTINCT l_shipdate) FROM lineitem WHERE " + shape.where);
        double w = 0, rd = 0;
        std::string chosen;
        for (int i = 0; i < kWrites; ++i) {
          core::ClearMaintenanceDecisionLog();
          const auto t = Clock::now();
          runtime.BeginWrite();
          Exec(c, UpdateSql(i % 2 ? -1 : 1, shape.where));
          runtime.CommitWrite();
          w += ms_since(t);
          chosen = CommitAction(runtime);
          for (int k = 0; k < kReadsPerWrite; ++k)
            rd += read(c, aux_sql);
        }
        if (!force.empty())
          chosen = force;
        std::fprintf(out, "%s,%lld,%lld,%s,%.3f,%.3f,%s\n", shape.name, (long long)rows, (long long)days,
                     label.c_str(), w / kWrites, rd / (kWrites * kReadsPerWrite), chosen.c_str());
        std::fflush(out);
        accelerator_codegen::test_support::RequireCrossAccelQueryCorrectness(c, q, "Q" + std::to_string(q));
      }
    }
    core::SetForcedMaintenanceActionName("");
    std::fclose(out);
    return 0;
  }

  std::printf("TPC-H Q%d on DuckDB. Predicted / measured costs in ms.\n\n", q);
  for (const auto &shape : shapes) {
    const auto rows = Scalar(c, "SELECT count(*) FROM lineitem WHERE " + shape.where);
    const auto days = Scalar(c, "SELECT count(DISTINCT l_shipdate) FROM lineitem WHERE " + shape.where);
    std::printf("== %s: %lld rows, %lld days\n", shape.name, static_cast<long long>(rows),
                static_cast<long long>(days));
    for (const double rpw : reads_per_write) {
      runtime.SetExpectedQueriesBeforeNextWrite(rpw);
      core::ClearMaintenanceDecisionLog();
      const double ms = Write(runtime, c, shape, +1);
      const auto log = core::LastMaintenanceDecision();
      Write(runtime, c, shape, -1);
      if (!log || !log->valid) {
        std::printf("   reads/write=%-5g no decision logged\n", rpw);
        continue;
      }
      // The same request on an engine whose readers can rebuild.
      core::MaintenanceRequest request;
      request.rows_touched = log->rows_touched;
      request.affected_partition_count = log->affected_partition_count;
      request.incremental_supported = log->predicted_apply_delta_us > 0;
      request.partition_rebuild_supported = true;
      request.expected_queries_before_next_write = rpw;
      const auto rebuildable = core::SelectAuxMaintenanceAction(request, params);
      std::printf("   reads/write=%-5g chose %-17s (%.1f ms commit) | predicted: delta=%.1f "
                  "partition=%.1f all=%.1f deferred=%.1f | where readers can rebuild: %s\n",
                  rpw, Name(log->selected), ms, log->predicted_apply_delta_us / 1000,
                  log->predicted_rebuild_partition_us / 1000, log->predicted_rebuild_all_us / 1000,
                  log->predicted_invalidate_us / 1000, Name(rebuildable.action));
    }
    std::printf("   measured commit (forced):");
    for (const auto action : actions) {
      core::SetForcedMaintenanceActionName(Name(action));
      const double up = Write(runtime, c, shape, +1);
      const double down = Write(runtime, c, shape, -1);
      std::printf(" %s=%.1f", Name(action), (up + down) / 2);
    }
    core::SetForcedMaintenanceActionName("");
    std::printf("\n");
    // The accelerator must still match the reference SQL.
    try {
      accelerator_codegen::test_support::RequireCrossAccelQueryCorrectness(c, q, "Q" + std::to_string(q));
    } catch (const std::exception &e) {
      std::printf("   MISMATCH after %s: %s\n", shape.name, e.what());
      return 1;
    }
  }
  return 0;
}
