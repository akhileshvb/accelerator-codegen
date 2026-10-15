// Shard workers on PostgreSQL: one worker per active accelerator holds its one copy of the
// state, maintained from the commits writers ship to it; a read is answered at the epoch its
// snapshot sees (state/shard_worker.hpp).
//
//   hit_across_connections  a second (and later) connection gets a hit with no rebuild after the
//                           first connection built the state
//   memory                  worker RSS stays flat as connections are added
//   concurrent              writers (commit / rollback / savepoints) and REPEATABLE READ readers
//                           at once: aux == reference in every reader snapshot, and after
//                           quiescence
//   hits_under_writes       a writer committing continuously while readers read: nearly every
//                           read is served (the old design fell back for as long as writes
//                           lasted), and no answer is wrong
//   read_after_own_commit   a read right after the same connection's commit sees it, served
//   older_snapshot          a REPEATABLE READ reader that began before a commit still gets its
//                           own snapshot's answer afterwards
//   abort_and_savepoint     rolled back transactions and subtransactions change nothing and cost
//                           no rebuild
//   truncate_rebuild        TRUNCATE (not maintainable) rebuilds the state; answers stay exact
//   kill_backend            a backend killed mid-transaction leaves the state usable (no
//                           rebuild, still hits)
//   restart_worker          killing the workers mid-load: answers stay exact, new workers
//                           rebuild on demand and hit again
//
// Run it where the server's processes are visible (/proc): inside the
// sandbox container via `pgsbx.sh run`.
//
// Usage: test_postgres_shared_state [scenario|all] [seconds=12] [seed=1]
// Env:   TPCH_PG_DB (template DB; a fresh copy is made), ACCEL_PG_DOCKER=1
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <libpq-fe.h>

#include "postgres/postgres_tpch_harness.hpp"
#include "tpch_fuzz_writes.hpp"

namespace {

using accel::postgres::Tools;
using namespace accel::postgres::tpch_harness;
using accelerator_codegen::aux_state::engine::postgres::LookupPostgresQuery;
namespace ts = accelerator_codegen::test_support;

const std::string kDb = "accel_tpch_shared";

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
    // Wait for a worker still building the state instead of answering from SQL: the scenarios
    // below check that reads are hits.
    Exec("SET accel.read_wait_ms = 120000");
  }
  ~Conn() { PQfinish(conn); }
  Conn(const Conn &) = delete;
  Conn &operator=(const Conn &) = delete;

  // Returns the error text, empty on success.
  std::string TryExec(const std::string &sql) {
    PGresult *r = PQexec(conn, sql.c_str());
    const auto st = PQresultStatus(r);
    const std::string err = PQresultErrorMessage(r);
    PQclear(r);
    return (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK) ? std::string() : (err.empty() ? "error" : err);
  }
  void Exec(const std::string &sql) {
    const auto err = TryExec(sql);
    Require(err.empty(), sql.substr(0, 300) + ": " + err);
  }
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
        out += PQgetisnull(r, i, j) ? "NULL" : PQgetvalue(r, i, j);
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
  int Pid() { return static_cast<int>(Scalar("SELECT pg_backend_pid()")); }
  // This backend's own accelerator answers: "hit rebuilt fallback".
  std::array<long, 3> Counts() {
    std::array<long, 3> out{0, 0, 0};
    std::sscanf(Text("SELECT accel_postgres_get_execute_counts()").c_str(),
                "hit=%ld rebuilt_on_read=%ld fallback=%ld", &out[0], &out[1], &out[2]);
    return out;
  }
  PGconn *Raw() { return conn; }

private:
  PGconn *conn = nullptr;
};

struct Stats {
  std::map<std::string, long> v;
  long operator[](const std::string &k) const {
    auto it = v.find(k);
    return it == v.end() ? 0 : it->second;
  }
};

Stats ReadStats(Conn &c) {
  Stats s;
  std::stringstream ss(TrimLines(c.Text("SELECT accel_postgres_shard_stats()")));
  std::string tok;
  while (ss >> tok) {
    const auto eq = tok.find('=');
    if (eq != std::string::npos)
      s.v[tok.substr(0, eq)] = std::atol(tok.c_str() + eq + 1);
  }
  return s;
}

