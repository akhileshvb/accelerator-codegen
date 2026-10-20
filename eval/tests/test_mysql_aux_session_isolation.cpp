// Per-session transaction isolation of captured writes on MySQL.
//
// Every connection stages its trigger-captured writes in its own transaction:
// another session's ROLLBACK must not discard them, a COMMIT must not publish
// another session's uncommitted writes, and a statement/savepoint/disconnect
// rollback must undo exactly that session's writes. Each scenario drives
// persistent connections (one `mysql` client process each) step by step, then
// requires the accelerator (Q1 over lineitem) to equal the reference SQL.
//
// Usage: test_mysql_aux_session_isolation [stress_sessions=4] [txns_per_session=12] [seed=1]
// Env:   TPCH_MYSQL_DB (default accel_mysql_test); ACCEL_SESSION_TEST_ACTION
//        (forced maintenance action, default ApplyDelta)
#include "eval/tests/mysql/mysql_session_client.hpp"
#include "eval/tests/mysql/mysql_tpch_harness.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace {
using namespace accel::mysql::tpch_harness;

constexpr int kQuery = 1;

std::string Add(const std::string &key, const std::string &delta) {
  return "UPDATE lineitem SET l_quantity = l_quantity + " + delta +
         " WHERE l_orderkey = " + key + " AND l_linenumber = 1";
}

