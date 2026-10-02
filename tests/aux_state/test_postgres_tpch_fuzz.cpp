// Randomized differential fuzz of the TPC-H aux accelerators on PostgreSQL,
// with the engine's own capture (transition-table triggers + xact callbacks).
//
// Runs each query on a fresh copy of the TPC-H test database (CREATE
// DATABASE ... TEMPLATE), dropped at the end. Per query, on persistent connections:
//   * the writer backend runs T random transactions (1-4 statements from
//     tpch_fuzz_writes.hpp; 15% roll back). After each, the accelerator must
//     equal the reference SQL on the writer's backend (its own maintained
//     state) and on a second backend (state synced or rebuilt across
//     backends);
//   * R reader backends run reference and accelerator inside one REPEATABLE
//     READ transaction (one snapshot), which must agree.
//
// write_interval_ms: the writer waits that long before each transaction
// (0: back to back), setting the write frequency the readers see.
//
// Usage: test_postgres_tpch_fuzz [queries|all] [txns=150] [seed=1] [readers=2]
//                                [write_interval_ms=0]
// Env: TPCH_PG_DB (template, default accel_tpch_test), ACCEL_FORCE_MAINTENANCE_ACTION
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <libpq-fe.h>

#include "postgres_tpch_harness.hpp"
#include "tpch_fuzz_writes.hpp"

namespace {

using accel::postgres::Tools;
using namespace accel::postgres::tpch_harness;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
namespace ts = accelerator_codegen::test_support;

class Conn {
public:
  Conn(const Tools &tools, const std::string &db) {
    const std::string info = "host=" + tools.host + " port=" + std::to_string(tools.port) + " user=" + tools.user +
                             " password=" + tools.password + " dbname=" + db;
    conn = PQconnectdb(info.c_str());
    Require(PQstatus(conn) == CONNECTION_OK, std::string("connect: ") + PQerrorMessage(conn));
    Exec("SET client_min_messages = warning");
    if (!tools.extension_so.empty()) {
      Exec("LOAD '" + tools.extension_so + "'");
    }
  }
  ~Conn() { PQfinish(conn); }
  Conn(const Conn &) = delete;
  Conn &operator=(const Conn &) = delete;

  void Exec(const std::string &sql) {
    PGresult *r = PQexec(conn, sql.c_str());
    const auto st = PQresultStatus(r);
    const std::string err = PQresultErrorMessage(r);
    PQclear(r);
    Require(st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK, sql.substr(0, 300) + ": " + err);
  }
  // Rows as psql -At prints them: cells joined by '|', one row per line.
  std::string Text(const std::string &sql) {
    PGresult *r = PQexec(conn, sql.c_str());
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
      const std::string err = PQresultErrorMessage(r);
      PQclear(r);
      throw std::runtime_error(sql.substr(0, 300) + ": " + err);
    }
    std::string out;
    for (int i = 0; i < PQntuples(r); i++) {
      for (int j = 0; j < PQnfields(r); j++) {
        if (j > 0) {
          out += '|';
        }
        out += PQgetisnull(r, i, j) ? "NULL" : PQgetvalue(r, i, j); // an all-NULL row stays a row
      }
      out += '\n';
    }
    PQclear(r);
    return out;
  }
  int64_t Scalar(const std::string &sql) {
    const auto t = TrimLines(Text(sql));
    return t.empty() ? 0 : std::stoll(t);
  }

private:
  PGconn *conn = nullptr;
};

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

// "hit=H rebuilt_on_read=R fallback=F" → {H, R, F}
std::array<long, 3> ParseCounts(const std::string &text) {
  std::array<long, 3> out {0, 0, 0};
  std::sscanf(text.c_str(), "hit=%ld rebuilt_on_read=%ld fallback=%ld", &out[0], &out[1], &out[2]);
  return out;
}

struct Result {
  std::array<long, 3> counts {0, 0, 0}; // hit / rebuilt_on_read / fallback
  long commits = 0, rollbacks = 0, statements = 0, checks = 0, reader_checks = 0;
  std::vector<std::string> failures;
};