std::string StatsLine(const Stats &s) {
  std::ostringstream o;
  for (const char *k : {"requests", "served", "fallback", "errors", "commits", "applied", "builds", "gaps",
                        "generation"})
    o << k << "=" << s[k] << " ";
  return o.str();
}

int WorkerPid(Conn &admin, const std::string &db) {
  return static_cast<int>(admin.Scalar("SELECT coalesce((SELECT pid FROM pg_stat_activity WHERE backend_type = "
                                       "'accel shard worker' AND datname = '" + db + "' LIMIT 1), 0)"));
}

// Every shard worker of `db`.
std::vector<int> WorkerPids(Conn &admin, const std::string &db) {
  std::vector<int> pids;
  std::istringstream in(admin.Text("SELECT pid FROM pg_stat_activity WHERE backend_type = 'accel shard worker' AND "
                                   "datname = '" + db + "'"));
  for (std::string line; std::getline(in, line);)
    if (!line.empty())
      pids.push_back(std::stoi(line));
  return pids;
}

void StopWorker(Conn &admin, const std::string &db) {
  admin.Exec("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'accel shard worker' AND "
             "datname = '" + db + "'");
  for (int i = 0; i < 100 && WorkerPid(admin, db) != 0; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

long ProcKb(int pid, const char *field) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  const std::string key = std::string(field) + ":";
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) == 0)
      return std::atol(line.c_str() + key.size());
  }
  return -1;
}

std::string SortedLines(const std::string &text) {
  std::vector<std::string> lines;
  std::stringstream ss(TrimLines(text));
  for (std::string line; std::getline(ss, line);)
    lines.push_back(line);
  std::sort(lines.begin(), lines.end());
  std::string out;
  for (const auto &line : lines)
    out += line + "\n";
  return out;
}

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

struct Q {
  int n;
  std::string ref, accel;
};
Q MakeQ(int n) {
  const auto &entry = LookupPostgresQuery("tpch", n);
  return Q{n, ConcreteBaseSql(n, entry), ConcreteAccelSql(n, entry, /*prefer_aux=*/true)};
}

std::string Check(Conn &c, const Q &q, const std::string &label) {
  try {
    return MatchError(c.Text(q.ref), c.Text(q.accel), label + " Q" + std::to_string(q.n));
  } catch (const std::exception &e) {
    return label + " Q" + std::to_string(q.n) + " query failed: " + e.what();
  }
}

// A fresh copy of the template database, the helper SQL functions installed.
void MakeDb(const Tools &tools) {
  const std::string so = tools.extension_so.empty() ? "accelerator_codegen_pg" : tools.extension_so;
  for (const char *fn : {"accel_postgres_shard_stats() RETURNS text", "accel_postgres_stop_shard_workers() RETURNS bool"}) {
    const std::string name = std::string(fn).substr(0, std::string(fn).find('('));
    Require(RunSQL(tools, std::string("CREATE OR REPLACE FUNCTION ") + fn + " AS " + SqlLiteral(so) + ", '" + name +
                              "' LANGUAGE C"),
            std::string("install ") + name);
  }
  const std::string templ = tools.database;
  Conn admin(tools, "postgres");
  StopWorker(admin, kDb);
  admin.Exec("DROP DATABASE IF EXISTS " + kDb + " WITH (FORCE)");
  StopWorker(admin, templ);
  admin.Exec("CREATE DATABASE " + kDb + " TEMPLATE " + templ);
}

int g_failures = 0;
void Report(const std::string &name, const std::string &err, const std::string &info) {
  std::cout << (err.empty() ? "PASS " : "FAIL ") << name << (info.empty() ? "" : "  [" + info + "]") << std::endl;
  if (!err.empty()) {
    std::cout << "  " << err.substr(0, 1500) << std::endl;
    ++g_failures;
  }
}

// ---- scenarios -------------------------------------------------------------------------

