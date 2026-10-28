#include "eval/harness/cost_calibration.hpp"
#include "eval/harness/param_sets.hpp"
#include "backends/duckdb/api/shared_query_engine.hpp"

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
      << "  output_json defaults to accelerators/<workload>/q<query_id>/config/<workload>_q<query_id>_aux_cost_model.json\n"
      << "   or: " << program << " verus <accelerator_id> <duckdb_path> <output_json> [params]\n"
      << "  a Verus kit (tpch_q6_vgen, ...), read with `params` (one SQL tuple, as in its\n"
      << "  tests' params.sql); set ACCEL_AUX_STORE=verus for the Verus store\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 4) {
      Usage(argv[0]);
      return EXIT_FAILURE;
    }

    const std::string workload = argv[1];
    const std::string db_path = argv[3];

    using namespace accelerator_codegen::aux_state;
    using namespace accelerator_codegen::aux_state::harness;
    using namespace accelerator_codegen::aux_state::engine;
    ::duckdb::DuckDB database(db_path);
    ::duckdb::Connection connection(database);
    SharedQueryCatalogEntry entry;
    std::string out_path;
    if (workload == "verus") {
      // The cost model it loads and refines is the one the runtime reads: point
      // ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL at output_json.
      const auto *kit = VerusKitCatalogEntry(argv[2]);
      if (!kit || argc < 5)
        throw std::runtime_error(std::string("no Verus kit ") + argv[2] + " (or no output_json)");
      entry = *kit;
      out_path = argv[4];
      if (argc > 5) {
        auto row = connection.Query(std::string("SELECT ") + argv[5]);
        if (!row || row->HasError() || row->RowCount() != 1)
          throw std::runtime_error(std::string("params: ") + (row ? row->GetError() : argv[5]));
        std::vector<::duckdb::Value> values;
        for (::duckdb::idx_t c = 0; c < row->ColumnCount(); ++c)
          values.push_back(row->GetValue(c, 0));
        entry.benchmark_arguments.values = AuxArgumentsFromValues(entry, values).values;
      }
    } else {
      const int query_id = std::stoi(argv[2]);
      entry = LookupSharedQuery(workload, query_id);
      out_path = argc > 4 ? argv[4] : SharedCostModelConfigPath(entry);
    }

    core::AuxCalibrationConfig config;
    if (const char *samples = std::getenv("ACCEL_AUX_CALIBRATION_SAMPLES")) {
      config.measurement_samples = static_cast<std::size_t>(std::stoul(samples));
    }
    if (const char *warmups = std::getenv("ACCEL_AUX_CALIBRATION_WARMUPS")) {
      config.warmup_samples = static_cast<std::size_t>(std::stoul(warmups));
    }

    const auto result = CalibrateSharedQueryCostModel(connection, entry, ArgumentSets(entry).front(), config);
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
