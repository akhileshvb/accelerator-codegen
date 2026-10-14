// SEC EDGAR Q1 on PostgreSQL: accelerator (accel_sec_edgar_q1_aux) vs plain SQL.
//   read   [reps]   read latency p50/p95/min of both
//   build  [reps]   first accelerator call of a fresh session (state build + answer)
//   write  [stmts]  write cost with the capture trigger off / on (no accelerator in the
//                   session) / on with the accelerator active in the session, for
//                   statements touching 1, 100 and 10000 rows; plus the read latency
//                   right after a write
// Env: Q1_PG_CONN (default host=127.0.0.1 port=55401 user=postgres password=postgres dbname=sec_edgar)
// The write mode leaves `pre` unchanged except for dead tuples (line is changed by +1/-1 alternately).
#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string ConnInfo() {
  const char *c = std::getenv("Q1_PG_CONN");
  return c ? c : "host=127.0.0.1 port=55401 user=postgres password=postgres dbname=sec_edgar";
}

struct Conn {
  PGconn *c;
  Conn() : c(PQconnectdb(ConnInfo().c_str())) {
    if (PQstatus(c) != CONNECTION_OK) throw std::runtime_error(PQerrorMessage(c));
  }
  ~Conn() { PQfinish(c); }
  std::string Exec(const std::string &sql) {
    PGresult *r = PQexec(c, sql.c_str());
    const auto st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
      std::string m = PQresultErrorMessage(r);
      PQclear(r);
      throw std::runtime_error(sql.substr(0, 120) + ": " + m);
    }
    std::string out;
    if (st == PGRES_TUPLES_OK && PQntuples(r) > 0 && PQnfields(r) > 0 && !PQgetisnull(r, 0, 0)) out = PQgetvalue(r, 0, 0);
    PQclear(r);
    return out;
  }
};

using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Stats {
  double p50, p95, min;
};
Stats Summarize(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return {v[v.size() / 2], v[std::min(v.size() - 1, (std::size_t)(v.size() * 0.95))], v.front()};
}

const char *kSql =
    "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, AVG(line) AS avg_line_num "
    "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC";
const char *kAccel = "SELECT * FROM accel_sec_edgar_q1_aux()";

template <typename F>
std::vector<double> Time(int reps, F &&f) {
  std::vector<double> t;
  for (int i = 0; i < reps; ++i) {
    const auto a = Clock::now();
    f();
    t.push_back(Ms(a, Clock::now()));
  }
  return t;
}

void Read(int reps) {
  Conn c;
  c.Exec("SET max_parallel_workers_per_gather = 0"); // single-process SQL baseline
  c.Exec(kAccel);                                    // build
  const auto acc = Summarize(Time(reps, [&] { c.Exec(kAccel); }));
  const auto sql1 = Summarize(Time(std::max(3, reps / 10), [&] { c.Exec(kSql); }));
  c.Exec("SET max_parallel_workers_per_gather = 2");
  const auto sqlp = Summarize(Time(std::max(3, reps / 10), [&] { c.Exec(kSql); }));
  std::printf("read latency (ms)        p50        p95        min\n");
  std::printf("accelerator        %10.3f %10.3f %10.3f\n", acc.p50, acc.p95, acc.min);
  std::printf("SQL (no parallel)  %10.3f %10.3f %10.3f\n", sql1.p50, sql1.p95, sql1.min);
  std::printf("SQL (parallel x2)  %10.3f %10.3f %10.3f\n", sqlp.p50, sqlp.p95, sqlp.min);
  std::printf("speedup p50: %.0fx vs non-parallel SQL, %.0fx vs parallel SQL\n", sql1.p50 / acc.p50, sqlp.p50 / acc.p50);
}

void Build(int reps) {
  std::vector<double> first, build_only;
  for (int i = 0; i < reps; ++i) {
    Conn c;
    const auto a = Clock::now();
    c.Exec(kAccel);
    first.push_back(Ms(a, Clock::now()));
  }
  const auto s = Summarize(first);
  std::printf("state build (first call of a fresh session, incl. answer): p50 %.0f ms, min %.0f ms (%d runs)\n", s.p50,
              s.min, reps);
}


// One statement touching ~rows rows: line + delta on rows addressed by ctid; returns the new ctids.
struct Writer {
  Conn &c;
  std::string ctids; // array literal of current ctids
  int delta = 1;
  Writer(Conn &conn, int rows) : c(conn) {
    ctids = c.Exec("SELECT array_agg(ctid)::text FROM (SELECT ctid FROM pre WHERE stmt IS NOT NULL AND line IS NOT NULL "
                   "LIMIT " + std::to_string(rows) + ") s");
  }
  void Step() {
    ctids = c.Exec("WITH t AS (UPDATE pre SET line = line + (" + std::to_string(delta) + ") WHERE ctid = ANY('" + ctids +
                   "'::tid[]) RETURNING ctid) SELECT array_agg(ctid)::text FROM t");
    delta = -delta;
  }
};

void Write(int stmts) {
  struct Cfg { const char *name; bool trigger; bool accel; };
  const Cfg cfgs[] = {{"plain write (trigger off)", false, false},
                      {"trigger on, no accelerator in session", true, false},
                      {"trigger on, accelerator active in session", true, true}};
  std::printf("%-44s %8s %12s %12s %12s\n", "configuration", "rows/stmt", "ms/stmt", "stmts/s", "read-after ms");
  for (int rows : {1, 100, 10000}) {
    const int n = rows >= 10000 ? std::max(5, stmts / 20) : rows >= 100 ? std::max(10, stmts / 4) : stmts;
    double base_ms = 0;
    for (const auto &cfg : cfgs) {
      Conn c;
      c.Exec(std::string("ALTER TABLE pre ") + (cfg.trigger ? "ENABLE" : "DISABLE") + " TRIGGER USER");
      if (cfg.accel) c.Exec(kAccel);
      Writer w(c, rows);
      for (int i = 0; i < 3; ++i) w.Step(); // warm-up (even count keeps net zero)
      std::vector<double> t, rd;
      for (int i = 0; i < n; ++i) {
        const auto a = Clock::now();
        w.Step();
        t.push_back(Ms(a, Clock::now()));
        if (cfg.accel && i % 5 == 0) {
          const auto b = Clock::now();
          c.Exec(kAccel);
          rd.push_back(Ms(b, Clock::now()));
        }
      }
      if (w.delta == -1) w.Step(); // odd number of steps: restore
      c.Exec("ALTER TABLE pre ENABLE TRIGGER USER");
      const double ms = Summarize(t).p50;
      if (!cfg.trigger) base_ms = ms;
      std::printf("%-44s %8d %12.3f %12.0f %12s", cfg.name, rows, ms, 1000.0 / ms,
                  rd.empty() ? "-" : (std::to_string(Summarize(rd).p50)).c_str());
      if (cfg.trigger) std::printf("   (+%.3f ms, %.2fx)", ms - base_ms, ms / base_ms);
      std::printf("\n");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::string mode = argc > 1 ? argv[1] : "read";
    const int n = argc > 2 ? std::atoi(argv[2]) : 0;
    if (mode == "read") Read(n ? n : 200);
    else if (mode == "build") Build(n ? n : 5);
    else if (mode == "write") Write(n ? n : 200);
    else { std::cerr << "usage: read|build|write [n]\n"; return 2; }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    return 1;
  }
}
