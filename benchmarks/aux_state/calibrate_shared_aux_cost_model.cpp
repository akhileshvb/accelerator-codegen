#include "accelerator_codegen/harness/cost_calibration.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

void Usage(const char *program) {
  std::cerr
      << "usage: " << program
      << " <workload> <query_id> <duckdb_path> [output_json]\n"
      << "  workload: tpch | sec_edgar\n"
      << "  output_json defaults to config/<workload>_q<query_id>_aux_cost_model.json\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 4) {
      Usage(argv[0]);
      return EXIT_FAILURE;
    }

    const std::string workload = argv[1];
    const int query_id = std::stoi(argv[2]);
    const std::string db_path = argv[3];

    using namespace accelerator_codegen::aux_state;
    using namespace accelerator_codegen::aux_state::harness;
    using namespace accelerator_codegen::aux_state::engine;
    const auto &entry = LookupSharedQuery(workload, query_id);
    const std::string out_path =
        argc > 4 ? argv[4] : SharedCostModelConfigPath(entry.workload, query_id);

    core::AuxCalibrationConfig config;
    if (const char *samples = std::getenv("ACCEL_AUX_CALIBRATION_SAMPLES")) {
      config.measurement_samples = static_cast<std::size_t>(std::stoul(samples));
    }
    if (const char *warmups = std::getenv("ACCEL_AUX_CALIBRATION_WARMUPS")) {
      config.warmup_samples = static_cast<std::size_t>(std::stoul(warmups));
    }

    duckdb::DuckDB database(db_path);
    duckdb::Connection connection(database);
    const auto result = CalibrateSharedQueryCostModel(connection, entry, config);
    WriteSharedQueryCostModel(out_path, result, entry);

    std::cout << "calibrate_shared_aux_cost_model wrote " << out_path << '\n'
              << "  aux_hit_us=" << result.params.aux_hit_us << '\n'
              << "  fallback_us=" << result.params.fallback_us << '\n'
              << "  partition_rebuild_us=" << result.params.partition_rebuild_us
              << '\n'
              << "  full_rebuild_us=" << result.params.full_rebuild_us << '\n'
              << "  apply_delta_per_row_us="
              << result.params.apply_delta_per_row_us << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "calibrate_shared_aux_cost_model failed: " << ex.what()
              << std::endl;
    return EXIT_FAILURE;
  }
}
