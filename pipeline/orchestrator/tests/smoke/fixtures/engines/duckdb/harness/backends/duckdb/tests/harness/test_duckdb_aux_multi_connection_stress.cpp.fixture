#include "backends/duckdb/tests/harness/common.hpp"

#include <atomic>

namespace h = accelerator_codegen::duckdb_harness;
namespace fs = std::filesystem;

namespace {
std::string PrivateCopy(const std::string &source) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dest = h::TempDirectory() / ("accel_multiconn_" + std::to_string(stamp) + ".duckdb");
  fs::copy_file(source, dest, fs::copy_options::overwrite_existing);
  if (fs::exists(source + ".wal")) fs::copy_file(source + ".wal", dest.string() + ".wal");
  return dest.string();
}
}

int main(int argc, char **argv) {
  try {
    const int query = h::QueryArgument(argc, argv, "test_duckdb_aux_multi_connection_stress <query>");
    if (query == 0) return 0;
    const auto &entry = h::Lookup(query);
    const auto path = PrivateCopy(h::DbPath());
    auto database = h::OpenDatabase(path);
    const int readers = std::max(1, std::stoi(std::getenv("ACCEL_MULTI_CONN_TARGET_READERS")
                                                  ? std::getenv("ACCEL_MULTI_CONN_TARGET_READERS") : "3"));
    const int seconds = std::max(1, std::stoi(std::getenv("ACCEL_MULTI_CONN_STRESS_SEC")
                                                 ? std::getenv("ACCEL_MULTI_CONN_STRESS_SEC") : "3"));
    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> completed{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < readers; ++i) {
      workers.emplace_back([&] {
        auto connection = h::Connect(*database);
        while (!stop.load(std::memory_order_relaxed)) {
          connection->BeginTransaction();  // one snapshot for the reference and the accelerated read, beside the writer
          h::Compare(*connection, entry, {}, "concurrent reader");
          connection->Commit();
          completed.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    std::thread writer([&] {
      auto connection = h::Connect(*database);
      while (!stop.load(std::memory_order_relaxed)) {
        if (entry.probe_select_sql && *entry.probe_select_sql && entry.build_probe) {
          auto probe = accelerator_codegen::aux_state::engine::FindSharedProbe(*connection, entry);
          const auto sql = accelerator_codegen::aux_state::engine::ProbeWriteSql(probe);
          h::Exec(*connection, sql);
          if (!probe.restore_sql.empty()) h::Exec(*connection, probe.restore_sql);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (auto &worker : workers) worker.join();
    h::Require(completed.load() > 0, "no concurrent read completed");
    std::cout << "multi-connection stress passed: " << completed.load() << " reads, " << readers
              << " readers, " << seconds << " seconds\n";
    database.reset();
    fs::remove(path);
    fs::remove(path + ".wal");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "multi-connection stress failed: " << error.what() << '\n';
    return 1;
  }
}
