// Measure MySQL TPC-H aux vs SQL and write config/qN_aux_cost_model_mysql.json.
// Usage: calibrate_mysql_tpch_aux_cost_model [query_id] [out_json]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "aux_state/mysql_tpch_harness.hpp"

namespace {

using namespace accel::mysql::tpch_harness;

double MedianUs(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

double TimeUs(const Tools &tools, const std::string &sql, int warmup, int iters) {
  return TimeSqlBatchUs(tools, sql, warmup, iters);
}

void WriteModel(const std::string &path, int query_id, double aux_hit_us,
                double fallback_us, double rebuild_us, double apply_delta_us,
                double interference_rebuild, double interference_delta,
                double slowdown) {
  std::ofstream out(path);
  out << "{\n"
      << "  \"version\": 1,\n"
      << "  \"engine\": \"mysql\",\n"
      << "  \"accelerator\": \"tpch_q" << query_id << "\",\n"
      << "  \"aux_hit_us\": " << aux_hit_us << ",\n"
      << "  \"fallback_us\": " << fallback_us << ",\n"
      << "  \"partition_rebuild_us\": " << rebuild_us << ",\n"
      << "  \"full_rebuild_us\": " << rebuild_us * 1.6 << ",\n"
      << "  \"first_query_blocking_us\": " << rebuild_us << ",\n"
      << "  \"apply_delta_overhead_us\": " << apply_delta_us << ",\n"
      << "  \"apply_delta_base_us\": " << apply_delta_us << ",\n"
      << "  \"apply_delta_per_row_us\": " << std::max(0.5, apply_delta_us / 8.0) << ",\n"
      << "  \"apply_delta_max_rows\": 1048576,\n"
      << "  \"apply_delta_max_part_fanout\": 16,\n"
      << "  \"invalidation_overhead_us\": " << std::max(10.0, apply_delta_us * 0.25) << ",\n"
      << "  \"decision_margin_us\": 1000,\n"
      << "  \"min_samples\": 8,\n"
      << "  \"incremental_failure_prob\": 0.01,\n"
      << "  \"interference_rebuild_per_query_us\": " << interference_rebuild << ",\n"
      << "  \"interference_rebuild_scan_byte_us\": 0,\n"
      << "  \"interference_delta_per_query_us\": " << interference_delta << ",\n"
      << "  \"interference_invalidate_per_query_us\": " << interference_delta * 0.5 << ",\n"
      << "  \"interference_query_slowdown_factor\": " << slowdown << ",\n"
      << "  \"calibration_samples\": 5\n"
      << "}\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    int query_id = argc >= 2 ? std::stoi(argv[1]) : 1;
    std::string out_path =
        argc >= 3 ? argv[2]
                  : ("config/q" + std::to_string(query_id) + "_aux_cost_model_mysql.json");
    auto tools = ConnectTpch();
    EnsureExtension(tools);
    EnsureMutationTriggers(tools);
    const auto &entry = LookupMysqlQuery("tpch", query_id);
    const std::string sql = ConcreteBaseSql(query_id, entry);
    const std::string aux = ConcreteAccelSql(query_id, entry, true);

    const auto cold_start = std::chrono::steady_clock::now();
    auto cold = QuerySQL(tools, aux);
    const auto cold_end = std::chrono::steady_clock::now();
    if (!cold.first) {
      throw std::runtime_error("cold aux execute failed: " + cold.second);
    }
    const double rebuild_us =
        std::chrono::duration<double, std::micro>(cold_end - cold_start).count();
    const double aux_hit_us = TimeUs(tools, aux, 2, 7);
    const double fallback_us = TimeUs(tools, sql, 1, 5);

    RunSQL(tools, "SELECT mysql_accel_set_force_maintenance_action('ApplyDelta')");
    const auto delta_start = std::chrono::steady_clock::now();
    RunSQL(tools, InvalidatingUpdateSql(query_id));
    QuerySQL(tools, aux);
    const auto delta_end = std::chrono::steady_clock::now();
    const double apply_delta_us =
        std::chrono::duration<double, std::micro>(delta_end - delta_start).count();
    RunSQL(tools, "SELECT mysql_accel_set_force_maintenance_action('')");

    const int peer = query_id == 1 ? 3 : 1;
    const auto &peer_entry = LookupMysqlQuery("tpch", peer);
    const std::string peer_aux = ConcreteAccelSql(peer, peer_entry, true);
    QuerySQL(tools, peer_aux);
    const double solo = TimeUs(tools, aux, 1, 5);
    const auto busy_start = std::chrono::steady_clock::now();
    QuerySQL(tools, peer_aux);
    const double busy = TimeUs(tools, aux, 0, 5);
    (void)busy_start;
    const double slowdown = std::max(0.0, (busy - solo) / std::max(solo, 1.0));
    const double interference_rebuild = std::max(0.0, busy - solo);
    const double interference_delta = interference_rebuild * 0.25;

    WriteModel(out_path, query_id, aux_hit_us, fallback_us, rebuild_us,
               apply_delta_us, interference_rebuild, interference_delta, slowdown);
    const double speedup = fallback_us / std::max(aux_hit_us, 1.0);
    std::cout << "calibrated Q" << query_id << " aux_hit_us=" << aux_hit_us
              << " fallback_us=" << fallback_us << " speedup=" << speedup
              << " wrote " << out_path << "\n";
    if (speedup < 1.2) {
      std::cerr << "warning: calibrated speedup is modest (" << speedup << ")\n";
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "calibrate_mysql_tpch_aux_cost_model failed: " << ex.what() << "\n";
    return 1;
  }
}
