// PostgreSQL multi-connection aux stress: concurrent readers + a writer, then
// SQL-vs-aux rematch. Builds: test_postgres_aux_multi_connection_stress
//
// Env: ACCEL_MULTI_CONN_TEST_QUERIES, ACCEL_MULTI_CONN_TARGET_READERS,
//      ACCEL_MULTI_CONN_BACKGROUND_READERS, ACCEL_MULTI_CONN_STRESS_SEC,
//      TPCH_PG_DB, ACCEL_PG_DOCKER=1

#include "aux_state/postgres_tpch_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace accel::postgres::tpch_harness;

int EnvInt(const char *name, int fallback) {
  const char *raw = std::getenv(name);
  if (!raw || !*raw)
    return fallback;
  return std::stoi(raw);
}

void ReaderLoop(const Tools &tools, int query_num, std::atomic<bool> *stop,
                std::atomic<int> *ok, std::atomic<int> *fail) {
  const auto &entry = LookupPostgresQuery("tpch", query_num);
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

    std::cerr << "postgres multi-conn: target Q" << target << '\n';
    RequireQueryMatch(tools, target, /*prefer_aux=*/true, "pre_stress");

    auto snap = QuerySQL(tools,
                         "SELECT l_orderkey::text || '|' || l_linenumber::text || '|' || "
                         "l_extendedprice::text FROM lineitem "
                         "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
                         "ORDER BY l_orderkey, l_linenumber LIMIT 1");
    Require(snap.first && !TrimLines(snap.second).empty(),
            "could not snapshot lineitem row for restore");

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
      const int q = queries[static_cast<size_t>(i) % queries.size()];
      threads.emplace_back(ReaderLoop, tools, q, &stop, &ok, &fail);
    }
    threads.emplace_back(WriterLoop, tools, target, &stop, &writes);

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true, std::memory_order_relaxed);
    for (auto &t : threads)
      t.join();

    Require(fail.load() == 0,
            "multi-connection stress had query failures: " +
                std::to_string(fail.load()));
    Require(ok.load() > 0, "multi-connection stress produced no successful reads");
    Require(writes.load() > 0, "multi-connection stress produced no writes");

    RequireQueryMatch(tools, target, /*prefer_aux=*/true, "post_stress");

    auto fields = SplitLines(TrimLines(snap.second));
    if (!fields.empty()) {
      auto cells = std::vector<std::string>();
      std::string cur;
      for (char c : fields[0]) {
        if (c == '|') {
          cells.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      cells.push_back(cur);
      if (cells.size() >= 3) {
        RunSQL(tools, "UPDATE lineitem SET l_extendedprice = " + cells[2] +
                          " WHERE l_orderkey = " + cells[0] +
                          " AND l_linenumber = " + cells[1]);
      }
    }
    std::cout << "test_postgres_aux_multi_connection_stress passed for Q" << target
              << " reads=" << ok.load() << " writes=" << writes.load() << std::endl;
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "test_postgres_aux_multi_connection_stress failed: " << ex.what()
              << std::endl;
    return EXIT_FAILURE;
  }
}
