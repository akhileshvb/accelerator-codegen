// Randomized differential fuzz of the TPC-H aux accelerators on MySQL, with
// the engine's own capture (triggers + the accelerator plugin).
//
// Per query: the accelerator is warmed, then W random autocommit writes (the
// capture contract MySQL supports) from tpch_fuzz_writes.hpp; after each one
// the accelerator must equal the reference SQL. Meanwhile R reader threads
// run reference, accelerator, reference: when at most one write landed in
// that window, the accelerator must equal one of the two references (the
// plugin's store is shared by all sessions and read concurrently).
// The eight tables are backed up first and restored (through the triggers)
// at the end.
//
// write_interval_ms: the writer waits that long before each write (0: back
// to back), setting the write frequency the readers see.
//
// Usage: test_mysql_tpch_fuzz [queries|all] [writes=150] [seed=1] [readers=2]
//                             [write_interval_ms=0]
// Env: TPCH_MYSQL_DB (default accel_mysql_test), ACCEL_FORCE_MAINTENANCE_ACTION
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mysql/mysql_tpch_harness.hpp"
#include "tpch_fuzz_writes.hpp"

namespace {

using accel::mysql::Tools;
using namespace accel::mysql::tpch_harness;
using accelerator_codegen::aux_state::engine::mysql::LookupMysqlQuery;
namespace ts = accelerator_codegen::test_support;

const char *kTables[] = {"lineitem", "orders", "customer", "part", "partsupp", "supplier", "nation", "region"};

int64_t Scalar(const Tools &tools, const std::string &sql) {
  auto r = QuerySQL(tools, sql);
  Require(r.first, "query failed: " + sql + "\n" + r.second);
  const auto v = TrimLines(r.second);
  return v.empty() || v == "NULL\n" || v == "NULL" ? 0 : std::stoll(v);
}

std::vector<std::string> Statements(const std::string &entry) {
  std::vector<std::string> out;
  for (std::size_t pos = 0;;) {
    const auto end = entry.find(";\n", pos);
    out.push_back(entry.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    if (end == std::string::npos) {
      return out;
    }
    pos = end + 2;
  }
}

std::string SortedLines(const std::string &text) {
  std::vector<std::string> lines;
  std::stringstream ss(TrimLines(text));
  for (std::string line; std::getline(ss, line);) {
    lines.push_back(line);
  }
  std::sort(lines.begin(), lines.end());
  std::string out;
  for (const auto &line : lines) {
    out += line + "\n";
  }
  return out;
}

// Ordered match; rows tied on the ORDER BY keys may come in any order, so a
// result that differs only in row order also matches (values are compared).
std::string MatchError(const std::string &ref, const std::string &accel, const std::string &label) {
  try {
    RequireOrderedMatch(ref, accel, label);
    return {};
  } catch (const std::exception &e) {
    try {
      RequireOrderedMatch(SortedLines(ref), SortedLines(accel), label);
      return {};
    } catch (...) {
      return e.what();
    }
  }
}

void Backup(const Tools &tools) {
  Require(RunSQL(tools, "CREATE DATABASE IF NOT EXISTS accel_fuzz_bak"), "create backup db");
  for (const char *t : kTables) {
    const std::string bak = std::string("accel_fuzz_bak.") + t;
    // Keep an existing backup (an earlier interrupted run): it is the original data.
    RunSQL(tools, "CREATE TABLE IF NOT EXISTS " + bak + " AS SELECT * FROM " + t);
  }
}

// The tables back to the backup (through the triggers, so the accelerators
// see it as writes).
void ResetTables(const Tools &tools) {
  for (const char *t : kTables) {
    const std::string bak = std::string("accel_fuzz_bak.") + t;
    Require(RunSQL(tools, std::string("DELETE FROM ") + t), std::string("restore: delete ") + t);
    Require(RunSQL(tools, std::string("INSERT INTO ") + t + " SELECT * FROM " + bak),
            std::string("restore: insert ") + t);
  }
}

void Restore(const Tools &tools) {
  ResetTables(tools);
  RunSQL(tools, "DROP DATABASE accel_fuzz_bak");
}

// "hit=H rebuilt_on_read=R fallback=F" (server-wide) → {H, R, F}
std::array<long, 3> ServerCounts(const Tools &tools) {
  std::array<long, 3> out {0, 0, 0};
  auto r = QuerySQL(tools, "SELECT mysql_accel_get_execute_counts()");
  if (r.first)
    std::sscanf(TrimLines(r.second).c_str(), "hit=%ld rebuilt_on_read=%ld fallback=%ld", &out[0], &out[1],
                &out[2]);
  return out;
}

struct Result {
  std::array<long, 3> counts {0, 0, 0}; // hit / rebuilt_on_read / fallback
  long writes = 0, statements = 0, checks = 0, reader_checks = 0, reader_ambiguous = 0;
  std::vector<std::string> failures;
};

Result FuzzQuery(const Tools &tools, int q, int writes, uint64_t seed, int readers, int write_interval_ms) {
  Result result;
  const auto counts_before = ServerCounts(tools);
  const auto &entry = LookupMysqlQuery("tpch", q);
  const std::string ref_sql = ConcreteBaseSql(q, entry);
  const std::string accel_sql = ConcreteAccelSql(q, entry, /*prefer_aux=*/true);
  std::mutex mu;
  auto fail = [&](const std::string &m) {
    std::lock_guard<std::mutex> l(mu);
    result.failures.push_back(m);
  };

  {
    auto ref = QuerySQL(tools, ref_sql);
    auto accel = QuerySQL(tools, accel_sql);
    Require(ref.first && accel.first, "Q" + std::to_string(q) + " warm-up failed: " + accel.second);
    if (auto e = MatchError(ref.second, accel.second, "build"); !e.empty()) {
      fail("after build: " + e);
    }
  }

  // Writes finished / started: a reader window is unambiguous when at most
  // one write could land in it (started at its end - finished at its start).
  std::atomic<long> write_seq {0}, write_started {0};
  std::atomic<bool> stop {false};
  std::atomic<long> reader_checks {0}, ambiguous {0};
  std::vector<std::thread> threads;
  for (int r = 0; r < readers; r++) {
    threads.emplace_back([&, r] {
      while (!stop.load()) {
        const long w0 = write_seq.load();
        auto ref1 = QuerySQL(tools, ref_sql);
        auto accel = QuerySQL(tools, accel_sql);
        auto ref2 = QuerySQL(tools, ref_sql);
        const long w1 = write_started.load();
        if (!ref1.first || !accel.first || !ref2.first) {
          fail("reader " + std::to_string(r) + ": query failed: " + accel.second);
          return;
        }
        if (w1 - w0 > 1) {
          ambiguous++;
          continue;
        }
        reader_checks++;
        const auto e1 = MatchError(ref1.second, accel.second, "reader");
        const auto e2 = MatchError(ref2.second, accel.second, "reader");
        if (!e1.empty() && !e2.empty()) {
          fail("reader " + std::to_string(r) + " (writes " + std::to_string(w0) + ".." + std::to_string(w1) +
               "): " + e1);
        }
      }
    });
  }

  ts::TpchFuzzWrites generator(ts::FuzzDialect::MySQL, seed * 1000 + static_cast<uint64_t>(q),
                               [&](const std::string &sql) { return Scalar(tools, sql); });
  auto failure_count = [&] {
    std::lock_guard<std::mutex> l(mu);
    return result.failures.size();
  };
  for (int w = 0; w < writes && failure_count() < 5; w++) {
    if (write_interval_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(write_interval_ms));
    }
    const auto next = generator.Next();
    for (const auto &sql : Statements(next)) {
      write_started++;
      auto r = QuerySQL(tools, sql);
      if (!r.first) {
        fail("write " + std::to_string(w) + " failed: " + sql + "\n" + r.second);
      }
      result.statements++;
    }
    result.writes++;
    auto ref = QuerySQL(tools, ref_sql);
    auto accel = QuerySQL(tools, accel_sql);
    result.checks++;
    if (!ref.first || !accel.first) {
      fail("after write " + std::to_string(w) + ": query failed: " + accel.second);
    } else if (auto e = MatchError(ref.second, accel.second, "after write"); !e.empty()) {
      fail("after write " + std::to_string(w) + " (" + next + "): " + e);
    }
    // Finished once its aux commit published (the check above waited for it).
    write_seq = write_started.load();
  }
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  result.reader_checks = reader_checks.load();
  result.reader_ambiguous = ambiguous.load();
  const auto counts_after = ServerCounts(tools);
  for (int i = 0; i < 3; ++i)
    result.counts[i] = counts_after[i] - counts_before[i];
  return result;
}

} // namespace

