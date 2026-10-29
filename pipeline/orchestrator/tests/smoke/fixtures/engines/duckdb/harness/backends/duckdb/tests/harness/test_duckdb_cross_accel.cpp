#include "backends/duckdb/tests/harness/common.hpp"

namespace h = accelerator_codegen::duckdb_harness;

namespace {
std::string PrivateCopy(const std::string &source) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dest = h::TempDirectory() / ("accel_crossaccel_" + std::to_string(stamp) + ".duckdb");
  std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing);
  if (std::filesystem::exists(source + ".wal")) std::filesystem::copy_file(source + ".wal", dest.string() + ".wal");
  return dest.string();
}
}

int main(int argc, char **argv) {
  try {
    const int query = h::QueryArgument(argc, argv, "test_duckdb_cross_accel <query>");
    if (query == 0) return 0;
    const auto &entry = h::Lookup(query);
    const auto path = PrivateCopy(h::DbPath());
    auto database = h::OpenDatabase(path);
    auto connection = h::Connect(*database);
    h::Compare(*connection, entry, {}, "target accelerator");
    std::size_t registered = 0;
    for (const auto *aux : accelerator_codegen::aux_state::engine::duckdb::AuxQueries()) {
      if (!aux || !aux->make_binding) continue;
      const auto *candidate = accelerator_codegen::aux_state::engine::FindSharedQueryByAccelerator(aux->accelerator_id);
      if (!candidate) continue;
      try {
        h::Query(*connection, h::SqlPair(*candidate).second);
        ++registered;
      } catch (const std::exception &) {
        h::Compare(*connection, entry, {}, "target survives another accelerator failure");
      }
    }
    h::Compare(*connection, entry, {}, "target survives cross-accelerator checks");
    std::cout << "cross-accelerator isolation passed: " << registered << " registered accelerators checked\n";
    database.reset();
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cross-accelerator test failed: " << error.what() << '\n';
    return 1;
  }
}
