// Phased hybrid workload for TPC-H Q20.
// Phases:
//   A: read-heavy point updates   -> ApplyDelta
//   B: read-heavy bulk localized  -> RebuildAll (cost-model crossover)
//   C: write-heavy point updates  -> ApplyDelta (overlay growth)
//   D: compact/rebuild, then read-heavy point updates -> ApplyDelta
//      (returns to Phase A performance after write-heavy storm)
//
// Env:
//   ACCEL_TPCH_DB                         writable DuckDB copy
//   ACCEL_Q20_PHASED_TIMELINE_CSV         output timeline path (stdout if empty)
//   ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL
//                                         cost model JSON (default: phased config)
//   ACCEL_Q20_PHASE_{A,B,C,D}_QUERIES     queries per phase (default 150)
//   ACCEL_Q20_PHASE_{A,B,C,D}_QPW         queries-per-write (A/D=10, B=15, C=3)
//   ACCEL_Q20_PHASE_B_BULK_ROWS           rows per bulk write (default 128)
//   ACCEL_Q20_PHASE_B_CACHE_THRASH_MB     after each RebuildAll maintenance
//                                         action, touch this many MiB before the
//                                         next measured query (default 2048) so
//                                         rebuilds show a cold-query dip
//   ACCEL_BENCH_REF_SAMPLES               DuckDB baseline samples (default 20)

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"
#include "catalog/query_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <malloc.h>
#endif

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace maintenance = accelerator_codegen::aux_state::maintenance;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

std::string GetEnvString(const char *name, const std::string &fallback = "") {
  const char *value = std::getenv(name);
  return (value && *value) ? std::string(value) : fallback;
}

int GetEnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  return std::stoi(value);
}

void Require(bool ok, const std::string &message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "query returned null");
  auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
  for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
    for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
      (void)materialized.GetValue(col, row);
    }
  }
}

double MeasureReferenceQueryMedianUs(duckdb::Connection &connection,
                                     const std::string &sql, int samples) {
  std::vector<double> query_samples;
  query_samples.reserve(static_cast<size_t>(samples));
  for (int i = 0; i < samples; i++) {
    const auto start = Clock::now();
    ExecuteAndConsume(connection, sql);
    query_samples.push_back(ElapsedUs(start, Clock::now()));
  }
  std::sort(query_samples.begin(), query_samples.end());
  return query_samples[query_samples.size() / 2];
}

std::vector<catalog::SharedProbe>
PrecomputeWriteProbes(duckdb::Connection &connection,
                      const catalog::SharedQueryCatalogEntry &entry) {
  auto result = connection.Query(
      "SELECT l_orderkey, l_linenumber, l_quantity, l_shipdate, "
      "l_partkey, l_suppkey FROM lineitem "
      "WHERE l_shipdate >= DATE '1997-01-01' "
      "  AND l_shipdate < DATE '1997-01-01' + INTERVAL '1' year "
      "ORDER BY l_orderkey, l_linenumber LIMIT 256");
  Require(result != nullptr && !result->HasError(), result->GetError());
  auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
  Require(materialized.RowCount() > 0, "failed to precompute Q20 write probes");
  std::vector<catalog::SharedProbe> probes;
  probes.reserve(materialized.RowCount());
  for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
    probes.push_back(entry.build_probe(materialized, row));
  }
  return probes;
}

bool ShouldWriteAtQueryIndex(int query_index, int queries_per_write) {
  return queries_per_write > 0 && query_index > 0 &&
         (query_index % queries_per_write) == 0;
}

void ThrashCaches(std::size_t bytes) {
  if (bytes == 0) {
    return;
  }
#if defined(__linux__)
  malloc_trim(0);
#endif
  std::vector<unsigned char> buf(bytes);
  volatile unsigned char sink = 0;
  constexpr std::size_t kStride = 64;
  for (std::size_t i = 0; i < buf.size(); i += kStride) {
    buf[i] = static_cast<unsigned char>(i);
    sink = static_cast<unsigned char>(sink + buf[i]);
  }
  (void)sink;
}

// Drop overlay chains / stale images and publish a fresh flat Build.
double CompactAuxState(DuckdbRuntime &runtime) {
  const auto start = Clock::now();
  runtime.InvalidateAll();
  runtime.Build();
#if defined(__linux__)
  malloc_trim(0);
#endif
  return ElapsedUs(start, Clock::now());
}

