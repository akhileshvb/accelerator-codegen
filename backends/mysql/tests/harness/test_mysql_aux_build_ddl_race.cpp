// Build-vs-concurrent-DDL on MySQL: a trigger on the queried table is dropped
// and created over and over while the accelerator is invalidated and rebuilt
// from partitioned scans on several snapshot connections. A DDL queues for
// the table's exclusive metadata lock behind every open scan transaction, so
// a connection that has not yet touched the table must not wait for it while
// its siblings wait to end: every round must finish, with the right answer.
//
// Usage: test_mysql_aux_build_ddl_race [query] [rounds=5]
// Env:   TPCH_MYSQL_DB (default accel_mysql_test); run with the server's
//        accelerator build threads above 1 and ACCEL_AUX_TEST_GRAIN=1.
#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace accel::mysql::tpch_harness;

constexpr const char *kProbeTrigger = "accel_ddl_race_probe";
constexpr auto kPauseBetweenDdl = std::chrono::milliseconds(300);
} // namespace

int main(int argc, char **argv) {
  try {
    const int query = argc > 1 ? std::atoi(argv[1]) : accel::tpch_params::kLineitemRaceQuery;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 5;
    Require(query == accel::tpch_params::kLineitemRaceQuery, "this test rebuilds the lineitem-sum accelerator over lineitem");

    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);
    EnsureUdf(tools, "accel_invalidate_all", "STRING");
    RunSQL(tools, std::string("DROP TRIGGER IF EXISTS ") + kProbeTrigger);

    std::atomic<bool> stop{false};
    std::atomic<int> ddl_rounds{0};
    std::thread ddl([&] {
      while (!stop.load()) {
        RunSQL(tools, std::string("CREATE TRIGGER ") + kProbeTrigger +
                          " BEFORE INSERT ON lineitem FOR EACH ROW SET @accel_ddl_race = 1");
        RunSQL(tools, std::string("DROP TRIGGER IF EXISTS ") + kProbeTrigger);
        ddl_rounds++;
        std::this_thread::sleep_for(kPauseBetweenDdl);
      }
    });
    for (int round = 0; round < rounds; ++round) {
      Require(RunSQL(tools, "SELECT accel_invalidate_all()"), "invalidate failed");
      RequireQueryMatch(tools, query, true, "round " + std::to_string(round));
    }
    stop = true;
    ddl.join();
    RunSQL(tools, std::string("DROP TRIGGER IF EXISTS ") + kProbeTrigger);
    std::cout << "PASS build vs DDL Q" << query << " (" << rounds << " rebuilds, " << ddl_rounds.load()
              << " DDL cycles)\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
  }
}
