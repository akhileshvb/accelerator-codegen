#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "backends/duckdb/tests/harness/duckdb_suite_check.hpp"
#include "eval/harness/differential.hpp"
#include "eval/tests/test_support/db_paths.hpp"
#include "catalog/query_catalog.hpp"

namespace {

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

// SF1 parity: DuckDB reference SQL vs aux Execute for one catalog query, on one private copy of <tpch_db_path> with
// one module build for every phase (eval/harness/differential.hpp): read-only, a single-probe incremental write,
// the adversarial probes (each undone through the runtime), the frozen test suite when ACCEL_SUITE_DIR is set, the
// refresh chunks when the suite requires bulk writes, and last the one from-scratch rebuild, which must answer as
// the maintained state did. ACCEL_DIFF_PHASES (comma-separated: read_only, incremental, adversarial, suite,
// refresh, drift) runs only those; build and the seconds of each phase are printed and in the suite report.
// The source DB is never opened for writing and concurrent runs do not contend for its lock.
// usage: test_duckdb_tpch_differential <query_id>   (database: ACCEL_TPCH_DB)
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "usage: test_duckdb_tpch_differential <query_id>   (database: ACCEL_TPCH_DB)\n";
      return 0;
    }
    Require(argc == 2, "usage: test_duckdb_tpch_differential <query_id>");
    const std::string tpch_db = accelerator_codegen::test_support::TpchDbPath();
    const int query_id = std::stoi(argv[1]);
    const auto &entry =
        accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch",
                                                                  query_id);
    namespace ts = accelerator_codegen::test_support;
    ts::ScratchDir scratch("tpch_shared_diff_q" + std::to_string(query_id));
    const std::string work = ts::PrivateDbCopy(tpch_db, scratch);
    if (accelerator_codegen::aux_state::harness::SuiteRequested() &&
        accelerator_codegen::aux_state::harness::SuiteEnv("ACCEL_SUITE_MODE") == "validate") {
      return accelerator_codegen::aux_state::harness::RunSuiteValidation(work) ? 0 : 1;
    }
    namespace harness = accelerator_codegen::aux_state::harness;
    namespace suite = accelerator_codegen::suite;
    const auto wants = [phases = harness::SuiteEnv("ACCEL_DIFF_PHASES")](const std::string &phase) {
      return phases.empty() || ("," + phases + ",").find("," + phase + ",") != std::string::npos;
    };
    const auto plan = harness::AdversarialPlanFromEnv(/*max_probes=*/5, /*min_chunks=*/1, /*require_bulk=*/false);
    const bool with_suite = harness::SuiteRequested() && wants("suite");
    if (with_suite)
      harness::SuiteCheck::PrepareDatabase(work);
    harness::DifferentialSession session(work, entry);
    std::optional<harness::SuiteCheck> suite_check;
    if (with_suite)
      suite_check.emplace(session);
    std::cerr << "differential: tpch Q" << query_id << " work=" << work << '\n';
    {
      suite::Phase phase("build");
      session.runtime.Build();
    }
    const double build_seconds = suite::PhaseSeconds().back().second;
    const auto run = [&](const std::string &name, const auto &check) {
      if (!wants(name))
        return;
      suite::Phase phase(name);
      check();
      std::cerr << "PASS tpch Q" << query_id << ' ' << name << '\n';
    };
    run("read_only", [&] { harness::CheckReadOnly(session); });
    run("incremental", [&] { harness::CheckIncrementalWrite(session); });
    run("adversarial", [&] { harness::CheckAdversarialWrites(session, plan); });
    if (suite_check)
      run("suite", [&] { suite_check->Run(build_seconds); });
    run("refresh", [&] { harness::CheckRefreshWrites(session, plan); });
    run("drift", [&] {
      if (suite_check)
        harness::CheckNoDrift(session, [&] { return suite_check->Answers(); });
      else
        harness::CheckNoDrift(session);
    });
    if (suite_check)
      suite_check->WriteReport();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_duckdb_tpch_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
