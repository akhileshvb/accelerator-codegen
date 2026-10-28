// SEC-EDGAR Q2 on PostgreSQL: accelerator vs plain SQL, build time and write
// overhead of the mutation capture.
//   benchmark_postgres_sec_edgar_q2 [reads=300] [plain_reads=3] [writes=200]
// Reads run for each (fy, uom) set drawn by eval/harness/param_source (ACCEL_PARAM_SEED=<s>
// reproduces a run); the write loops insert rows of the first set.
// Connects to the PG* environment (default 127.0.0.1:55402 / sec_edgar).
#include <algorithm>
#include <chrono>
#include <iomanip>

#include "sec_edgar/q2/tests/postgres_sec_edgar_q2_common.hpp"

using namespace q2pg;
using Clock = std::chrono::steady_clock;

namespace {
double Ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
double P50(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[v.size() / 2];
}
double P95(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[std::min(v.size() - 1, v.size() * 95 / 100)];
}
} // namespace

int main(int argc, char **argv) {
  const int reads = argc > 1 ? std::atoi(argv[1]) : 300;
  const int plain_reads = argc > 2 ? std::atoi(argv[2]) : 3;
  const int writes = argc > 3 ? std::atoi(argv[3]) : 200;
  try {
    Conn c(Env("PGDATABASE", "sec_edgar"));
    c.Exec("SET max_parallel_workers_per_gather = 4");
    std::cout << std::unitbuf << std::fixed << std::setprecision(3);

    const auto &sets = DrawnParams(c);
    const Params &first = sets.front();
    auto t0 = Clock::now();
    c.Query(AccelSql(), {first.fy.c_str(), first.uom.c_str()});
    std::cout << "build (first call): " << Ms(t0, Clock::now()) << " ms\n";

    for (const auto &set : sets) {
      const std::vector<const char *> params{set.fy.c_str(), set.uom.c_str()};
      std::vector<double> accel, plain;
      for (int i = 0; i < reads; ++i) {
        auto a = Clock::now();
        c.Query(AccelSql(), params);
        accel.push_back(Ms(a, Clock::now()));
      }
      for (int i = 0; i < plain_reads; ++i) {
        auto a = Clock::now();
        c.Query(PlainSql(), params);
        plain.push_back(Ms(a, Clock::now()));
      }
      std::cout << "read fy=" << set.fy << " uom=" << set.uom << " accel p50=" << P50(accel) << " ms p95=" << P95(accel)
                << " ms | plain SQL p50=" << P50(plain) << " ms | speedup x"
                << P50(plain) / std::max(P50(accel), 1e-6) << "\n";
    }

    // Write overhead: statement-level single-row writes with the capture
    // triggers (state maintained) vs the same statements with them disabled.
    const auto write_loop = [&](bool triggers) {
      c.Exec(triggers ? "ALTER TABLE num ENABLE TRIGGER USER" : "ALTER TABLE num DISABLE TRIGGER USER");
      c.Query(AccelSql(), {first.fy.c_str(), first.uom.c_str()}); // state valid before the loop
      std::vector<double> ins, upd, del;
      for (int i = 0; i < writes; ++i) {
        const std::string tag = "q2bench" + std::to_string(i);
        auto a = Clock::now();
        const Rows ret = c.Query(
            "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) "
            "SELECT adsh, '" + tag + "', 'w', 20220101, 4, $1::text, 1e15 + " + std::to_string(i) +
                " FROM sub WHERE fy = $2::bigint LIMIT 1 RETURNING ctid::text",
            {first.uom.c_str(), first.fy.c_str()});
        std::string ctid = ret.at(0).at(0).text;
        auto b = Clock::now();
        ctid = c.Query("UPDATE num SET value = value + 1 WHERE ctid = '" + ctid +
                       "'::tid RETURNING ctid::text").at(0).at(0).text;
        auto d = Clock::now();
        c.Exec("DELETE FROM num WHERE ctid = '" + ctid + "'::tid");
        auto e = Clock::now();
        ins.push_back(Ms(a, b));
        upd.push_back(Ms(b, d));
        del.push_back(Ms(d, e));
      }
      std::cout << "write (triggers " << (triggers ? "on " : "off") << ") insert p50=" << P50(ins)
                << " ms update p50=" << P50(upd) << " ms delete p50=" << P50(del) << " ms\n";
    };
    write_loop(false);
    write_loop(true);
    c.Exec("ALTER TABLE num ENABLE TRIGGER USER");
    // The loops restore the table; make sure the state is still exact.
    std::cout << "post-check: " << (Compare(c, c, first.fy.c_str(), first.uom.c_str()).empty() ? "ok" : "MISMATCH")
              << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
}
