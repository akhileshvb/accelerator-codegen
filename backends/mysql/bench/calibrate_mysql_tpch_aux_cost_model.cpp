// Calibrate a MySQL TPC-H aux cost model and write
// accelerators/tpch/qN/config/qN_aux_cost_model_mysql.json.
//
// Read side: the accelerator's hit time, the reference SQL's time, and the
// slowdown another accelerator's queries cause (interference). Maintenance:
// core::CalibrateFromWrites (the substrate procedure) over the module's
// CalibrationWrites, through MysqlCalibrationEngine below (one autocommit
// write with the forced action, then one accelerator query). A query whose
// module has no calibration writes keeps its configured maintenance costs.
// The writes revert themselves (apply, then revert).
//
// Usage: calibrate_mysql_tpch_aux_cost_model --query N [out_json]  (a leading positional query is accepted)
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eval/bench/driver/bench_cli.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"
#include "substrate/aux/core/aux_calibration.hpp"

namespace {

using namespace accel::mysql::tpch_harness;
namespace core = accelerator_codegen::aux_state::core;
namespace mnt = accelerator_codegen::aux_state::maintenance;
using Clock = std::chrono::steady_clock;

double TimeUs(const Tools &tools, const std::string &sql, int warmup, int iters) {
  return TimeSqlBatchUs(tools, sql, warmup, iters);
}

// The MySQL side of core::CalibrateFromWrites. Captured writes are
// autocommit statements (MySQL's capture contract); the forced action and
// the decision log are server-wide.
class MysqlCalibrationEngine final : public core::CalibrationEngine {
public:
  MysqlCalibrationEngine(const Tools &tools_p, std::string accel_sql_p)
      : tools(tools_p), accel_sql(std::move(accel_sql_p)) {}

  // One mysql session per call; the server times the write and the read
  // (NOW(6) at each statement's start), so client start-up is not charged.
  // The decision is read before the forced action is reset (resetting it
  // clears the decision log).
  Timed WriteThenRead(const std::string &sql, std::optional<mnt::MaintenanceAction> forced) override {
    const std::string action = forced ? mnt::MaintenanceActionName(*forced) : "";
    const std::string out = RunMysqlScript(
        tools, "DO accel_set_force_maintenance_action(" + SqlLiteral(action) + ");\n"
               "SET @t0 = NOW(6);\n" + sql + ";\nSET @t1 = NOW(6);\n" + accel_sql + ";\n"
               "SELECT TIMESTAMPDIFF(MICROSECOND, @t0, @t1), TIMESTAMPDIFF(MICROSECOND, @t1, NOW(6)), "
               "accel_get_last_maintenance_decision();\n"
               "DO accel_set_force_maintenance_action('');\n");
    std::string trimmed = TrimLines(out);
    while (!trimmed.empty() && trimmed.back() == '\n') trimmed.pop_back();
    const auto nl = trimmed.rfind('\n');
    const std::string last = nl == std::string::npos ? trimmed : trimmed.substr(nl + 1);
    const auto bar1 = last.find('|');
    const auto bar2 = bar1 == std::string::npos ? bar1 : last.find('|', bar1 + 1);
    Require(bar2 != std::string::npos, "calibration timing line missing: " + last);
    Timed timed;
    const double write_us = std::stod(last.substr(0, bar1));
    timed.read_us = std::stod(last.substr(bar1 + 1, bar2 - bar1 - 1));
    timed.us = write_us + timed.read_us;
    timed.decision = core::ParseMaintenanceDecision(last.substr(bar2 + 1));
    return timed;
  }

private:
  const Tools &tools;
  std::string accel_sql;
};


} // namespace

int main(int argc, char **argv) {
  try {
    const accelerator_codegen::benchmarks::driver::Args args(argc, argv, 1);
    const int query_id = args.query;
    const std::string name = "q" + std::to_string(query_id) + "_aux_cost_model";
    const std::string config_dir = std::getenv("ACCEL_SRC") ? std::string(std::getenv("ACCEL_SRC")) + "/accelerators/tpch/q" +
                                                                   std::to_string(query_id) + "/config/"
                                                             : "accelerators/tpch/q" + std::to_string(query_id) + "/config/";
    const std::string out_path = args.Positional(0, config_dir + name + "_mysql.json");
    auto tools = ConnectTpch();
    EnsureExtension(tools);
    EnsureMutationTriggers(tools);
    const auto &entry = LookupMysqlQuery("tpch", query_id);
    const std::string sql = ConcreteBaseSql(query_id, entry);
    const std::string aux = ConcreteAccelSql(query_id, entry, true);

    // Seed: this engine's model if any, else the shared one.
    auto params = core::LoadAuxCostModelJson(config_dir + name + ".json");
    params = core::LoadAuxCostModelJson(config_dir + name + "_mysql.json", params);

    Require(QuerySQL(tools, aux).first, "aux execute failed"); // builds the state
    params.aux_hit_us = TimeUs(tools, aux, 2, 7);
    params.fallback_us = TimeUs(tools, sql, 1, 5);

    // Interference: this accelerator's read time while another one is busy.
    const int peer = query_id == 1 ? 3 : 1;
    const std::string peer_aux = ConcreteAccelSql(peer, LookupMysqlQuery("tpch", peer), true);
    QuerySQL(tools, peer_aux);
    const double solo = TimeUs(tools, aux, 1, 5);
    QuerySQL(tools, peer_aux);
    const double busy = TimeUs(tools, aux, 0, 5);
    params.interference_query_slowdown_factor = std::max(0.0, (busy - solo) / std::max(solo, 1.0));

    const auto writes = core::CalibrationWritesFor("tpch", query_id);
    if (writes.empty()) {
      std::cerr << "Q" << query_id << ": the module has no calibration writes; maintenance costs kept\n";
    } else {
      MysqlCalibrationEngine engine(tools, aux);
      const auto fitted = core::CalibrateFromWrites(writes, engine, params);
      params = fitted.params;
      for (const auto &name : fitted.unfitted) {
        std::cout << "  not calibrated (no write measured it; kept): " << name << "\n";
      }
      for (const auto &s : fitted.samples) {
        std::cout << "  write " << s.name << ": rows=" << s.rows << " partitions=" << s.partitions
                  << " control=" << s.control_us << "us delta=" << s.apply_delta_us
                  << "us partition=" << s.rebuild_partition_us << "us all=" << s.rebuild_all_us << "us\n";
      }
    }

    core::AuxCalibrationResult result;
    result.params = params;
    core::WriteAuxCostModelJson(out_path, result, "mysql", "tpch_q" + std::to_string(query_id));
    std::cout << "Q" << query_id << ": aux_hit=" << params.aux_hit_us << "us fallback=" << params.fallback_us
              << "us apply_delta=" << params.apply_delta_overhead_us << "+" << params.apply_delta_per_row_us
              << "*rows us rebuild_partition=" << params.partition_rebuild_base_us << "+"
              << params.partition_rebuild_us << "*partitions us rebuild_all=" << params.full_rebuild_us
              << "us -> " << out_path << "\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "calibrate_mysql_tpch_aux_cost_model failed: " << ex.what() << "\n";
    return 1;
  }
}