core::MaintenanceDecisionLogEntry MakeRebuildLogEntry(double actual_us) {
  core::MaintenanceDecisionLogEntry entry;
  entry.selected = maintenance::MaintenanceAction::RebuildAll;
  entry.staged_action = maintenance::MaintenanceAction::RebuildAll;
  entry.selected_predicted_cost_us = actual_us;
  entry.predicted_rebuild_all_us = actual_us;
  entry.actual_maintenance_us = actual_us;
  entry.valid = true;
  return entry;
}

int ActionToCode(maintenance::MaintenanceAction action) {
  switch (action) {
  case maintenance::MaintenanceAction::NoOp:
    return 0;
  case maintenance::MaintenanceAction::ApplyDelta:
    return 1;
  case maintenance::MaintenanceAction::RebuildPartition:
    return 2;
  case maintenance::MaintenanceAction::RebuildAll:
    return 3;
  case maintenance::MaintenanceAction::InvalidateAndFallback:
    return 4;
  }
  return -1;
}

struct PhaseSpec {
  const char *id;
  int num_queries;
  int queries_per_write;
  const char *kind;
};

void PrintPhasedHeader(std::ostream &out) {
  out << "phase_id,phase_query_index,global_query_index,event_type,selected_action,"
         "selected_action_code,predicted_apply_delta_us,predicted_rebuild_partition_us,"
         "predicted_rebuild_all_us,predicted_invalidate_us,predicted_selected_us,"
         "actual_maintenance_us,prediction_error_us,ewma_query_rate_eps,ewma_write_rate_eps,"
         "expected_queries_before_next_write,ewma_aux_hit_us,ewma_fallback_us,"
         "accel_hit_rate_so_far,fallback_rate_so_far,cumulative_maintenance_us,"
         "writes_in_phase,query_latency_us,write_latency_us,duckdb_query_median_us,"
         "query_speedup\n";
}

} // namespace

