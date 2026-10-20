// Build-vs-concurrent-write race on MySQL: the accelerator's state is
// invalidated, then writers (autocommit UPDATEs, INSERTs and DELETEs on the
// queried table) run while readers query the accelerator, so the rebuild
// happens in the middle of the write stream (a larger database widens the
// window: TPCH_MYSQL_DB=<sf1 db>). After the writers stop, the accelerator
// must equal the reference SQL exactly: a write lost (not replayed after the
// snapshot build) or applied twice (once in the snapshot, once replayed)
// changes the sums. Repeats ROUNDS times.
//
// Usage: test_mysql_aux_build_race [query=1] [rounds=5] [writers=3] [writes_per_writer=30]
// Env:   TPCH_MYSQL_DB (default accel_mysql_test)
#include "eval/tests/mysql/mysql_tpch_harness.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace accel::mysql::tpch_harness;
} // namespace

int main(int argc, char **argv) {
  try {
    const int query = argc > 1 ? std::atoi(argv[1]) : 1;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 5;
    const int writers = argc > 3 ? std::atoi(argv[3]) : 3;
    const int writes = argc > 4 ? std::atoi(argv[4]) : 30;
    Require(query == 1, "this race test drives lineitem writes against Q1's sums");

    auto tools = ConnectTpch();
    if (const char *skip = std::getenv("ACCEL_RACE_SKIP_TRIGGERS"); skip && *skip == '1') {
      EnsureExtension(tools); // the database only has the queried table's triggers
    } else {
      EnsureMutationTriggers(tools);
    }
    EnsureUdf(tools, "accel_invalidate_all", "STRING");
    EnsureUdf(tools, "mysql_accel_set_scan_database", "STRING");
    EnsureUdf(tools, "accel_get_build_stats", "STRING");
    EnsureUdf(tools, "mysql_accel_set_scan_mode", "STRING");
    // ACCEL_RACE_SCAN_MODE=cli: the restart-on-write fallback scheme instead of snapshot builds.
    if (const char *mode = std::getenv("ACCEL_RACE_SCAN_MODE"); mode && *mode) {
      RunSQL(tools, std::string("SELECT mysql_accel_set_scan_mode('") + mode + "')");
    }
    // The plugin scans the database named in the server's environment unless told otherwise.
    if (const char *db = std::getenv("TPCH_MYSQL_DB"); db && *db) {
      Require(RunSQL(tools, std::string("SELECT mysql_accel_set_scan_database('") + db + "')"),
              "set scan database failed");
      Require(RunSQL(tools, "SELECT accel_invalidate_all()"), "invalidate failed");
    }
    const auto &entry = LookupMysqlQuery("tpch", query);
    const std::string accel_sql = ConcreteAccelSql(query, entry, /*prefer_aux=*/true);

    // Rows the writers touch: early shipdates (inside Q1's window), distinct per writer.
    auto picked = QuerySQL(tools,
        "SELECT l_orderkey FROM lineitem WHERE l_linenumber = 1 AND l_shipdate < '1997-01-01' "
        "ORDER BY l_orderkey LIMIT " + std::to_string(writers * writes));
    Require(picked.first, "cannot pick rows");
    const auto keys = SplitLines(TrimLines(picked.second));
    Require(static_cast<int>(keys.size()) >= writers * writes, "not enough rows to write");

    RequireQueryMatch(tools, query, true, "warm");
    int failures = 0;
    for (int round = 0; round < rounds; ++round) {
      Require(RunSQL(tools, "SELECT accel_invalidate_all()"), "invalidate failed");
      std::atomic<bool> stop{false};
      std::atomic<int> reads{0};
      std::vector<std::thread> threads;
      for (int r = 0; r < 2; ++r) {
        threads.emplace_back([&] {
          while (!stop.load()) {
            if (QuerySQL(tools, accel_sql).first) reads++;
          }
        });
      }
      std::vector<std::thread> writer_threads;
      for (int w = 0; w < writers; ++w) {
        writer_threads.emplace_back([&, w] {
          for (int i = 0; i < writes; ++i) {
            const std::string key = keys[static_cast<std::size_t>(w * writes + i)];
            const std::string fake = std::to_string(900000000 + (round * 1000 + w * 100 + i));
            RunSQL(tools, "UPDATE lineitem SET l_quantity = l_quantity + 1 WHERE l_orderkey = " +
                              key + " AND l_linenumber = 1");
            if (i % 3 == 0) {
              RunSQL(tools, "INSERT INTO lineitem SELECT " + fake +
                                ", l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, "
                                "l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, "
                                "l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment "
                                "FROM lineitem WHERE l_orderkey = " + key + " AND l_linenumber = 1");
            }
            if (i % 3 == 1 && i >= 3) {
              const std::string old_fake =
                  std::to_string(900000000 + (round * 1000 + w * 100 + i - 1));
              RunSQL(tools, "DELETE FROM lineitem WHERE l_orderkey = " + old_fake);
            }
          }
        });
      }
      for (auto &t : writer_threads) t.join();
      stop = true;
      for (auto &t : threads) t.join();

      auto counts = QuerySQL(tools, "SELECT accel_get_build_stats()");
      try {
        RequireQueryMatch(tools, query, true, "round " + std::to_string(round));
        std::cout << "round " << round << " ok (" << reads.load() << " concurrent reads) "
                  << TrimLines(counts.second) << std::endl;
      } catch (const std::exception &e) {
        ++failures;
        std::cout << "round " << round << " MISMATCH: " << e.what() << "\n";
      }
    }
    // Leave the table as found: undo the +1s and drop the copies.
    RunSQL(tools, "DELETE FROM lineitem WHERE l_orderkey >= 900000000");
    std::string in_list;
    for (const auto &key : keys) in_list += (in_list.empty() ? "" : ",") + key;
    RunSQL(tools, "UPDATE lineitem SET l_quantity = l_quantity - " + std::to_string(rounds) +
                      " WHERE l_linenumber = 1 AND l_orderkey IN (" + in_list + ")");
    RequireQueryMatch(tools, query, true, "restored");
    if (failures) {
      std::cout << "FAIL build race: " << failures << " of " << rounds << " rounds mismatched\n";
      return 1;
    }
    std::cout << "PASS build race Q" << query << " (" << rounds << " rounds)\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "\n";
    return 1;
  }
}
