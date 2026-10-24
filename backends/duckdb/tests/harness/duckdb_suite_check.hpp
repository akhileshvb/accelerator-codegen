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
#include "eval/harness/differential.hpp"
#include "eval/harness/suite.hpp"
#include "substrate/aux/runtime/thread_budget.hpp"
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

// The suite (ACCEL_SUITE_*) as the run's environment gives it.
inline suite::Spec LoadSuiteSpec() {
  suite::Spec spec = suite::LoadSpec(SuiteEnv("ACCEL_SUITE_DIR"), "ACCEL_SUITE_REPLACE_PARAMS_FILE", "ACCEL_SUITE_PARAMS_FILE");
  if (const auto txns = SuiteEnv("ACCEL_SUITE_TXNS"); !txns.empty())
    suite::KeepTxns(spec, txns);
  return spec;
}

// The suite's differential on a session (eval/harness/differential.hpp): the fixed tuples plus the run's random ones, after
// the build and after each transaction. The transactions stay committed, so run it after the checks that need the data as it was.
class SuiteCheck {
public:
  // The suite's constraints and setup.sql, on the database before a session opens it (the accelerator is built on them).
  static void PrepareDatabase(const std::string &db_path) {
    duckdb::DuckDB database(db_path);
    duckdb::Connection connection(database);
    if (const auto schema = suite::SchemaSql("ACCEL_SUITE_SCHEMA_SQL"); !schema.empty())
      suite::Q(connection, schema);
    if (const auto setup = LoadSuiteSpec().setup; !setup.empty())
      suite::Q(connection, setup);
  }

  explicit SuiteCheck(DifferentialSession &session)
      : session_(session), query_(session.database), spec_(LoadSuiteSpec()), query_sql_(suite::ReadFile(SuiteEnv("ACCEL_SUITE_QUERY_SQL"))),
        function_(SuiteEnv("ACCEL_SUITE_FUNCTION")), arity_(std::stoul(SuiteEnv("ACCEL_SUITE_ARITY", "1000"))),
        seed_(std::stoull(SuiteEnv("ACCEL_SUITE_SEED", "1"))) {
    suite::Require(!function_.empty() && !query_sql_.empty(), "ACCEL_SUITE_FUNCTION and ACCEL_SUITE_QUERY_SQL are required");
    ::accelerator_codegen::RegisterAcceleratorFunctions(query_);
    ordering_ = suite::MakeOrdering(query_, query_sql_, spec_.params);
  }

  // Throws when the accelerated path disagrees with the query; the report is written either way.
  void Run(double build_seconds) {
    namespace core = ::accelerator_codegen::aux_state::core;
    auto &runtime = session_.runtime;
    build_seconds_ = build_seconds;
    core::ResetAuxExecuteCounts();
    const auto started = std::chrono::steady_clock::now();
    const auto apply = [&](const std::string &sql) { runtime.ApplyWrite(sql); };
    const auto abort = [&](const std::string &sql) {
      runtime.BeginWrite();
      for (auto &statement : session_.connection.ExtractStatements(sql))
        suite::Require(!session_.connection.Query(std::move(statement))->HasError(), "a rollback transaction failed");
      runtime.RollbackWrite();
    };
    const auto maintenance_us = [&] {
      const auto stats = runtime.Stats();
      return stats.apply_us + stats.build_us;
    };
    const auto last_action = [&, stale = runtime.Stats().commits_stale]() mutable {
      const auto stats = runtime.Stats();
      const bool rebuilt = std::exchange(stale, stats.commits_stale) != stats.commits_stale;
      if (rebuilt)
        return std::string("RebuildAll");
      return stats.last_decision ? std::string(maintenance::MaintenanceActionName(stats.last_decision->staged_action))
                                 : std::string("module");
    };
    run_ = suite::RunDifferentialLoop(tally_, spec_, query_sql_, ordering_, query_, std::stoi(SuiteEnv("ACCEL_SUITE_EVERY", "10")), seed_,
                                      [&](const std::string &, const std::string &tuple) { return Answer(tuple); }, apply, abort,
                                      maintenance_us, last_action);
    seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    counts_ = core::ReadAuxExecuteCounts();
    ok_ = tally_.checks > 0 && tally_.reference_errors <= tally_.checks && run_.write_errors == 0 &&
          tally_.mismatches + tally_.order_mismatches == 0;
    WriteReport();
    std::cerr << "suite seed=" << seed_ << " tuples=" << spec_.params.size() << " txns=" << spec_.txns.size()
              << " checks=" << tally_.checks << " mismatches=" << tally_.mismatches + tally_.order_mismatches
              << " write_errors=" << run_.write_errors << " seconds=" << seconds_ << '\n';
    if (!ok_)
      throw std::runtime_error("test suite failed (seed " + std::to_string(seed_) + "): " +
                               (tally_.examples.empty() ? "see the report" : tally_.examples.front()) +
                               (run_.write_errors ? " write_errors=" + std::to_string(run_.write_errors) : "") +
                               (tally_.checks > 0 ? "" : " nothing compared"));
  }

  // The accelerated answer to every tuple on the data as it stands, one after the other (the drift check's input).
  suite::Rows Answers() {
    suite::Rows all;
    for (const auto &tuple : spec_.params) {
      auto rows = Answer(tuple);
      all.insert(all.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
      all.push_back({"--"});
    }
    return all;
  }

  // ACCEL_SUITE_REPORT, if set: the run's counts, write costs and, so far, the seconds of each phase.
  void WriteReport() const {
    const auto path = SuiteEnv("ACCEL_SUITE_REPORT");
    if (path.empty())
      return;
    std::string params = "[";
    for (std::size_t i = 0; i < spec_.params.size(); ++i)
      params += (i ? "," : "") + suite::Json(spec_.params[i]);
    const std::size_t budget = runtime::ResolveThreadBudget(0);
    std::ofstream(path) << "{\"ok\":" << (ok_ ? "true" : "false") << ",\"seed\":" << seed_
                        << ",\"params\":" << spec_.params.size() << ",\"param_tuples\":" << params
                        << "],\"txns\":" << spec_.txns.size() << ",\"write_errors\":" << run_.write_errors
                        << ",\"write_error_examples\":" << run_.write_error_examples
                        << "],\"threads\":" << budget << ",\"build_threads\":" << runtime::ResolveBuildThreads(0, budget)
                        << ",\"build_seconds\":" << build_seconds_ << "," << run_.CostFields() << "," << suite::PhaseFields()
                        << ",\"executes\":{\"hit\":" << counts_.hit << ",\"fallback\":" << counts_.fallback
                        << "},\"seconds\":" << seconds_ << "," << tally_.Fields() << "}\n";
  }

private:
  suite::Rows Answer(const std::string &tuple) {
    return suite::RunAccel(query_, function_, suite::Project(tuple, arity_));
  }

  DifferentialSession &session_;
  duckdb::Connection query_;
  suite::Spec spec_;
  std::string query_sql_, function_;
  std::size_t arity_ = 0;
  uint64_t seed_ = 1;
  suite::Ordering ordering_;
  suite::Tally tally_;
  suite::DifferentialRun run_;
  ::accelerator_codegen::aux_state::core::AuxExecuteCounts counts_;
  double build_seconds_ = 0, seconds_ = 0;
  bool ok_ = false;
};

} // namespace accelerator_codegen::aux_state::harness