int main(int argc, char **argv) {
  try {
    std::vector<int> queries;
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all") {
      for (int q = 1; q <= 22; q++) {
        queries.push_back(q);
      }
    } else {
      std::stringstream ss(which);
      std::string item;
      while (std::getline(ss, item, ',')) {
        queries.push_back(std::stoi(item));
      }
    }
    const int writes = argc > 2 ? std::atoi(argv[2]) : 150;
    const uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
    const int readers = argc > 4 ? std::atoi(argv[4]) : 2;
    const int write_interval_ms = argc > 5 ? std::atoi(argv[5]) : 0;

    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);
    if (const char *force = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION"); force && *force) {
      RunSQL(tools, std::string("SELECT mysql_accel_set_force_maintenance_action(") + SqlLiteral(force) + ")");
    }
    Backup(tools);
    int failed = 0;
    for (int q : queries) {
      Result r;
      try {
        ResetTables(tools); // every query starts from the same data
        r = FuzzQuery(tools, q, writes, seed, readers, write_interval_ms);
      } catch (const std::exception &e) {
        r.failures.push_back(std::string("error: ") + e.what());
      }
      std::cout << (r.failures.empty() ? "PASS" : "FAIL") << " Q" << q << ": " << r.writes << " writes, "
                << r.statements << " statements, " << r.checks << " checks, " << r.reader_checks
                << " reader checks (" << r.reader_ambiguous << " ambiguous windows skipped) [hit=" << r.counts[0]
                << " rebuilt_on_read=" << r.counts[1] << " fallback=" << r.counts[2] << "]" << std::endl;
      for (std::size_t i = 0; i < r.failures.size() && i < 3; i++) {
        std::cout << "  " << r.failures[i].substr(0, 1500) << std::endl;
      }
      failed += r.failures.empty() ? 0 : 1;
    }
    Restore(tools);
    std::cout << (failed == 0 ? "ALL PASS" : std::to_string(failed) + " queries FAILED") << std::endl;
    return failed == 0 ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "test_mysql_tpch_fuzz failed: " << e.what() << std::endl;
    return 2;
  }
}