void Match(const Tools &tools, const std::string &label) {
  RequireQueryMatch(tools, kQuery, /*prefer_aux=*/true, label);
  std::cerr << "  ok: " << label << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const int stress_sessions = argc > 1 ? std::atoi(argv[1]) : 4;
    const int txns = argc > 2 ? std::atoi(argv[2]) : 12;
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1;

    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);
    EnsureUdf(tools, "accel_invalidate_all", "STRING");
    EnsureUdf(tools, "mysql_accel_set_scan_database", "STRING");
    EnsureUdf(tools, "accel_set_force_maintenance_action", "STRING");
    Require(RunSQL(tools, std::string("SELECT mysql_accel_set_scan_database(") +
                              SqlLiteral(tools.database) + ")"),
            "set scan database failed");
    Require(RunSQL(tools, "SELECT accel_invalidate_all()"), "invalidate failed");
    // Apply captured writes as deltas (never rebuild from the tables), so a
    // wrongly published or discarded write stays visible in the aux state.
    const char *action = std::getenv("ACCEL_SESSION_TEST_ACTION");
    Require(RunSQL(tools, std::string("SELECT accel_set_force_maintenance_action('") +
                              (action && *action ? action : "ApplyDelta") + "')"),
            "force maintenance action failed");

    // Rows inside Q1's shipdate window, distinct, outside the stress rows.
    const int need = 6 + stress_sessions * 4;
    auto picked = QuerySQL(tools,
        "SELECT l_orderkey FROM lineitem WHERE l_linenumber = 1 AND l_shipdate < '1997-01-01' "
        "ORDER BY l_orderkey LIMIT " + std::to_string(need));
    Require(picked.first, "cannot pick rows");
    const auto keys = SplitLines(TrimLines(picked.second));
    Require(static_cast<int>(keys.size()) >= need, "not enough rows to write");
    const std::string &ka = keys[0], &kb = keys[1], &kc = keys[2];

    Match(tools, "warm");

    std::cerr << "S1: A writes, B writes, A rolls back, B commits\n";
    {
      Conn a(tools), b(tools);
      a.Ok("BEGIN");
      a.Ok(Add(ka, "7"));
      b.Ok("BEGIN");
      b.Ok(Add(kb, "11"));
      a.Ok("ROLLBACK");
      b.Ok("COMMIT");
      Match(tools, "S1 after A rollback / B commit");
      b.Ok(Add(kb, "-11")); // autocommit: restore
      Match(tools, "S1 restored");
    }

    std::cerr << "S2: B commits while A's transaction is still open\n";
    {
      Conn a(tools), b(tools);
      a.Ok("BEGIN");
      a.Ok(Add(ka, "13"));
      Match(tools, "S2 A's write staged, uncommitted");
      b.Ok(Add(kb, "3")); // autocommit
      Match(tools, "S2 B committed, A open");
      a.Ok("COMMIT");
      Match(tools, "S2 A committed");
      a.Ok(Add(ka, "-13"));
      b.Ok(Add(kb, "-3"));
      Match(tools, "S2 restored");
    }

    std::cerr << "S3: ROLLBACK TO SAVEPOINT\n";
    {
      Conn a(tools), b(tools);
      a.Ok("BEGIN");
      a.Ok(Add(ka, "5"));
      a.Ok("SAVEPOINT s1");
      a.Ok(Add(kb, "9"));
      b.Ok(Add(kc, "2")); // another session commits meanwhile
      a.Ok("ROLLBACK TO SAVEPOINT s1");
      a.Ok("COMMIT");
      Match(tools, "S3 after savepoint rollback");
      a.Ok(Add(ka, "-5"));
      b.Ok(Add(kc, "-2"));
      Match(tools, "S3 restored");
    }

    std::cerr << "S4: a failing statement inside a transaction undoes only itself\n";
    {
      Conn a(tools);
      a.Ok("BEGIN");
      a.Ok(Add(ka, "4"));
      const std::string out = a.Exec(
          "UPDATE lineitem SET l_quantity = IF(l_orderkey = " + kb +
          ", 1/0, l_quantity + 1) WHERE l_linenumber = 1 AND l_orderkey IN (" + kb + "," + kc + ")");
      Require(out.find("ERROR") != std::string::npos, "expected the statement to fail: " + out);
      a.Ok("COMMIT");
      Match(tools, "S4 after failed statement + commit");
      a.Ok(Add(ka, "-4"));
      Match(tools, "S4 restored");
    }

    std::cerr << "S5: disconnect with an open transaction\n";
    {
      Conn a(tools), b(tools);
      a.Ok("BEGIN");
      a.Ok(Add(ka, "21"));
      a.Kill();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      b.Ok(Add(kb, "2"));
      // The dead connection's staged write must not stay pending: a pending
      // write makes every read wait (bounded, 0.5 s) for it to end.
      const auto &entry = LookupMysqlQuery("tpch", kQuery);
      const auto t0 = std::chrono::steady_clock::now();
      Require(QuerySQL(tools, ConcreteAccelSql(kQuery, entry, true)).first, "accel query failed");
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
      Require(ms < 400, "read after disconnect waited " + std::to_string(ms) +
                            " ms: the closed connection's write is still pending");
      Match(tools, "S5 after disconnect");
      b.Ok(Add(kb, "-2"));
      Match(tools, "S5 restored");
    }

    std::cerr << "S6: BEGIN commits the transaction still open (implicit commit)\n";
    {
      Conn a(tools);
      a.Ok("BEGIN");
      a.Ok(Add(ka, "6"));
      a.Ok("BEGIN"); // implicit commit of the first transaction
      a.Ok("ROLLBACK");
      Match(tools, "S6 implicit commit");
      a.Ok(Add(ka, "-6"));
      Match(tools, "S6 restored");
    }

    std::cerr << "S7: " << stress_sessions << " concurrent sessions, random commit/rollback\n";
    {
      std::atomic<int> errors{0};
      std::vector<std::thread> workers;
      std::vector<std::string> sums(stress_sessions);
      for (int w = 0; w < stress_sessions; ++w) {
        workers.emplace_back([&, w] {
          try {
            std::mt19937 rng(seed * 1000 + w);
            Conn c(tools);
            const std::string k1 = keys[6 + w * 4], k2 = keys[7 + w * 4];
            long net1 = 0, net2 = 0;
            for (int t = 0; t < txns; ++t) {
              const int d1 = 1 + static_cast<int>(rng() % 9), d2 = 1 + static_cast<int>(rng() % 9);
              c.Ok("BEGIN");
              c.Ok(Add(k1, std::to_string(d1)));
              if (rng() % 3 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 30));
              c.Ok(Add(k2, std::to_string(d2)));
              if (rng() % 2) {
                c.Ok("COMMIT");
                net1 += d1;
                net2 += d2;
              } else {
                c.Ok("ROLLBACK");
              }
            }
            c.Ok(Add(k1, std::to_string(-net1)));
            c.Ok(Add(k2, std::to_string(-net2)));
          } catch (const std::exception &e) {
            std::cerr << "  worker " << w << ": " << e.what() << '\n';
            ++errors;
          }
        });
      }
      for (auto &t : workers) t.join();
      Require(errors == 0, "stress worker failed");
      Match(tools, "S7 after concurrent sessions");
    }

    std::cout << "test_mysql_aux_session_isolation passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "test_mysql_aux_session_isolation failed: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