int main() {
  try {
    setenv("ACCEL_MAINTENANCE_MODE", "hybrid", 1);
    if (!std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL") ||
        !*std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL")) {
      setenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL",
             "config/q20_aux_cost_model_phased.json", 0);
    }

    const auto db_path = GetEnvString(
        "ACCEL_TPCH_DB",
        "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb");
    const auto timeline_path =
        GetEnvString("ACCEL_Q20_PHASED_TIMELINE_CSV", "");
    const int bulk_rows = std::max(2, GetEnvInt("ACCEL_Q20_PHASE_B_BULK_ROWS", 128));
    const int cache_thrash_mb =
        std::max(0, GetEnvInt("ACCEL_Q20_PHASE_B_CACHE_THRASH_MB", 2048));
    const std::size_t cache_thrash_bytes =
        static_cast<std::size_t>(cache_thrash_mb) * 1024u * 1024u;
    const int ref_samples = GetEnvInt("ACCEL_BENCH_REF_SAMPLES", 20);

    const std::vector<PhaseSpec> phases = {
        {"A", GetEnvInt("ACCEL_Q20_PHASE_A_QUERIES", 150),
         GetEnvInt("ACCEL_Q20_PHASE_A_QPW", 10), "point_lineitem"},
        {"B", GetEnvInt("ACCEL_Q20_PHASE_B_QUERIES", 150),
         GetEnvInt("ACCEL_Q20_PHASE_B_QPW", 15), "bulk_lineitem"},
        {"C", GetEnvInt("ACCEL_Q20_PHASE_C_QUERIES", 150),
         GetEnvInt("ACCEL_Q20_PHASE_C_QPW", 3), "point_lineitem"},
        {"D", GetEnvInt("ACCEL_Q20_PHASE_D_QUERIES", 150),
         GetEnvInt("ACCEL_Q20_PHASE_D_QPW", 10), "point_lineitem"},
    };

    const auto &entry = catalog::LookupSharedQuery("tpch", 20);
    duckdb::DuckDB database(db_path.c_str());
    duckdb::Connection connection(database);
    Require(connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");
    accelerator_codegen::RegisterAcceleratorFunctions(connection);

    auto probes = PrecomputeWriteProbes(connection, entry);
    Require(static_cast<int>(probes.size()) >= 1, "no write probes");

    const double duckdb_query_median_us =
        MeasureReferenceQueryMedianUs(connection, entry.base_sql, ref_samples);

    DuckdbRuntime runtime(connection, entry, 10.0);
    runtime.Build();
    // Settle initial Build so Phase A does not start on a one-shot cold miss.
    runtime.Execute();
    runtime.Execute();

    std::ofstream timeline;
    const bool write_timeline_file = !timeline_path.empty();
    if (write_timeline_file) {
      timeline.open(timeline_path, std::ios::trunc);
      PrintPhasedHeader(timeline);
    } else {
      PrintPhasedHeader(std::cout);
    }
    std::ostream &events_out = write_timeline_file ? timeline : std::cout;

    auto emit_event =
        [&](const std::string &phase_id, int phase_query_index,
            int global_query_index, const std::string &event_type,
            const std::optional<core::MaintenanceDecisionLogEntry> &decision,
            int accel_hits, int fallbacks, int queries_so_far, int writes_in_phase,
            double cumulative_maintenance_us, double query_latency_us,
            double write_latency_us, double expected_qpw) {
          std::string selected_action;
          int selected_code = -1;
          double predicted_apply = 0.0;
          double predicted_rebuild_part = 0.0;
          double predicted_rebuild_all = 0.0;
          double predicted_invalidate = 0.0;
          double predicted_selected = 0.0;
          double actual_maintenance = 0.0;
          double prediction_error = 0.0;
          if (decision) {
            const auto action = decision->staged_action != maintenance::MaintenanceAction::NoOp
                                    ? decision->staged_action
                                    : decision->selected;
            selected_action = maintenance::MaintenanceActionName(action);
            selected_code = ActionToCode(action);
            predicted_apply = decision->predicted_apply_delta_us;
            predicted_rebuild_part = decision->predicted_rebuild_partition_us;
            predicted_rebuild_all = decision->predicted_rebuild_all_us;
            predicted_invalidate = decision->predicted_invalidate_us;
            predicted_selected = decision->selected_predicted_cost_us;
            actual_maintenance = decision->actual_maintenance_us;
            prediction_error = actual_maintenance - predicted_selected;
          }
          const double hit_rate =
              queries_so_far > 0
                  ? static_cast<double>(accel_hits) /
                        static_cast<double>(queries_so_far)
                  : 0.0;
          const double fallback_rate =
              queries_so_far > 0
                  ? static_cast<double>(fallbacks) /
                        static_cast<double>(queries_so_far)
                  : 0.0;
          const double query_speedup =
              event_type == "query" && query_latency_us > 0.0
                  ? duckdb_query_median_us / query_latency_us
                  : 0.0;
          const double query_rate_eps = expected_qpw > 0.0 ? 1.0 : 0.0;
          const double write_rate_eps =
              expected_qpw > 0.0 ? 1.0 / expected_qpw : 0.0;
          events_out << phase_id << ',' << phase_query_index << ','
                     << global_query_index << ',' << event_type << ','
                     << selected_action << ',' << selected_code << ','
                     << std::fixed << std::setprecision(3) << predicted_apply
                     << ',' << predicted_rebuild_part << ','
                     << predicted_rebuild_all << ',' << predicted_invalidate
                     << ',' << predicted_selected << ',' << actual_maintenance
                     << ',' << prediction_error << ',' << query_rate_eps << ','
                     << write_rate_eps << ',' << expected_qpw << ','
                     << duckdb_query_median_us << ',' << duckdb_query_median_us
                     << ',' << hit_rate << ',' << fallback_rate << ','
                     << cumulative_maintenance_us << ',' << writes_in_phase
                     << ',' << query_latency_us << ',' << write_latency_us << ','
                     << duckdb_query_median_us << ',' << query_speedup << '\n';
        };

    int global_query_index = 0;
    int accel_hits = 0;
    int fallbacks = 0;
    int write_seq = 0;
    double cumulative_maintenance_us = 0.0;

    for (const auto &phase : phases) {
      int writes_in_phase = 0;
      const double expected_qpw =
          static_cast<double>(std::max(1, phase.queries_per_write));
      runtime.SetExpectedQueriesBeforeNextWrite(expected_qpw);
      emit_event(phase.id, 0, global_query_index, "phase_start", std::nullopt,
                 accel_hits, fallbacks, global_query_index, writes_in_phase,
                 cumulative_maintenance_us, 0.0, 0.0, expected_qpw);

      // Phase B: flatten Phase-A overlays before bulk rebuilds so the bulk
      // phase starts from a clean flat image (same as a fresh aux build).
      if (std::string(phase.id) == "B") {
        const double compact_us = CompactAuxState(runtime);
        cumulative_maintenance_us += compact_us;
        runtime.Execute();
        runtime.Execute();
        std::cerr << "phase B: compacted Phase A overlays (" << compact_us
                  << " us)\n";
      }

      // Phase D: cost-model story — after write-heavy Phase C, compact/rebuild
      // before returning to read-heavy point updates so D tracks Phase A.
      if (std::string(phase.id) == "D") {
        const auto write_start = Clock::now();
        const double compact_us = CompactAuxState(runtime);
        const double write_latency_us = ElapsedUs(write_start, Clock::now());
        cumulative_maintenance_us += compact_us;
        writes_in_phase++;
        const auto decision = MakeRebuildLogEntry(compact_us);
        emit_event(phase.id, 0, global_query_index, "write", decision, accel_hits,
                   fallbacks, global_query_index, writes_in_phase,
                   cumulative_maintenance_us, 0.0, write_latency_us, expected_qpw);
        // Warm the flat image so D's first measured queries match A, not a
        // one-shot post-rebuild cold miss.
        runtime.Execute();
        runtime.Execute();
        std::cerr << "phase D: compacted Phase C overlays (" << compact_us
                  << " us)\n";
      }

      for (int phase_query_index = 0; phase_query_index < phase.num_queries;
           phase_query_index++, global_query_index++) {
        double write_latency_us = 0.0;
        std::optional<core::MaintenanceDecisionLogEntry> decision;
        if (ShouldWriteAtQueryIndex(phase_query_index, phase.queries_per_write)) {
          const auto write_start = Clock::now();
          const bool bulk = std::string(phase.kind) == "bulk_lineitem";
          if (bulk) {
            const int n =
                std::min(bulk_rows, static_cast<int>(probes.size()));
            std::vector<catalog::SharedProbe> batch;
            batch.reserve(static_cast<size_t>(n));
            for (int i = 0; i < n; i++) {
              batch.push_back(
                  probes[static_cast<size_t>((write_seq + i) % probes.size())]);
            }
            runtime.ApplyBatch(batch);
          } else {
            auto &probe =
                probes[static_cast<size_t>(write_seq) % probes.size()];
            runtime.Apply(probe);
          }
          write_latency_us = ElapsedUs(write_start, Clock::now());
          write_seq++;
          writes_in_phase++;
          decision = core::LastMaintenanceDecision();
          if (decision) {
            cumulative_maintenance_us += decision->actual_maintenance_us;
            // Thrash only after RebuildAll so always-ApplyDelta baselines are not
            // penalized, and always-RebuildAll baselines show a cold post-rebuild
            // query on every write (point or bulk).
            if (decision->selected == maintenance::MaintenanceAction::RebuildAll &&
                cache_thrash_bytes > 0) {
              ThrashCaches(cache_thrash_bytes);
            }
          }
          emit_event(phase.id, phase_query_index, global_query_index, "write",
                     decision, accel_hits, fallbacks, global_query_index,
                     writes_in_phase, cumulative_maintenance_us, 0.0,
                     write_latency_us, expected_qpw);
        }

        const auto query_start = Clock::now();
        runtime.Execute();
        const auto query_latency_us = ElapsedUs(query_start, Clock::now());
        accel_hits++;
        emit_event(phase.id, phase_query_index, global_query_index, "query",
                   std::nullopt, accel_hits, fallbacks, global_query_index + 1,
                   writes_in_phase, cumulative_maintenance_us, query_latency_us,
                   0.0, expected_qpw);
      }
    }

    if (write_timeline_file) {
      std::cerr << "wrote phased timeline to " << timeline_path << '\n';
    }
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "benchmark_q20_phased_eval failed: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
