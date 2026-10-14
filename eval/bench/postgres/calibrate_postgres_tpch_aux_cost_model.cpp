// Calibrate a PostgreSQL TPC-H aux cost model and write
// accelerators/tpch/qN/config/qN_aux_cost_model_postgres.json.
//
// Read side: the accelerator's hit time and the reference SQL's time.
// Maintenance: core::CalibrateFromWrites (the substrate procedure) over the
// module's CalibrationWrites, through PostgresCalibrationEngine below (one
// write transaction with the forced action, then one accelerator query, on
// one backend). Runs on a copy of the TPC-H database, dropped at the end.
//
// Usage: calibrate_postgres_tpch_aux_cost_model <query> [out.json]
// Env: TPCH_PG_DB (template database, default accel_tpch_test)
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <libpq-fe.h>

#include "substrate/aux/core/aux_calibration.hpp"
#include "eval/tests/postgres/postgres_tpch_harness.hpp"
#include "substrate/aux/core/aux_calibration.hpp"

namespace {

using accel::postgres::Tools;
using namespace accel::postgres::tpch_harness;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
namespace core = accelerator_codegen::aux_state::core;
namespace mnt = accelerator_codegen::aux_state::maintenance;
using Clock = std::chrono::steady_clock;

class Conn {
public:
  Conn(const Tools &tools, const std::string &db) {
    const std::string info = "host=" + tools.host + " port=" + std::to_string(tools.port) +
                             " user=" + tools.user + " password=" + tools.password + " dbname=" + db;
    conn = PQconnectdb(info.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
      throw std::runtime_error(std::string("connect: ") + PQerrorMessage(conn));
    }
    Exec("SET client_min_messages = warning");
    if (!tools.extension_so.empty()) {
      Exec("LOAD '" + tools.extension_so + "'");
    }
  }
  ~Conn() { PQfinish(conn); }
  Conn(const Conn &) = delete;
  Conn &operator=(const Conn &) = delete;

  void Exec(const std::string &sql) {
    PGresult *r = PQexec(conn, sql.c_str());
    const auto st = PQresultStatus(r);
    const std::string err = PQresultErrorMessage(r);
    PQclear(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
      throw std::runtime_error(sql.substr(0, 200) + ": " + err);
    }
  }
  // The first cell ("" if NULL or no rows).
  std::string Value(const std::string &sql) {
    PGresult *r = PQexec(conn, sql.c_str());
    std::string out;
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0 && !PQgetisnull(r, 0, 0)) {
      out = PQgetvalue(r, 0, 0);
    }
    const bool ok = PQresultStatus(r) == PGRES_TUPLES_OK;
    const std::string err = PQresultErrorMessage(r);
    PQclear(r);
    if (!ok) {
      throw std::runtime_error(sql.substr(0, 200) + ": " + err);
    }
    return out;
  }

private:
  PGconn *conn = nullptr;
};

// The PostgreSQL side of core::CalibrateFromWrites, on one backend (the
// accelerator's state and decision log are per backend).
class PostgresCalibrationEngine final : public core::CalibrationEngine {
public:
  PostgresCalibrationEngine(Conn &conn_p, std::string accel_sql_p)
      : conn(conn_p), accel_sql(std::move(accel_sql_p)) {}

  Timed WriteThenRead(const std::string &sql, std::optional<mnt::MaintenanceAction> forced) override {
    conn.Exec(std::string("SELECT accel_postgres_set_force_maintenance_action('") +
              (forced ? mnt::MaintenanceActionName(*forced) : "") + "')");
    Timed timed;
    const auto start = Clock::now();
    conn.Exec("BEGIN");
    conn.Exec(sql);
    conn.Exec("COMMIT");
    const auto read_start = Clock::now();
    conn.Value(accel_sql);
    timed.read_us = std::chrono::duration<double, std::micro>(Clock::now() - read_start).count();
    timed.us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    // The decision first: setting the forced action clears the decision log.
    const auto text = conn.Value("SELECT accel_postgres_get_last_maintenance_decision()");
    conn.Exec("SELECT accel_postgres_set_force_maintenance_action('')");
    timed.decision = core::ParseMaintenanceDecision(text);
    if (!timed.decision) {
      warned = true;
      std::cerr << "no maintenance decision after: " << sql.substr(0, 120) << " (got '" << text << "')\n";
    }
    return timed;
  }

private:
  Conn &conn;
  std::string accel_sql;
  bool warned = false;
};


