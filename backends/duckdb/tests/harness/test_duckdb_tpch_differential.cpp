#include <cstdlib>
#include <iostream>
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

// SF1 parity: DuckDB reference SQL vs aux Execute for one catalog query.
// Runs read-only, single-probe incremental, then adversarial multi-point + RF, then the
// frozen test suite when ACCEL_SUITE_DIR is set.
// Every phase runs on a private copy of <tpch_db_path> in a per-process
// scratch dir (the incremental phase writes and restores, the adversarial one
// writes), so the source DB is never opened for writing and concurrent runs
// do not contend for its lock.
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
    std::cerr << "differential (read-only): tpch Q" << query_id << '\n';
    accelerator_codegen::aux_state::harness::RunDifferentialCheck(work, entry);
    std::cerr << "PASS tpch Q" << query_id << " read-only differential\n";
    std::cerr << "differential (incremental): tpch Q" << query_id << '\n';
    accelerator_codegen::aux_state::harness::RunIncrementalDifferentialCheck(
        work, entry);
    std::cerr << "PASS tpch Q" << query_id << " incremental differential\n";

    // Fresh copy: the adversarial phase starts from the source DB's state.
    ts::PrivateDbCopy(tpch_db, scratch);
    std::cerr << "differential (adversarial writes): tpch Q" << query_id
              << " work=" << work << '\n';
    const bool default_bulk = false;
    accelerator_codegen::aux_state::harness::RunAdversarialWriteDifferentialCheck(
        work, entry, /*max_probes=*/5, /*min_chunks=*/1, default_bulk);
    std::cerr << "PASS tpch Q" << query_id << " adversarial write differential\n";
    if (accelerator_codegen::aux_state::harness::SuiteRequested()) {
      // The frozen test suite (ACCEL_SUITE_*): its tuples and transactions, from the source DB's state.
      ts::PrivateDbCopy(tpch_db, scratch);
      accelerator_codegen::aux_state::harness::RunSuiteDifferential(work, entry);
      std::cerr << "PASS tpch Q" << query_id << " test suite\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_duckdb_tpch_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
