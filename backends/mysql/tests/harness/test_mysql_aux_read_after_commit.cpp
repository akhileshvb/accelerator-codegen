// Read-after-commit on MySQL: a read issued after a write's commit returned
// must see it, whichever connection reads, and reads concurrent with writes
// must be answered from the state, never wrongly.
//
// Writers add 1 to the quantity of distinct lineitem rows (inside Q1's
// shipdate window, so Q1's total quantity moves by exactly 1 per committed
// write). A read's total must lie between the writes that completed before it
// began and the writes that began before it ended.
//
//  1. sequential: after each commit (autocommit, explicit transaction) a read
//     on another connection returns the new total;
//  2. slow commit: the same with maintenance forced to rebuild, so each commit
//     is followed by a rebuild the read must wait for, not skip;
//  3. continuous: writer connections write while reader connections read; every
//     read is within bounds, most are answered from the state (hits), and the
//     accelerator equals the reference SQL at the end.
//
// Usage: test_mysql_aux_read_after_commit [sequential_rounds=100] [rebuild_rounds=2]
//                                         [writers=2] [readers=3] [seconds=20]
// Env:   TPCH_MYSQL_DB (default accel_mysql_test)
#include "backends/mysql/tests/harness/mysql_session_client.hpp"
#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace accel::mysql::tpch_harness;

constexpr int kQuery = 1;
constexpr int64_t kHundredths = 100; // one unit of quantity, at the column's scale 2

// Q1's total quantity, in hundredths, from the accelerator's rows
// (returnflag|linestatus|sum_qty|...).
int64_t TotalQuantity(const std::string &rows) {
  int64_t total = 0;
  for (const auto &line : SplitLines(TrimLines(rows))) {
    const auto start = line.find('|', line.find('|') + 1) + 1;
    std::string quantity = line.substr(start, line.find('|', start) - start);
    quantity.erase(quantity.find('.'), 1);
    total += std::stoll(quantity);
  }
  return total;
}

std::string AddOne(const std::string &key) {
  return "UPDATE lineitem SET l_quantity = l_quantity + 1 WHERE l_orderkey = " + key +
         " AND l_linenumber = 1";
}

struct ExecuteCounts {
  uint64_t hit = 0, fallback = 0;
};

ExecuteCounts ReadCounts(const Tools &tools) {
  const auto counts = QuerySQL(tools, "SELECT accel_get_execute_counts()");
  Require(counts.first, "execute counts failed");
  ExecuteCounts out;
  const std::string text = counts.second;
  out.hit = std::stoull(text.substr(text.find("hit=") + 4));
  out.fallback = std::stoull(text.substr(text.find("fallback=") + 9));
  return out;
}

void ForceAction(const Tools &tools, const char *action) {
  Require(RunSQL(tools, std::string("SELECT accel_set_force_maintenance_action('") + action + "')"),
          "force maintenance action failed");
}

} // namespace

