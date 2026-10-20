// MySQL multi-connection aux stress: concurrent readers + a writer, then
// SQL-vs-aux rematch. Builds: test_mysql_aux_multi_connection_stress

#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace accel::mysql::tpch_harness;

int EnvInt(const char *name, int fallback) {
  const char *raw = std::getenv(name);
  if (!raw || !*raw)
    return fallback;
  return std::stoi(raw);
}

void ReaderLoop(const Tools &tools, int query_num, std::atomic<bool> *stop,
                std::atomic<int> *ok, std::atomic<int> *fail) {
  const auto &entry = LookupMysqlQuery("tpch", query_num);
  const std::string sql = ConcreteAccelSql(query_num, entry, /*prefer_aux=*/true);
  while (!stop->load(std::memory_order_relaxed)) {
    auto r = QuerySQL(tools, sql);
    if (r.first)
      ok->fetch_add(1, std::memory_order_relaxed);
    else
      fail->fetch_add(1, std::memory_order_relaxed);
  }
}

void WriterLoop(const Tools &tools, int query_num, std::atomic<bool> *stop,
                std::atomic<int> *writes) {
  const std::string sql = InvalidatingUpdateSql(query_num);
  while (!stop->load(std::memory_order_relaxed)) {
    if (RunSQL(tools, sql))
      writes->fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

} // namespace

int main() {
  try {
    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);

    const auto queries = ParseQueryList("ACCEL_MULTI_CONN_TEST_QUERIES", {1});
    Require(!queries.empty(), "no target queries configured");
    const int target = queries.front();

    std::cerr << "mysql multi-conn: target Q" << target << '\n';
    RequireQueryMatch(tools, target, /*prefer_aux=*/true, "pre_stress");

    const int target_readers = EnvInt("ACCEL_MULTI_CONN_TARGET_READERS", 3);
    const int background_readers = EnvInt("ACCEL_MULTI_CONN_BACKGROUND_READERS", 2);
    const int duration_sec = EnvInt("ACCEL_MULTI_CONN_STRESS_SEC", 5);

    std::atomic<bool> stop{false};
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    std::atomic<int> writes{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < target_readers; ++i) {
      threads.emplace_back(ReaderLoop, tools, target, &stop, &ok, &fail);
    }
    for (int i = 0; i < background_readers; ++i) {
      const int peer = target == 1 ? 3 : 1;
      threads.emplace_back(ReaderLoop, tools, peer, &stop, &ok, &fail);
    }
    threads.emplace_back(WriterLoop, tools, target, &stop, &writes);

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true, std::memory_order_relaxed);
    for (auto &t : threads)
      t.join();

    std::cerr << "stress ok=" << ok.load() << " fail=" << fail.load()
              << " writes=" << writes.load() << '\n';
    Require(fail.load() == 0, "reader failures during multi-conn stress");
    Require(ok.load() > 0, "no successful reader queries");
    RequireQueryMatch(tools, target, /*prefer_aux=*/true, "post_stress");
    std::cout << "test_mysql_aux_multi_connection_stress passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "test_mysql_aux_multi_connection_stress failed: " << ex.what()
              << std::endl;
    return EXIT_FAILURE;
  }
}