double MedianUs(Conn &conn, const std::string &sql, int samples) {
  std::vector<double> times;
  for (int i = 0; i < samples; ++i) {
    const auto start = Clock::now();
    conn.Value(sql);
    times.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
  }
  std::sort(times.begin(), times.end());
  return times[times.size() / 2];
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      std::cerr << "usage: calibrate_postgres_tpch_aux_cost_model <query> [out.json]\n";
      return 2;
    }
    const int q = std::stoi(argv[1]);
    const std::string name = "q" + std::to_string(q) + "_aux_cost_model";
    const std::string config_dir = std::getenv("ACCEL_SRC") ? std::string(std::getenv("ACCEL_SRC")) + "/accelerators/tpch/q" +
                                                                   std::to_string(q) + "/config/"
                                                             : "accelerators/tpch/q" + std::to_string(q) + "/config/";
    const std::string out = argc > 2 ? argv[2] : config_dir + name + "_postgres.json";

    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);
    const std::string db = "accel_tpch_calib";
    {
      Conn admin(tools, "postgres");
      admin.Exec("DROP DATABASE IF EXISTS " + db);
      admin.Exec("CREATE DATABASE " + db + " TEMPLATE " + tools.database);
    }
    const auto &entry = LookupPostgresQuery("tpch", q);
    const std::string base_sql = ConcreteBaseSql(q, entry);
    const std::string accel_sql = ConcreteAccelSql(q, entry, /*prefer_aux=*/true);

    core::AuxCalibrationResult result;
    {
      Conn conn(tools, db);
      // Seed: this engine's model if any, else the shared one.
      auto seed = core::LoadAuxCostModelJson(config_dir + name + ".json");
      seed = core::LoadAuxCostModelJson(config_dir + name + "_postgres.json", seed);
      conn.Value(accel_sql); // builds this backend's state
      seed.aux_hit_us = MedianUs(conn, accel_sql, 7);
      seed.fallback_us = MedianUs(conn, base_sql, 3);
      result.params = seed;

      const auto writes = core::CalibrationWritesFor("tpch", q);
      if (writes.empty()) {
        std::cerr << "Q" << q << ": the module has no calibration writes; maintenance costs kept\n";
      } else {
        PostgresCalibrationEngine engine(conn, accel_sql);
        const auto fitted = core::CalibrateFromWrites(writes, engine, seed);
        result.params = fitted.params;
        for (const auto &name : fitted.unfitted) {
        std::cout << "  not calibrated (no write measured it; kept): " << name << "\n";
      }
      for (const auto &s : fitted.samples) {
          std::cout << "  write " << s.name << ": rows=" << s.rows << " partitions=" << s.partitions
                    << " control=" << s.control_us << "us delta=" << s.apply_delta_us
                    << "us partition=" << s.rebuild_partition_us << "us all=" << s.rebuild_all_us
                    << "us\n";
        }
      }
    }
    {
      Conn admin(tools, "postgres");
      admin.Exec("DROP DATABASE IF EXISTS " + db);
    }
    core::WriteAuxCostModelJson(out, result, "postgres", "tpch_q" + std::to_string(q));
    const auto &p = result.params;
    std::cout << "Q" << q << ": aux_hit=" << p.aux_hit_us << "us fallback=" << p.fallback_us
              << "us apply_delta=" << p.apply_delta_overhead_us << "+" << p.apply_delta_per_row_us
              << "*rows us rebuild_partition=" << p.partition_rebuild_base_us << "+"
              << p.partition_rebuild_us << "*partitions us rebuild_all=" << p.full_rebuild_us
              << "us -> " << out << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "calibrate_postgres_tpch_aux_cost_model failed: " << e.what() << "\n";
    return 1;
  }
}