Result FuzzQuery(const Tools &tools, const std::string &db, int q, int txns, uint64_t seed, int readers,
                 const std::string &force, int write_interval_ms) {
  Result result;
  const auto &entry = LookupPostgresQuery("tpch", q);
  const std::string ref_sql = ConcreteBaseSql(q, entry);
  const std::string accel_sql = ConcreteAccelSql(q, entry, /*prefer_aux=*/true);
  std::mutex mu;
  auto fail = [&](const std::string &m) {
    std::lock_guard<std::mutex> l(mu);
    result.failures.push_back(m);
  };
  // Each backend counts its own accelerator queries.
  auto add_counts = [&](Conn &c) {
    try {
      const auto counts = ParseCounts(c.Text("SELECT accel_postgres_get_execute_counts()"));
      std::lock_guard<std::mutex> l(mu);
      for (int i = 0; i < 3; ++i)
        result.counts[i] += counts[i];
    } catch (const std::exception &) {
    }
  };

  Conn writer(tools, db), checker(tools, db);
  writer.Exec("SELECT accel_postgres_set_force_maintenance_action('" + force + "')");
  auto check = [&](Conn &c, const std::string &label) {
    try {
      const auto ref = c.Text(ref_sql);
      const auto accel = c.Text(accel_sql);
      return MatchError(ref, accel, label);
    } catch (const std::exception &e) {
      return std::string(label + " query failed: ") + e.what();
    }
  };
  if (auto e = check(writer, "build"); !e.empty()) {
    fail("after build: " + e);
  }

  std::atomic<bool> stop {false};
  std::atomic<long> reader_checks {0};
  std::vector<std::thread> threads;
  for (int r = 0; r < readers; r++) {
    threads.emplace_back([&, r] {
      try {
        Conn rc(tools, db);
        while (!stop.load()) {
          rc.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
          const auto ref = rc.Text(ref_sql);
          const auto accel = rc.Text(accel_sql);
          rc.Exec("COMMIT");
          reader_checks++;
          if (auto e = MatchError(ref, accel, "reader"); !e.empty()) {
            fail("reader " + std::to_string(r) + ": " + e);
          }
        }
        add_counts(rc);
      } catch (const std::exception &e) {
        fail("reader " + std::to_string(r) + " error: " + e.what());
      }
    });
  }

  ts::TpchFuzzWrites generator(ts::FuzzDialect::Postgres, seed * 1000 + static_cast<uint64_t>(q),
                               [&](const std::string &sql) { return writer.Scalar(sql); });
  std::mt19937_64 rng(seed * 7919 + static_cast<uint64_t>(q));
  auto failure_count = [&] {
    std::lock_guard<std::mutex> l(mu);
    return result.failures.size();
  };
  for (int t = 0; t < txns && failure_count() < 5; t++) {
    if (write_interval_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(write_interval_ms));
    }
    std::string all;
    writer.Exec("BEGIN");
    try {
      const int n = 1 + static_cast<int>(rng() % 4);
      for (int i = 0; i < n; i++) {
        const auto next = generator.Next();
        all += (all.empty() ? "" : " ;; ") + next;
        for (const auto &sql : Statements(next)) {
          writer.Exec(sql);
          result.statements++;
        }
      }
    } catch (const std::exception &e) {
      writer.Exec("ROLLBACK");
      fail("txn " + std::to_string(t) + " statement failed: " + e.what());
      break;
    }
    if (rng() % 100 < 15) {
      writer.Exec("ROLLBACK");
      result.rollbacks++;
    } else {
      writer.Exec("COMMIT");
      result.commits++;
    }
    result.checks++;
    if (auto e = check(writer, "writer backend"); !e.empty()) {
      fail("after txn " + std::to_string(t) + " (" + all.substr(0, 1200) + "): " + e);
    }
    if (auto e = check(checker, "other backend"); !e.empty()) {
      fail("after txn " + std::to_string(t) + " (" + all.substr(0, 1200) + "): " + e);
    }
  }
  stop = true;
  for (auto &th : threads) {
    th.join();
  }
  writer.Exec("SELECT accel_postgres_set_force_maintenance_action('')");
  add_counts(writer);
  add_counts(checker);
  result.reader_checks = reader_checks.load();
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
    const int txns = argc > 2 ? std::atoi(argv[2]) : 150;
    const uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
    const int readers = argc > 4 ? std::atoi(argv[4]) : 2;
    const int write_interval_ms = argc > 5 ? std::atoi(argv[5]) : 0;
    const char *force_env = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
    const std::string force = force_env ? force_env : "";

    auto tools = ConnectTpch(); // the template DB: extension functions + triggers
    EnsureMutationTriggers(tools);
    const std::string templ = tools.database;
    const std::string db = "accel_tpch_fuzz";
    int failed = 0;
    for (int q : queries) {
      Result r;
      try {
        // Every query starts from the same data: a fresh copy of the template.
        Conn admin(tools, "postgres");
        admin.Exec("DROP DATABASE IF EXISTS " + db + " WITH (FORCE)");
        admin.Exec("CREATE DATABASE " + db + " TEMPLATE " + templ);
        r = FuzzQuery(tools, db, q, txns, seed, readers, force, write_interval_ms);
      } catch (const std::exception &e) {
        r.failures.push_back(std::string("error: ") + e.what());
      }
      std::cout << (r.failures.empty() ? "PASS" : "FAIL") << " Q" << q << ": " << r.commits << " commits, "
                << r.rollbacks << " rollbacks, " << r.statements << " statements, " << r.checks
                << " checks (x2 backends), " << r.reader_checks << " reader checks [hit=" << r.counts[0]
                << " rebuilt_on_read=" << r.counts[1] << " fallback=" << r.counts[2] << "]" << std::endl;
      for (std::size_t i = 0; i < r.failures.size() && i < 3; i++) {
        std::cout << "  " << r.failures[i].substr(0, 1500) << std::endl;
      }
      failed += r.failures.empty() ? 0 : 1;
    }
    {
      Conn admin(tools, "postgres");
      admin.Exec("DROP DATABASE IF EXISTS " + db + " WITH (FORCE)");
    }
    std::cout << (failed == 0 ? "ALL PASS" : std::to_string(failed) + " queries FAILED") << std::endl;
    return failed == 0 ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "test_postgres_tpch_fuzz failed: " << e.what() << std::endl;
    return 2;
  }
}
