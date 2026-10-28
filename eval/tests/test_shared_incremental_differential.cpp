#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/duckdb/api/sql_fallback.hpp"
#include "eval/harness/differential.hpp"
#include "eval/harness/param_sets.hpp"
#include "eval/tests/test_support/db_paths.hpp"
#include "catalog/query_catalog.hpp"

#include "duckdb.hpp"

namespace {

namespace engine = accelerator_codegen::aux_state::engine;

constexpr int kSkipReturnCode = 77; // ctest SKIP_RETURN_CODE

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// Whether the DB has a row for the query's catalog probe select under `arguments`: the
// incremental check needs one to invalidate the query. A test DB built
// without that query's seed rows has none (a data gap, not a code bug).
bool HasProbeRow(const std::string &db_path, const engine::SharedQueryCatalogEntry &entry,
                 const accelerator_codegen::aux_state::core::QueryArguments &arguments) {
  duckdb::DuckDB database(db_path);
  duckdb::Connection connection(database);
  auto result = connection.Query(
      engine::BindArguments(engine::ExpandProbeSelectLimit(entry.probe_select_sql, 1), arguments));
  Require(result != nullptr && !result->HasError(),
          "probe select failed for " + std::string(entry.workload_name) + " Q" +
              std::to_string(entry.query_id) + ": " + (result ? result->GetError() : "null"));
  return result->Cast<duckdb::MaterializedQueryResult>().RowCount() > 0;
}

// Runs every query of `workload`; returns the ids skipped for lack of probe rows.
std::vector<int> RunWorkload(const std::string &db_path, engine::SharedWorkloadKind workload) {
  std::vector<int> no_probe_rows;
  for (const int query_id : engine::SharedQueriesForWorkload(workload)) {
    const auto &entry = engine::LookupSharedQuery(workload, query_id);
    // TPC-H: the DB is generated with every query's seeds, so a missing probe
    // row is a real failure (left to the check). SEC-EDGAR: the test DB is a
    // sample of an external dataset that may predate a query's probe, and a drawn
    // parameter set may select none of it: such sets are skipped, and the query
    // is a gap when no set has a probe row.
    int ran = 0;
    for (const auto &arguments : accelerator_codegen::aux_state::harness::ArgumentSets(entry)) {
      if (workload == engine::SharedWorkloadKind::SecEdgar && !HasProbeRow(db_path, entry, arguments)) {
        continue;
      }
      std::cerr << "incremental differential: " << entry.workload_name << " Q" << entry.query_id << " ("
                << accelerator_codegen::aux_state::harness::ArgumentLiterals(arguments) << ")\n";
      accelerator_codegen::aux_state::harness::RunIncrementalDifferentialCheck(db_path, entry, arguments);
      ++ran;
    }
    if (ran == 0) {
      std::cerr << "NO PROBE ROWS " << entry.workload_name << " Q" << entry.query_id << '\n';
      no_probe_rows.push_back(query_id);
      continue;
    }
    std::cerr << "PASS " << entry.workload_name << " Q" << entry.query_id << " (" << ran << " parameter sets)\n";
  }
  return no_probe_rows;
}

} // namespace

// Incremental differential (one invalidating probe, applied then restored)
// for every shared TPC-H and SEC-EDGAR query, on the small test databases.
//
// usage: test_shared_incremental_differential [tpch_test_db|-] [sec_edgar_test_db|-]
//   defaults: ACCEL_TPCH_TEST_DB / ACCEL_SEC_EDGAR_TEST_DB (test_support);
//   "-" skips that workload.
// ctest runs the two workloads as separate tests. The TPC-H one gets a
// tpch_test DB freshly built from workloads/tpch/test_db_subset.sql (fixture
// make_tpch_test_db), whose seeds give every query's probe_select a row.
// Both DBs are copied into a private scratch dir first: the probes write, and
// the source files are shared with other runs.
//
// Exit 77 (skip) when a DB is missing, or when every query passed except
// SEC-EDGAR queries the SEC test DB has no probe rows for: the DB lacks their
// seed data (ctest rebuilds it from SF3 via the sec_edgar_test_db fixture; or run scripts/create_sec_edgar_test_db.py).
int main(int argc, char **argv) {
  namespace ts = accelerator_codegen::test_support;
  try {
    Require(argc <= 3,
            "usage: test_shared_incremental_differential "
            "[tpch_test_db|-] [sec_edgar_test_db|-]");
    const std::string tpch_db = argc > 1 ? std::string(argv[1]) : ts::TpchTestDbPath();
    const std::string sec_db = argc > 2 ? std::string(argv[2]) : ts::SecEdgarTestDbPath();
    struct Run {
      std::string db;
      const char *env;
      engine::SharedWorkloadKind workload;
    };
    const Run runs[] = {{tpch_db, "ACCEL_TPCH_TEST_DB", engine::SharedWorkloadKind::Tpch},
                        {sec_db, "ACCEL_SEC_EDGAR_TEST_DB", engine::SharedWorkloadKind::SecEdgar}};
    for (const auto &run : runs) {
      if (run.db != "-" && !std::filesystem::exists(run.db)) {
        std::cerr << "SKIP: test database " << run.db << " does not exist (set " << run.env << ")\n";
        return kSkipReturnCode;
      }
    }
    ts::ScratchDir scratch("shared_incremental_diff");
    std::string gaps;
    for (const auto &run : runs) {
      if (run.db == "-") {
        continue;
      }
      for (const int query_id : RunWorkload(ts::PrivateDbCopy(run.db, scratch), run.workload)) {
        gaps += " " + std::string(run.workload == engine::SharedWorkloadKind::Tpch ? "tpch" : "sec_edgar") +
                " Q" + std::to_string(query_id) + " (" + run.db + ")";
      }
    }
    if (!gaps.empty()) {
      std::cerr << "SKIP: all other queries passed, but the test DB has no row for the catalog "
                   "probe_select of:"
                << gaps
                << "\n  The DB lacks those queries' seed data; regenerate it with "
                   "scripts/create_sec_edgar_test_db.py from a populated SEC-EDGAR DB.\n";
      return kSkipReturnCode;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_shared_incremental_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