void HitAcrossConnections(const Tools &tools) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const Q q1 = MakeQ(1), q6 = MakeQ(6);
    Conn a(tools, kDb);
    if (auto e = Check(a, q1, "first connection"); !e.empty())
      throw std::runtime_error(e);
    const Stats s1 = ReadStats(a);
    Require(s1["builds"] == 1, "expected exactly one build, got " + StatsLine(s1));
    Require(a.Counts()[0] >= 1, "the first connection should have been served by the worker");
    // Ten more connections: every Q1 is a hit, nobody rebuilds.
    long hits = 0;
    for (int i = 0; i < 10; i++) {
      Conn b(tools, kDb);
      const auto before = b.Counts();
      if (auto e = Check(b, q1, "connection " + std::to_string(i)); !e.empty())
        throw std::runtime_error(e);
      const auto after = b.Counts();
      Require(after[0] - before[0] >= 1 && after[2] == before[2],
              "connection " + std::to_string(i) + " did not get a pure hit");
      hits += after[0] - before[0];
    }
    const Stats s2 = ReadStats(a);
    Require(s2["builds"] == 1, "a later connection rebuilt: " + StatsLine(s2));
    // A second accelerator: built once, then shared the other way round.
    Conn b(tools, kDb);
    if (auto e = Check(b, q6, "Q6 first use"); !e.empty())
      throw std::runtime_error(e);
    const auto c6 = a.Counts();
    if (auto e = Check(a, q6, "Q6 on the first connection"); !e.empty())
      throw std::runtime_error(e);
    const auto d6 = a.Counts();
    Require(d6[0] > c6[0] && d6[2] == c6[2], "the first connection should hit Q6");
    const Stats s3 = ReadStats(a);
    Require(s3["builds"] == 2, "expected two builds in total: " + StatsLine(s3));
    info = "11 connections, " + std::to_string(hits) + " hits; " + StatsLine(s3);
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("hit_across_connections", err, info);
}

struct MemRow {
  int conns;
  long worker_anon_kb, clients_anon_kb;
  long builds;
};

MemRow MeasureMemory(const Tools &tools, int n, const std::vector<Q> &qs) {
  Conn admin(tools, "postgres");
  StopWorker(admin, kDb);
  std::vector<std::unique_ptr<Conn>> conns;
  std::vector<int> pids;
  for (int i = 0; i < n; i++) {
    conns.emplace_back(std::make_unique<Conn>(tools, kDb));
    pids.push_back(conns.back()->Pid());
  }
  std::vector<std::thread> th;
  std::atomic<int> bad{0};
  for (int i = 0; i < n; i++) {
    th.emplace_back([&, i] {
      for (const auto &q : qs) {
        if (!Check(*conns[i], q, "mem").empty())
          bad++;
      }
    });
  }
  for (auto &t : th)
    t.join();
  Require(bad == 0, "mismatch while measuring memory");
  MemRow row{n, 0, 0, 0};
  // One worker per accelerator: together they hold the one copy of every state.
  const std::vector<int> wps = WorkerPids(admin, kDb);
  Require(!wps.empty(), "no worker");
  for (int wp : wps)
    row.worker_anon_kb += ProcKb(wp, "RssAnon");
  Conn probe(tools, kDb);
  row.builds = ReadStats(probe)["builds"];
  for (int pid : pids)
    row.clients_anon_kb += ProcKb(pid, "RssAnon");
  return row;
}

