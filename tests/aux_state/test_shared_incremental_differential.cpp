#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "accelerator_codegen/aux_state/harness/differential.hpp"
#include "accelerator_codegen/aux_state/engine/shared/query_catalog.hpp"

namespace {

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void RunWorkload(const std::string &db_path,
                 accelerator_codegen::aux_state::engine::SharedWorkloadKind workload) {
  for (const int query_id :
       accelerator_codegen::aux_state::engine::SharedQueriesForWorkload(
           workload)) {
    const auto &entry =
        accelerator_codegen::aux_state::engine::LookupSharedQuery(workload,
                                                                 query_id);
    std::cerr << "incremental differential: " << entry.workload_name << " Q"
              << entry.query_id << '\n';
    accelerator_codegen::aux_state::harness::RunIncrementalDifferentialCheck(
        db_path, entry);
    std::cerr << "PASS " << entry.workload_name << " Q" << entry.query_id
              << '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc == 3,
            "usage: test_shared_incremental_differential "
            "<tpch_db_path> <sec_edgar_db_path>");
    RunWorkload(argv[1],
                accelerator_codegen::aux_state::engine::SharedWorkloadKind::Tpch);
    RunWorkload(
        argv[2],
        accelerator_codegen::aux_state::engine::SharedWorkloadKind::SecEdgar);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_shared_incremental_differential failed: " << error.what()
              << '\n';
    return 1;
  }
}
