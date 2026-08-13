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

int main(int argc, char **argv) {
  try {
    Require(argc == 2,
            "usage: test_tpch_q18_shared_differential <tpch_db_path>");
    const auto &entry =
        accelerator_codegen::aux_state::engine::LookupSharedQuery("tpch", 18);
    std::cerr << "differential: tpch Q18\n";
    accelerator_codegen::aux_state::harness::RunDifferentialCheck(argv[1],
                                                                  entry);
    std::cerr << "incremental differential: tpch Q18\n";
    accelerator_codegen::aux_state::harness::RunIncrementalDifferentialCheck(
        argv[1], entry);
    std::cerr << "PASS tpch Q18\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_tpch_q18_shared_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