void Memory(const Tools &tools) {
  std::string err, info;
  try {
    std::vector<Q> qs;
    for (int n : {1, 3, 6, 9, 12, 14, 18, 22})
      qs.push_back(MakeQ(n));
    std::vector<MemRow> rows;
    for (int n : {1, 8, 32})
      rows.push_back(MeasureMemory(tools, n, qs));
    std::ostringstream o;
    auto mb = [](long kb) { return static_cast<double>(kb) / 1024.0; };
    o << "queries=" << qs.size() << " RssAnon MB:";
    for (const auto &r : rows)
      o << " | N=" << r.conns << ": workers=" << mb(r.worker_anon_kb) << " clients_total=" << mb(r.clients_anon_kb)
        << " builds=" << r.builds;
    info = o.str();
    // The workers' footprint must not scale with connections.
    const double w1 = mb(rows.front().worker_anon_kb), wn = mb(rows.back().worker_anon_kb);
    Require(wn <= w1 * 1.3 + 16.0, "workers' RSS grew with connections: " + std::to_string(w1) + " -> " + std::to_string(wn) + " MB");
    // Builds happen once per accelerator, not once per connection.
    Require(rows.back().builds <= static_cast<long>(qs.size()),
            "built more than once per accelerator: " + std::to_string(rows.back().builds));
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("memory", err, info);
}

// Writers (commit / rollback / savepoint) and REPEATABLE READ readers at once.
void Concurrent(const Tools &tools, int seconds, uint64_t seed) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const std::vector<int> qn = {1, 3, 6, 12, 14, 18};
    std::vector<Q> qs;
    for (int n : qn)
      qs.push_back(MakeQ(n));
    std::mutex mu;
    std::vector<std::string> failures;
    auto fail = [&](const std::string &m) {
      std::lock_guard<std::mutex> l(mu);
      if (failures.size() < 5)
        failures.push_back(m);
    };
    std::atomic<bool> stop{false};
    std::atomic<long> commits{0}, rollbacks{0}, savepoints{0}, deadlocks{0}, reads{0};
    std::array<std::atomic<long>, 3> counts{};
    for (auto &c : counts)
      c = 0;
    std::vector<std::thread> threads;
    const int writers = 3, readers = 4;
    // One generator for all writers (keys stay unique across them: the TPC-H
    // test tables have no primary keys), serialised by a mutex.
    Conn gen_conn(tools, kDb);
    ts::TpchFuzzWrites gen(ts::FuzzDialect::Postgres, seed * 131,
                           [&](const std::string &sql) { return gen_conn.Scalar(sql); });
    std::mutex gen_mu;
    for (int w = 0; w < writers; w++) {
      threads.emplace_back([&, w] {
        try {
          Conn c(tools, kDb);
          std::mt19937_64 rng(seed * 977 + static_cast<uint64_t>(w));
          while (!stop.load()) {
            c.Exec("BEGIN");
            bool broken = false;
            const int n = 1 + static_cast<int>(rng() % 3);
            for (int i = 0; i < n && !broken; i++) {
              const bool sp = rng() % 6 == 0;
              if (sp)
                c.Exec("SAVEPOINT s1");
              std::string stmt;
              {
                std::lock_guard<std::mutex> l(gen_mu);
                stmt = gen.Next();
              }
              std::size_t pos = 0;
              while (true) {
                const auto end = stmt.find(";\n", pos);
                const auto one = stmt.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                const auto e = c.TryExec(one);
                if (!e.empty()) {
                  if (e.find("deadlock") != std::string::npos || e.find("could not serialize") != std::string::npos ||
                      e.find("duplicate key") != std::string::npos || e.find("current transaction is aborted") != std::string::npos) {
                    deadlocks++;
                    broken = true;
                  } else {
                    fail("writer statement failed: " + one.substr(0, 200) + ": " + e);
                    broken = true;
                  }
                  break;
                }
                if (end == std::string::npos)
                  break;
                pos = end + 2;
              }
              if (sp && !broken) {
                if (rng() % 2) {
                  c.Exec("ROLLBACK TO SAVEPOINT s1");
                  savepoints++;
                } else {
                  c.Exec("RELEASE SAVEPOINT s1");
                }
              }
            }
            if (broken || rng() % 100 < 25) {
              c.Exec("ROLLBACK");
              rollbacks++;
            } else {
              const auto e = c.TryExec("COMMIT");
              if (!e.empty())
                fail("commit failed: " + e);
              commits++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2 + rng() % 20));
          }
        } catch (const std::exception &e) {
          fail(std::string("writer error: ") + e.what());
        }
      });
    }
    for (int r = 0; r < readers; r++) {
      threads.emplace_back([&, r] {
        try {
          Conn c(tools, kDb);
          std::mt19937_64 rng(seed * 31 + static_cast<uint64_t>(r));
          while (!stop.load()) {
            const Q &q = qs[rng() % qs.size()];
            c.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
            const auto ref = c.Text(q.ref);
            const auto accel = c.Text(q.accel);
            c.Exec("COMMIT");
            reads++;
            if (auto e = MatchError(ref, accel, "reader Q" + std::to_string(q.n)); !e.empty()) {
              auto head = [](const std::string &t) {
                std::stringstream in(t);
                std::string line, out;
                for (int i = 0; i < 4 && std::getline(in, line); i++)
                  out += "    " + line + "\n";
                return out;
              };
              fail("reader " + std::to_string(r) + " pid " + std::to_string(c.Pid()) + ": " + e + "\n  ref:\n" + head(ref) + "  accel:\n" + head(accel));
            }
          }
          const auto k = c.Counts();
          for (int i = 0; i < 3; i++)
            counts[i] += k[i];
        } catch (const std::exception &e) {
          fail(std::string("reader error: ") + e.what());
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    for (auto &t : threads)
      t.join();
    if (!failures.empty())
      throw std::runtime_error(failures.front());
    // Quiescent: every accelerator equals the reference, and is served (hit).
    Conn c(tools, kDb);
    for (const auto &q : qs) {
      if (auto e = Check(c, q, "after quiescence"); !e.empty())
        throw std::runtime_error(e);
    }
    const auto before = c.Counts();
    for (const auto &q : qs) {
      if (auto e = Check(c, q, "after quiescence (again)"); !e.empty())
        throw std::runtime_error(e);
    }
    const auto after = c.Counts();
    Require(after[0] - before[0] == static_cast<long>(qs.size()) && after[2] == before[2] && after[1] == before[1],
            "after quiescence the accelerators should all hit");
    const Stats s = ReadStats(c);
    std::ostringstream o;
    o << writers << " writers, " << readers << " readers, " << seconds << "s: commits=" << commits
      << " rollbacks=" << rollbacks << " savepoint_rollbacks=" << savepoints << " deadlock/dup=" << deadlocks
      << " reader_checks=" << reads << " reader_paths[hit=" << counts[0] << " rebuilt=" << counts[1]
      << " fallback=" << counts[2] << "]; worker: " << StatsLine(s);
    info = o.str();
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("concurrent", err, info);
}

void KillBackend(const Tools &tools) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const Q q = MakeQ(1);
    Conn reader(tools, kDb);
    if (auto e = Check(reader, q, "warm"); !e.empty())
      throw std::runtime_error(e);
    const Stats s0 = ReadStats(reader);
    // A writer dies in the middle of its transaction (an uncommitted change in flight).
    {
      Conn w(tools, kDb);
      const int pid = w.Pid();
      w.Exec("BEGIN");
      w.Exec("UPDATE lineitem SET l_quantity = l_quantity + 1 WHERE ctid IN (SELECT ctid FROM lineitem LIMIT 50)");
      // While it is in flight the reader cannot be certified: SQL answers, still exact.
      if (auto e = Check(reader, q, "while in flight"); !e.empty())
        throw std::runtime_error(e);
      admin.Exec("SELECT pg_terminate_backend(" + std::to_string(pid) + ")");
      for (int i = 0; i < 100; i++) {
        if (admin.Scalar("SELECT count(*) FROM pg_stat_activity WHERE pid = " + std::to_string(pid)) == 0)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      // The dead backend is gone; its connection object just fails.
    }
    const auto before = reader.Counts();
    if (auto e = Check(reader, q, "after the writer died"); !e.empty())
      throw std::runtime_error(e);
    const auto after = reader.Counts();
    Require(after[0] > before[0] && after[1] == before[1],
            "after the killed writer the state should serve hits again without a rebuild");
    const Stats s1 = ReadStats(reader);
    Require(s1["builds"] == s0["builds"], "the killed writer forced a rebuild: " + StatsLine(s1));
    // A writer killed after it wrote and before it committed must not have changed anything.
    Conn w2(tools, kDb);
    const int pid2 = w2.Pid();
    w2.Exec("BEGIN");
    w2.Exec("DELETE FROM lineitem WHERE ctid IN (SELECT ctid FROM lineitem LIMIT 10)");
    admin.Exec("SELECT pg_terminate_backend(" + std::to_string(pid2) + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (auto e = Check(reader, q, "after the second kill"); !e.empty())
      throw std::runtime_error(e);
    info = StatsLine(s1);
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("kill_backend", err, info);
}

void RestartWorker(const Tools &tools, int seconds, uint64_t seed) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const Q q1 = MakeQ(1), q12 = MakeQ(12);
    Conn a(tools, kDb);
    for (const auto *q : {&q1, &q12})
      if (auto e = Check(a, *q, "warm"); !e.empty())
        throw std::runtime_error(e);
    const Stats s0 = ReadStats(a);
    const int gen0 = static_cast<int>(s0["generation"]);
    // Restart while readers and a writer are active.
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::vector<std::string> failures;
    auto fail = [&](const std::string &m) {
      std::lock_guard<std::mutex> l(mu);
      if (failures.size() < 5)
        failures.push_back(m);
    };
    std::vector<std::thread> threads;
    for (int r = 0; r < 3; r++) {
      threads.emplace_back([&, r] {
        try {
          Conn c(tools, kDb);
          while (!stop.load()) {
            const Q &q = (r % 2) ? q12 : q1;
            c.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
            const auto ref = c.Text(q.ref);
            const auto accel = c.Text(q.accel);
            c.Exec("COMMIT");
            if (auto e = MatchError(ref, accel, "reader during restart"); !e.empty())
              fail(e);
          }
        } catch (const std::exception &e) {
          fail(std::string("reader error: ") + e.what());
        }
      });
    }
    threads.emplace_back([&] {
      try {
        Conn c(tools, kDb);
        ts::TpchFuzzWrites gen(ts::FuzzDialect::Postgres, seed * 7 + 5,
                               [&](const std::string &sql) { return c.Scalar(sql); });
        while (!stop.load()) {
          c.Exec("BEGIN");
          const auto stmt = gen.Next();
          std::size_t pos = 0;
          bool ok = true;
          while (ok) {
            const auto end = stmt.find(";\n", pos);
            if (!c.TryExec(stmt.substr(pos, end == std::string::npos ? std::string::npos : end - pos)).empty())
              ok = false;
            if (end == std::string::npos)
              break;
            pos = end + 2;
          }
          c.Exec(ok ? "COMMIT" : "ROLLBACK");
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      } catch (const std::exception &e) {
        fail(std::string("writer error: ") + e.what());
      }
    });
    int restarts = 0;
    for (int s = 0; s < seconds; s++) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (s % 3 == 1) {
        StopWorker(admin, kDb);
        restarts++;
      }
    }
    stop = true;
    for (auto &t : threads)
      t.join();
    if (!failures.empty())
      throw std::runtime_error(failures.front());
    // Final: a new worker (or the surviving one) answers exactly and hits.
    Conn c(tools, kDb);
    for (const auto *q : {&q1, &q12}) {
      if (auto e = Check(c, *q, "after restarts"); !e.empty())
        throw std::runtime_error(e);
    }
    const auto b = c.Counts();
    for (const auto *q : {&q1, &q12}) {
      if (auto e = Check(c, *q, "after restarts (again)"); !e.empty())
        throw std::runtime_error(e);
    }
    const auto a2 = c.Counts();
    Require(a2[0] - b[0] == 2 && a2[1] == b[1] && a2[2] == b[2], "a restarted worker should serve hits once rebuilt");
    const Stats s1 = ReadStats(c);
    Require(s1["generation"] > gen0, "the worker did not restart (generation " + std::to_string(s1["generation"]) + ")");
    info = std::to_string(restarts) + " restarts; generation " + std::to_string(gen0) + " -> " +
           std::to_string(s1["generation"]) + "; " + StatsLine(s1);
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("restart_worker", err, info);
}

// A writer committing continuously, readers (REPEATABLE READ, so the reference and the
// accelerator see one snapshot) reading at once: nothing wrong, and nearly everything served.
void HitsUnderWrites(const Tools &tools, int seconds, uint64_t seed) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const std::vector<Q> qs = {MakeQ(6), MakeQ(14)};
    {
      Conn warm(tools, kDb);
      for (const auto &q : qs)
        if (auto e = Check(warm, q, "warm"); !e.empty())
          throw std::runtime_error(e);
    }
    const Stats before = [&] { Conn c(tools, kDb); return ReadStats(c); }();
    std::mutex mu;
    std::vector<std::string> failures;
    auto fail = [&](const std::string &m) {
      std::lock_guard<std::mutex> l(mu);
      if (failures.size() < 5)
        failures.push_back(m);
    };
    std::atomic<bool> stop{false};
    std::atomic<long> commits{0}, reads{0};
    std::array<std::atomic<long>, 3> counts{};
    for (auto &c : counts)
      c = 0;
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
      try {
        Conn c(tools, kDb);
        std::mt19937_64 rng(seed);
        while (!stop.load()) {
          // Answers change: Q6 and Q14 read l_extendedprice.
          c.Exec("UPDATE lineitem SET l_extendedprice = l_extendedprice + 1 WHERE l_orderkey = " +
                 std::to_string(1 + rng() % 6000000));
          commits++;
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      } catch (const std::exception &e) {
        fail(std::string("writer error: ") + e.what());
      }
    });
    for (int r = 0; r < 4; r++) {
      threads.emplace_back([&, r] {
        try {
          Conn c(tools, kDb);
          std::mt19937_64 rng(seed * 31 + static_cast<uint64_t>(r));
          while (!stop.load()) {
            const Q &q = qs[rng() % qs.size()];
            c.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
            const auto ref = c.Text(q.ref);
            const auto accel = c.Text(q.accel);
            c.Exec("COMMIT");
            reads++;
            if (auto e = MatchError(ref, accel, "reader Q" + std::to_string(q.n)); !e.empty())
              fail("reader " + std::to_string(r) + ": " + e);
          }
          const auto k = c.Counts();
          for (int i = 0; i < 3; i++)
            counts[i] += k[i];
        } catch (const std::exception &e) {
          fail(std::string("reader error: ") + e.what());
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    for (auto &t : threads)
      t.join();
    if (!failures.empty())
      throw std::runtime_error(failures.front());
    Conn c(tools, kDb);
    const Stats after = ReadStats(c);
    const long total = counts[0] + counts[2];
    std::ostringstream o;
    o << commits << " commits, " << reads << " reads: hit=" << counts[0] << " fallback=" << counts[2]
      << "; builds " << before["builds"] << " -> " << after["builds"] << "; " << StatsLine(after);
    info = o.str();
    Require(commits > 0 && total > 0, "no work done");
    // A reader whose snapshot lies inside a range the worker applied as one combined change cannot
    // be served at its epoch (it answers from SQL, exactly); the rest are hits.
    Require(counts[0] * 2 >= total, "under continuous writes fewer than half of the reads were hits: " + info);
    Require(after["builds"] == before["builds"], "continuous writes forced a rebuild: " + info);
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("hits_under_writes", err, info);
}

void ReadAfterOwnCommit(const Tools &tools) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const Q q = MakeQ(6);
    Conn c(tools, kDb);
    if (auto e = Check(c, q, "warm"); !e.empty())
      throw std::runtime_error(e);
    const auto before = c.Counts();
    for (int i = 0; i < 40; i++) {
      c.Exec("UPDATE lineitem SET l_extendedprice = l_extendedprice + 7 WHERE l_orderkey = " + std::to_string(1 + i * 37));
      if (auto e = Check(c, q, "after own commit " + std::to_string(i)); !e.empty())
        throw std::runtime_error(e);
    }
    const auto after = c.Counts();
    Require(after[0] - before[0] == 40 && after[2] == before[2], "reads right after a commit should all be hits");
    info = "40 commits, each read right after it";
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("read_after_own_commit", err, info);
}

void OlderSnapshot(const Tools &tools) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const Q q = MakeQ(6);
    Conn a(tools, kDb), w(tools, kDb);
    if (auto e = Check(a, q, "warm"); !e.empty())
      throw std::runtime_error(e);
    a.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ");
    const auto ref0 = a.Text(q.ref);
    long newer_hits = 0;
    for (int i = 0; i < 5; i++) {
      w.Exec("UPDATE lineitem SET l_extendedprice = l_extendedprice + 100 WHERE l_orderkey = " + std::to_string(1 + i * 53));
      // Another connection, at the newest epoch.
      if (auto e = Check(w, q, "newer snapshot " + std::to_string(i)); !e.empty())
        throw std::runtime_error(e);
      newer_hits++;
    }
    // `a` still sees the world as of its snapshot, whatever the worker holds now.
    const auto before = a.Counts();
    const auto accel0 = a.Text(q.accel);
    const auto after = a.Counts();
    a.Exec("COMMIT");
    if (auto e = MatchError(ref0, accel0, "older snapshot"); !e.empty())
      throw std::runtime_error(e);
    info = std::string("older snapshot answered from ") + (after[0] > before[0] ? "its epoch's version (hit)" : "SQL (fallback)");
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("older_snapshot", err, info);
}

void AbortAndSavepoint(const Tools &tools) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const Q q = MakeQ(6);
    Conn c(tools, kDb);
    if (auto e = Check(c, q, "warm"); !e.empty())
      throw std::runtime_error(e);
    const Stats s0 = ReadStats(c);
    const auto before = c.Counts();
    c.Exec("BEGIN");
    c.Exec("UPDATE lineitem SET l_extendedprice = l_extendedprice + 5 WHERE l_orderkey < 2000");
    c.Exec("ROLLBACK");
    if (auto e = Check(c, q, "after rollback"); !e.empty())
      throw std::runtime_error(e);
    c.Exec("BEGIN");
    c.Exec("SAVEPOINT s");
    c.Exec("UPDATE lineitem SET l_extendedprice = l_extendedprice + 5 WHERE l_orderkey < 2000");
    c.Exec("ROLLBACK TO SAVEPOINT s");
    c.Exec("SAVEPOINT t");
    c.Exec("UPDATE lineitem SET l_extendedprice = l_extendedprice + 9 WHERE l_orderkey = 3");
    c.Exec("RELEASE SAVEPOINT t");
    c.Exec("COMMIT");
    if (auto e = Check(c, q, "after savepoints"); !e.empty())
      throw std::runtime_error(e);
    const auto after = c.Counts();
    const Stats s1 = ReadStats(c);
    Require(after[0] - before[0] == 2 && after[2] == before[2], "reads after aborts should be hits");
    Require(s1["builds"] == s0["builds"] && s1["gaps"] == s0["gaps"], "an abort cost a rebuild: " + StatsLine(s1));
    info = StatsLine(s1);
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("abort_and_savepoint", err, info);
}

void TruncateRebuild(const Tools &tools) {
  std::string err, info;
  try {
    Conn admin(tools, "postgres");
    StopWorker(admin, kDb);
    const Q q = MakeQ(5);
    Conn c(tools, kDb);
    if (auto e = Check(c, q, "warm"); !e.empty())
      throw std::runtime_error(e);
    const Stats s0 = ReadStats(c);
    c.Exec("BEGIN");
    c.Exec("CREATE TEMP TABLE nation_copy AS SELECT * FROM nation");
    c.Exec("TRUNCATE nation");
    c.Exec("INSERT INTO nation SELECT * FROM nation_copy");
    c.Exec("COMMIT");
    // Not maintainable: the state is rebuilt. Answers are exact meanwhile.
    for (int i = 0; i < 5; i++)
      if (auto e = Check(c, q, "after truncate"); !e.empty())
        throw std::runtime_error(e);
    Stats s1 = ReadStats(c);
    for (int i = 0; i < 600 && s1["builds"] == s0["builds"]; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      s1 = ReadStats(c);
    }
    Require(s1["builds"] == s0["builds"] + 1, "TRUNCATE should have caused one rebuild: " + StatsLine(s1));
    const auto before = c.Counts();
    if (auto e = Check(c, q, "after the rebuild"); !e.empty())
      throw std::runtime_error(e);
    Require(c.Counts()[0] > before[0], "the rebuilt state should serve");
    info = StatsLine(s1);
  } catch (const std::exception &e) {
    err = e.what();
  }
  Report("truncate_rebuild", err, info);
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::string which = argc > 1 ? argv[1] : "all";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 12;
    const uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);
    MakeDb(tools);
    auto run = [&](const char *name) { return which == "all" || which == name; };
    if (run("hit_across_connections"))
      HitAcrossConnections(tools);
    if (run("memory"))
      Memory(tools);
    if (run("concurrent"))
      Concurrent(tools, seconds, seed);
    if (run("hits_under_writes"))
      HitsUnderWrites(tools, seconds, seed);
    if (run("read_after_own_commit"))
      ReadAfterOwnCommit(tools);
    if (run("older_snapshot"))
      OlderSnapshot(tools);
    if (run("abort_and_savepoint"))
      AbortAndSavepoint(tools);
    if (run("truncate_rebuild"))
      TruncateRebuild(tools);
    if (run("kill_backend"))
      KillBackend(tools);
    if (run("restart_worker"))
      RestartWorker(tools, seconds, seed);
    if (g_failures == 0 || !std::getenv("ACCEL_SHARED_TEST_KEEP")) {
      Conn admin(tools, "postgres");
      StopWorker(admin, kDb); // workers hold a connection to the database
      admin.Exec("DROP DATABASE IF EXISTS " + kDb + " WITH (FORCE)");
    }
    std::cout << (g_failures == 0 ? "ALL PASS" : std::to_string(g_failures) + " scenarios FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "test_postgres_shared_state failed: " << e.what() << std::endl;
    return 1;
  }
}