int main(int argc, char **argv) {
  try {
    const int sequential_rounds = argc > 1 ? std::atoi(argv[1]) : 100;
    const int rebuild_rounds = argc > 2 ? std::atoi(argv[2]) : 2;
    const int writers = argc > 3 ? std::atoi(argv[3]) : 2;
    const int readers = argc > 4 ? std::atoi(argv[4]) : 3;
    const int seconds = argc > 5 ? std::atoi(argv[5]) : 20;

    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);
    EnsureUdf(tools, "accel_set_force_maintenance_action", "STRING");
    EnsureUdf(tools, "accel_get_execute_counts", "STRING");
    EnsureUdf(tools, "accel_invalidate_all", "STRING");
    EnsureUdf(tools, "mysql_accel_set_scan_database", "STRING");
    Require(RunSQL(tools, "SELECT mysql_accel_set_scan_database(" + SqlLiteral(tools.database) + ")"),
            "set scan database failed");
    Require(RunSQL(tools, "SELECT accel_invalidate_all()"), "invalidate failed");
    ForceAction(tools, "ApplyDelta");

    const auto &entry = LookupMysqlQuery("tpch", kQuery);
    const std::string accel_sql = ConcreteAccelSql(kQuery, entry, /*prefer_aux=*/true);

    const int need = sequential_rounds + rebuild_rounds + writers * 4 + 4;
    auto picked = QuerySQL(tools,
        "SELECT l_orderkey FROM lineitem WHERE l_linenumber = 1 AND l_shipdate < '1997-01-01' "
        "ORDER BY l_orderkey LIMIT " + std::to_string(need));
    Require(picked.first, "cannot pick rows");
    const auto keys = SplitLines(TrimLines(picked.second));
    Require(static_cast<int>(keys.size()) >= need, "not enough rows to write");

    RequireQueryMatch(tools, kQuery, true, "warm");
    Conn writer(tools), reader(tools);
    auto total_now = [&] { return TotalQuantity(reader.Exec(accel_sql)); };
    const int64_t base = total_now();
    std::vector<std::string> written; // every key written once per +1, restored at the end

    auto sequential = [&](int rounds, const char *label, int first_key) {
      int64_t expected = total_now();
      for (int i = 0; i < rounds; ++i) {
        const std::string key = keys[static_cast<std::size_t>(first_key + i)];
        if (i % 2 == 0) {
          writer.Ok(AddOne(key));
        } else {
          writer.Ok("BEGIN");
          writer.Ok(AddOne(key));
          writer.Ok("COMMIT");
        }
        written.push_back(key);
        expected += kHundredths;
        const int64_t got = total_now();
        Require(got == expected, std::string(label) + " round " + std::to_string(i) +
                                     ": read after commit returned " + std::to_string(got) +
                                     ", expected " + std::to_string(expected));
      }
      std::cerr << "  ok: " << label << " (" << rounds << " rounds)\n";
    };

    sequential(sequential_rounds, "sequential", 0);

    // An open transaction's write is not visible to another connection.
    writer.Ok("BEGIN");
    writer.Ok(AddOne(keys[static_cast<std::size_t>(sequential_rounds)]));
    Require(total_now() == base + sequential_rounds * kHundredths, "an open transaction's write is visible");
    writer.Ok("ROLLBACK");

    ForceAction(tools, "RebuildAll");
    sequential(rebuild_rounds, "slow commit (rebuild)", sequential_rounds + 1);
    ForceAction(tools, "ApplyDelta");

    // Continuous writes and reads.
    std::atomic<bool> stop{false};
    std::atomic<int64_t> started{0}, completed{0};
    std::atomic<int> errors{0}, reads{0};
    std::vector<std::vector<std::string>> writer_keys(static_cast<std::size_t>(writers));
    const int free_key = sequential_rounds + rebuild_rounds + 1;
    const int64_t floor_total = total_now();
    const ExecuteCounts before = ReadCounts(tools);
    std::vector<std::thread> threads;
    for (int w = 0; w < writers; ++w) {
      threads.emplace_back([&, w] {
        try {
          Conn c(tools);
          const std::string k1 = keys[static_cast<std::size_t>(free_key + w * 2)];
          const std::string k2 = keys[static_cast<std::size_t>(free_key + w * 2 + 1)];
          for (int i = 0; !stop.load(); ++i) {
            if (i % 2 == 0) {
              started += 1;
              c.Ok(AddOne(k1));
              completed += 1;
              writer_keys[static_cast<std::size_t>(w)].push_back(k1);
            } else {
              started += 2;
              c.Ok("BEGIN");
              c.Ok(AddOne(k1));
              c.Ok(AddOne(k2));
              c.Ok("COMMIT");
              completed += 2;
              writer_keys[static_cast<std::size_t>(w)].push_back(k1);
              writer_keys[static_cast<std::size_t>(w)].push_back(k2);
            }
          }
        } catch (const std::exception &e) {
          std::cerr << "  writer " << w << ": " << e.what() << '\n';
          ++errors;
        }
      });
    }
    for (int r = 0; r < readers; ++r) {
      threads.emplace_back([&] {
        try {
          Conn c(tools);
          while (!stop.load()) {
            const int64_t at_least = completed.load();
            const int64_t got = TotalQuantity(c.Exec(accel_sql));
            const int64_t at_most = started.load();
            const int64_t writes = (got - floor_total) / kHundredths;
            if ((got - floor_total) % kHundredths != 0 || writes < at_least || writes > at_most) {
              std::cerr << "  WRONG read: " << writes << " writes visible, between " << at_least
                        << " and " << at_most << '\n';
              ++errors;
            }
            ++reads;
          }
        } catch (const std::exception &e) {
          std::cerr << "  reader: " << e.what() << '\n';
          ++errors;
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    for (auto &t : threads) t.join();
    const ExecuteCounts after = ReadCounts(tools);
    const uint64_t hits = after.hit - before.hit, fallbacks = after.fallback - before.fallback;
    std::cerr << "  continuous: writes=" << completed.load() << " reads=" << reads.load()
              << " hits=" << hits << " fallbacks=" << fallbacks << " wrong=" << errors.load() << '\n';
    Require(errors == 0, "wrong answers or failures under continuous writes");
    Require(hits > fallbacks, "continuous reads were mostly answered from SQL, not the state");
    RequireQueryMatch(tools, kQuery, true, "after continuous writes");

    // Leave the table as found.
    for (const auto &keys_of : writer_keys) written.insert(written.end(), keys_of.begin(), keys_of.end());
    for (const auto &key : written) {
      writer.Ok("UPDATE lineitem SET l_quantity = l_quantity - 1 WHERE l_orderkey = " + key +
                " AND l_linenumber = 1");
    }
    Require(total_now() == base, "restored table differs");
    std::cout << "PASS read after commit (" << sequential_rounds << "+" << rebuild_rounds
              << " sequential rounds, continuous hits=" << hits << " fallbacks=" << fallbacks << ")\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "FAIL read after commit: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
