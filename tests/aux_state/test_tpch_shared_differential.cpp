#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "accelerator_codegen/harness/differential.hpp"
#include "catalog/query_catalog.hpp"

namespace {

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

// SF1 parity: DuckDB reference SQL vs aux Execute for one catalog query.
// usage: test_tpch_shared_differential <tpch_db_path> <query_id>
int main(int argc, char **argv) {
  try {
    Require(argc == 3,
            "usage: test_tpch_shared_differential <tpch_db_path> <query_id>");
    const int query_id = std::stoi(argv[2]);
    const auto &entry =
        accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch",
                                                                  query_id);
    std::cerr << "differential: tpch Q" << query_id << '\n';
    accelerator_codegen::aux_state::harness::RunDifferentialCheck(argv[1],
                                                                  entry);
    std::cerr << "PASS tpch Q" << query_id << " shared differential\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_tpch_shared_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
