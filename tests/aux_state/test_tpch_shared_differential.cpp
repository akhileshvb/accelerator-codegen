#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "accelerator_codegen/harness/differential.hpp"
#include "catalog/query_catalog.hpp"

namespace {

namespace fs = std::filesystem;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void CopyDb(const fs::path &src, const fs::path &dst) {
  Require(fs::exists(src), "missing source DB: " + src.string());
  Require(src != dst, "work DB path must differ from source DB");
  fs::create_directories(dst.parent_path().empty() ? fs::path(".")
                                                   : dst.parent_path());
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

} // namespace

// SF1 parity: DuckDB reference SQL vs aux Execute for one catalog query.
// Runs read-only, single-probe incremental, then adversarial multi-point + RF.
// usage: test_tpch_shared_differential <tpch_db_path> <query_id>
int main(int argc, char **argv) {
  try {
    Require(argc == 3,
            "usage: test_tpch_shared_differential <tpch_db_path> <query_id>");
    const int query_id = std::stoi(argv[2]);
    const auto &entry =
        accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch",
                                                                  query_id);
    std::cerr << "differential (read-only): tpch Q" << query_id << '\n';
    accelerator_codegen::aux_state::harness::RunDifferentialCheck(argv[1],
                                                                  entry);
    std::cerr << "PASS tpch Q" << query_id << " read-only differential\n";
    std::cerr << "differential (incremental): tpch Q" << query_id << '\n';
    accelerator_codegen::aux_state::harness::RunIncrementalDifferentialCheck(
        argv[1], entry);
    std::cerr << "PASS tpch Q" << query_id << " incremental differential\n";

    const fs::path work =
        fs::temp_directory_path() /
        ("tpch_adv_diff_q" + std::to_string(query_id) + ".duckdb");
    CopyDb(argv[1], work);
    std::cerr << "differential (adversarial writes): tpch Q" << query_id
              << " work=" << work << '\n';
    const bool default_bulk = false;
    accelerator_codegen::aux_state::harness::RunAdversarialWriteDifferentialCheck(
        work.string(), entry, /*max_probes=*/5, /*min_chunks=*/1, default_bulk);
    std::cerr << "PASS tpch Q" << query_id << " adversarial write differential\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_tpch_shared_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
