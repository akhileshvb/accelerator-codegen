#include "backends/duckdb/tests/harness/common.hpp"

#ifndef ACCEL_QUERY_NUMBER
#define ACCEL_QUERY_NUMBER 0
#endif

namespace h = accelerator_codegen::duckdb_harness;
namespace fs = std::filesystem;

int main(int argc, char **argv) {
  try {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
      std::cout << "Usage: test_<benchmark>_q<query>_aux\n";
      return 0;
    }
    const auto &entry = h::Lookup(ACCEL_QUERY_NUMBER);
    const auto parameter_tuples = h::WorkloadSuiteTuples(
        h::Benchmark(), ACCEL_QUERY_NUMBER, h::ArgumentCount(entry));
    const auto source = h::DbPath();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = h::TempDirectory() / ("accel_module_test_" + std::to_string(stamp) + ".duckdb");
    fs::copy_file(source, path, fs::copy_options::overwrite_existing);
    auto database = h::OpenDatabase(path.string());
    auto connection = h::Connect(*database);
    for (const auto &parameters : parameter_tuples) {
      h::Compare(*connection, entry, parameters, "module build answer");
      h::Compare(*connection, entry, parameters, "module repeated answer");
    }
    if (entry.probe_select_sql && *entry.probe_select_sql && entry.build_probe) {
      auto probe = accelerator_codegen::aux_state::engine::FindSharedProbe(*connection, entry);
      h::Exec(*connection, accelerator_codegen::aux_state::engine::ProbeWriteSql(probe));
      for (const auto &parameters : parameter_tuples)
        h::Compare(*connection, entry, parameters, "module point write");
      if (!probe.restore_sql.empty()) {
        h::Exec(*connection, probe.restore_sql);
        for (const auto &parameters : parameter_tuples)
          h::Compare(*connection, entry, parameters, "module restored state");
      }
    }
    database.reset();
    fs::remove(path);
    fs::remove(path.string() + ".wal");
    std::cout << "module smoke test passed for " << entry.workload_name << " query " << entry.query_id << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "module smoke test failed: " << error.what() << '\n';
    return 1;
  }
}
