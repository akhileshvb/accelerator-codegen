// GENERATED BACKEND
// The frozen test suite of a query (eval/harness/suite.hpp), run against the DuckDB accelerated path.
// Configured by the host contract's ACCEL_SUITE_* environment (pipeline/prompts/engine_backend/host_contract.md,
// "Test suite"); every run states its seed, and a failure carries what reproduces it.
#pragma once

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/harness/suite.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"

namespace accelerator_codegen::aux_state::harness {

inline bool SuiteRequested() {
  const char *dir = std::getenv("ACCEL_SUITE_DIR");
  return dir && *dir;
}

inline std::string SuiteEnv(const char *name, const std::string &fallback = "") {
  const char *value = std::getenv(name);
  return value && *value ? value : fallback;
}

// The suite on plain SQL, no accelerator: every tuple evaluates, every transaction applies and rolls back, and
// which of them change an answer. Writes the report (ACCEL_SUITE_REPORT) and returns whether it found no error.
inline bool RunSuiteValidation(const std::string &db_path) {
  namespace suite = ::accelerator_codegen::suite;
  const suite::Spec spec = suite::LoadSpec(SuiteEnv("ACCEL_SUITE_DIR"), "ACCEL_SUITE_REPLACE_PARAMS_FILE",
                                           "ACCEL_SUITE_PARAMS_FILE");
  std::ofstream report(SuiteEnv("ACCEL_SUITE_REPORT", "/dev/stderr"));
  return suite::Validate(db_path, spec, suite::ReadFile(SuiteEnv("ACCEL_SUITE_QUERY_SQL")),
                         suite::SchemaSql("ACCEL_SUITE_SCHEMA_SQL"), report) == 0;
}

// Differential of the suite on a writable copy of the database: the fixed tuples plus the run's random ones, after
// the build and after each transaction. Throws when the accelerated path disagrees with the query.
inline void RunSuiteDifferential(const std::string &db_path, const engine::SharedQueryCatalogEntry &entry) {
  namespace suite = ::accelerator_codegen::suite;
  namespace core = ::accelerator_codegen::aux_state::core;
  suite::Spec spec = suite::LoadSpec(SuiteEnv("ACCEL_SUITE_DIR"), "ACCEL_SUITE_REPLACE_PARAMS_FILE",
                                     "ACCEL_SUITE_PARAMS_FILE");
  if (const auto txns = SuiteEnv("ACCEL_SUITE_TXNS"); !txns.empty())
    suite::KeepTxns(spec, txns);
  const std::string query_sql = suite::ReadFile(SuiteEnv("ACCEL_SUITE_QUERY_SQL"));
  const std::string function = SuiteEnv("ACCEL_SUITE_FUNCTION");
  suite::Require(!function.empty() && !query_sql.empty(), "ACCEL_SUITE_FUNCTION and ACCEL_SUITE_QUERY_SQL are required");
  const std::size_t arity = std::stoul(SuiteEnv("ACCEL_SUITE_ARITY", "1000"));
  const uint64_t seed = std::stoull(SuiteEnv("ACCEL_SUITE_SEED", "1"));
  const int every = std::stoi(SuiteEnv("ACCEL_SUITE_EVERY", "10"));
  const bool stateful = !spec.txns.empty();

  duckdb::DuckDB db(db_path);
  duckdb::Connection wc(db), qc(db);
  if (const auto schema = suite::SchemaSql("ACCEL_SUITE_SCHEMA_SQL"); !schema.empty())
    suite::Q(qc, schema);
  if (!spec.setup.empty())
    suite::Q(qc, spec.setup);
  ::accelerator_codegen::RegisterAcceleratorFunctions(qc);
  const auto ordering = suite::MakeOrdering(qc, query_sql, spec.params);
  engine::duckdb::DuckdbRuntime runtime(wc, entry, 10.0);
  core::ResetAuxExecuteCounts();
  const auto started = std::chrono::steady_clock::now();
  if (stateful)
    runtime.Build();
  const double build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  suite::Tally tally;
  const auto accel = [&](const std::string &, const std::string &tuple) {
    return suite::RunAccel(qc, function, suite::Project(tuple, arity));
  };
  const auto apply = [&](const std::string &sql) { runtime.ApplyWrite(sql); };
  const auto abort = [&](const std::string &sql) {
    runtime.BeginWrite();
    for (auto &statement : wc.ExtractStatements(sql))
      suite::Require(!wc.Query(std::move(statement))->HasError(), "a rollback transaction failed");
    runtime.RollbackWrite();
  };
  const auto maintenance_us = [&] {
    const auto stats = runtime.Stats();
    return stats.apply_us + stats.build_us;
  };
  const auto run = suite::RunDifferentialLoop(tally, spec, query_sql, ordering, qc, every, seed, accel, apply, abort, maintenance_us);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  const auto counts = core::ReadAuxExecuteCounts();
  const bool compared = tally.checks > 0 && tally.reference_errors <= tally.checks;
  const bool ok = compared && run.write_errors == 0 && tally.mismatches + tally.order_mismatches == 0;
  std::string params = "[";
  for (std::size_t i = 0; i < spec.params.size(); ++i)
    params += (i ? "," : "") + suite::Json(spec.params[i]);
  const std::string report = "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"seed\":" + std::to_string(seed) +
                             ",\"params\":" + std::to_string(spec.params.size()) + ",\"param_tuples\":" + params +
                             "],\"txns\":" + std::to_string(spec.txns.size()) + ",\"write_errors\":" +
                             std::to_string(run.write_errors) + ",\"write_error_examples\":" + run.write_error_examples +
                             "],\"build_seconds\":" + std::to_string(build_seconds) + "," + run.CostFields() + ",\"executes\":{\"hit\":" + std::to_string(counts.hit) + ",\"fallback\":" +
                             std::to_string(counts.fallback) + "},\"seconds\":" + std::to_string(seconds) + "," +
                             tally.Fields() + "}";
  if (const auto path = SuiteEnv("ACCEL_SUITE_REPORT"); !path.empty())
    std::ofstream(path) << report << '\n';
  std::cerr << "suite seed=" << seed << " tuples=" << spec.params.size() << " txns=" << spec.txns.size()
            << " checks=" << tally.checks << " mismatches=" << tally.mismatches + tally.order_mismatches
            << " write_errors=" << run.write_errors << " seconds=" << seconds << '\n';
  if (!ok)
    throw std::runtime_error("test suite failed (seed " + std::to_string(seed) + "): " +
                             (tally.examples.empty() ? "see the report" : tally.examples.front()) +
                             (run.write_errors ? " write_errors=" + std::to_string(run.write_errors) : "") +
                             (compared ? "" : " nothing compared"));
}

} // namespace accelerator_codegen::aux_state::harness
