#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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
// Runs read-only, single-probe incremental, then adversarial multi-point + RF.
// Every phase runs on a private copy of <tpch_db_path> in a per-process
// scratch dir (the incremental phase writes and restores, the adversarial one
// writes), so the source DB is never opened for writing and concurrent runs
// do not contend for its lock.
// usage: test_tpch_shared_differential <tpch_db_path> <query_id>
int main(int argc, char **argv) {
  try {
    Require(argc == 3,
            "usage: test_tpch_shared_differential <tpch_db_path> <query_id>");
    const int query_id = std::stoi(argv[2]);
    const auto &entry =
        accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch",
                                                                  query_id);
    namespace ts = accelerator_codegen::test_support;
    ts::ScratchDir scratch("tpch_shared_diff_q" + std::to_string(query_id));
    const std::string work = ts::PrivateDbCopy(argv[1], scratch);
    std::cerr << "differential (read-only): tpch Q" << query_id << '\n';
    accelerator_codegen::aux_state::harness::RunDifferentialCheck(work, entry);
    std::cerr << "PASS tpch Q" << query_id << " read-only differential\n";
    std::cerr << "differential (incremental): tpch Q" << query_id << '\n';
    accelerator_codegen::aux_state::harness::RunIncrementalDifferentialCheck(
        work, entry);
    std::cerr << "PASS tpch Q" << query_id << " incremental differential\n";

    // Fresh copy: the adversarial phase starts from the source DB's state.
    ts::PrivateDbCopy(argv[1], scratch);
    std::cerr << "differential (adversarial writes): tpch Q" << query_id
              << " work=" << work << '\n';
    const bool default_bulk = false;
    accelerator_codegen::aux_state::harness::RunAdversarialWriteDifferentialCheck(
        work, entry, /*max_probes=*/5, /*min_chunks=*/1, default_bulk);
    std::cerr << "PASS tpch Q" << query_id << " adversarial write differential\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_tpch_shared_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
