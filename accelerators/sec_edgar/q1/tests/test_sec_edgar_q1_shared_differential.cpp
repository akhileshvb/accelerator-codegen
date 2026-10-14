// SF-scale parity for SEC EDGAR Q1: read-only, single-probe incremental, and
// adversarial multi-probe write differentials (single and parallel accelerator).
// usage: test_sec_edgar_q1_shared_differential <sec_edgar_db_path> <work_db_copy>
// <work_db_copy> is overwritten with a copy of the source (probes mutate it).
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "eval/harness/differential.hpp"
#include "catalog/query_catalog.hpp"

namespace fs = std::filesystem;
namespace harness = accelerator_codegen::aux_state::harness;
namespace engine = accelerator_codegen::aux_state::engine;

static void RunAll(const engine::SharedQueryCatalogEntry &entry, const std::string &src, const std::string &work) {
  std::cerr << "q1 differential: " << entry.accelerator_id << "\n";
  harness::RunDifferentialCheck(src, entry);
  harness::RunIncrementalDifferentialCheck(work, entry);
  harness::RunAdversarialWriteDifferentialCheck(work, entry, /*max_probes=*/5, /*min_chunks=*/1, false);
  std::cerr << "PASS " << entry.accelerator_id << "\n";
}

int main(int argc, char **argv) {
  try {
    if (argc != 3) {
      throw std::runtime_error("usage: test_sec_edgar_q1_shared_differential <db> <work_db_copy>");
    }
    if (fs::path(argv[1]) == fs::path(argv[2])) {
      throw std::runtime_error("work DB must differ from source DB");
    }
    fs::copy_file(argv[1], argv[2], fs::copy_options::overwrite_existing);
    RunAll(engine::LookupSharedQuery("sec_edgar", 1), argv[1], argv[2]);
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    RunAll(engine::LookupSharedQueryParallel("sec_edgar", 1), argv[1], argv[2]);
#endif
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_sec_edgar_q1_shared_differential failed: " << error.what() << "\n";
    return 1;
  }
}
