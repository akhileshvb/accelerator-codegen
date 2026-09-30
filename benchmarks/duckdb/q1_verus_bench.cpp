// TPC-H Q1 on DuckDB, stateless: base SQL vs the Verus-verified kernel
// (accel_tpch_q1_verus) vs the C++ fused kernel (accel_tpch_q1).
//
//   q1_verus_bench <db-path> [warmup=3] [iters=10] [threads=1]
//
// For each seed parameter (the Q1 manifest's day offsets):
//   1. differential check: the Verus accelerator's rows vs the SQL's rows
//      (text columns and DECIMAL sums and counts exactly, AVG doubles to a
//      relative 1e-12);
//   2. timings: median / mean / min over `iters` runs after `warmup` runs,
//      for SQL at `threads`, the C++ accelerator and the Verus accelerators
//      (single-threaded kernels: blocked and simple), then SQL at DuckDB's
//      default thread count and, when built with ACCEL_ENABLE_PARALLEL_TPCH,
//      the parallel C++ and Verus accelerators at that thread count.
// Latencies are client-observed and include materializing the result.
// Output: CSV on stdout.
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

std::string Q1Sql(int offset) {
    return "SELECT l_returnflag, l_linestatus, sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
           "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
           "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, avg(l_quantity) AS avg_qty, "
           "avg(l_extendedprice) AS avg_price, avg(l_discount) AS avg_disc, count(*) AS count_order "
           "FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '" +
           std::to_string(offset) +
           "' DAY GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";
}

std::unique_ptr<duckdb::MaterializedQueryResult> Run(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (r->HasError()) {
        std::cerr << "query failed: " << sql << "\n" << r->GetError() << "\n";
        std::exit(1);
    }
    return r;
}

bool SameRows(duckdb::MaterializedQueryResult &a, duckdb::MaterializedQueryResult &b, std::string &why) {
    if (a.RowCount() != b.RowCount()) {
        why = "row count " + std::to_string(a.RowCount()) + " vs " + std::to_string(b.RowCount());
        return false;
    }
    for (duckdb::idx_t r = 0; r < a.RowCount(); r++) {
        for (duckdb::idx_t c = 0; c < 10; c++) {
            auto x = a.GetValue(c, r);
            auto y = b.GetValue(c, r);
            if (c >= 6 && c <= 8) {
                const double dx = x.GetValue<double>(), dy = y.GetValue<double>();
                if (std::fabs(dx - dy) > 1e-12 * std::max(std::fabs(dx), 1.0)) {
                    why = "row " + std::to_string(r) + " col " + std::to_string(c) + ": " + x.ToString() +
                          " vs " + y.ToString();
                    return false;
                }
            } else if (x.ToString() != y.ToString()) {
                why = "row " + std::to_string(r) + " col " + std::to_string(c) + ": " + x.ToString() + " vs " +
                      y.ToString();
                return false;
            }
        }
    }
    return true;
}

struct Stats {
    double median, mean, min;
};

Stats Time(duckdb::Connection &con, const std::string &sql, int warmup, int iters) {
    for (int i = 0; i < warmup; i++) {
        Run(con, sql);
    }
    std::vector<double> ms;
    for (int i = 0; i < iters; i++) {
        auto t0 = Clock::now();
        auto r = Run(con, sql);
        for (duckdb::idx_t row = 0; row < r->RowCount(); row++) {
            for (duckdb::idx_t c = 0; c < r->ColumnCount(); c++) {
                (void)r->GetValue(c, row);
            }
        }
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::vector<double> s = ms;
    std::sort(s.begin(), s.end());
    double mean = 0;
    for (double x : ms) {
        mean += x;
    }
    return {s[s.size() / 2], mean / ms.size(), s.front()};
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: q1_verus_bench <db-path> [warmup=3] [iters=10] [threads=1]\n";
        return 2;
    }
    const std::string path = argv[1];
    const int warmup = argc > 2 ? std::atoi(argv[2]) : 3;
    const int iters = argc > 3 ? std::atoi(argv[3]) : 10;
    const int threads = argc > 4 ? std::atoi(argv[4]) : 1;

    duckdb::DBConfig config;
    config.options.access_mode = duckdb::AccessMode::READ_ONLY;
    duckdb::DuckDB db(path, &config);
    duckdb::Connection con(db);
    accelerator_codegen::RegisterAcceleratorFunctions(con);
    auto default_threads = Run(con, "SELECT current_setting('threads')")->GetValue(0, 0).ToString();

    const int offsets[] = {103, 105, 90, 98, 104};
    std::cout << "day_offset,system,threads,median_ms,mean_ms,min_ms,matches_sql\n";
    bool all_ok = true;
    for (int off : offsets) {
        const std::string sql = Q1Sql(off);
        const std::string verus = "SELECT * FROM accel_tpch_q1_verus(" + std::to_string(off) + ")";
        const std::string cpp = "SELECT * FROM accel_tpch_q1(" + std::to_string(off) + ")";
        const std::string simple = "SELECT * FROM accel_tpch_q1_verus_simple(" + std::to_string(off) + ")";

        Run(con, "PRAGMA threads=" + std::to_string(threads));
        auto ref = Run(con, sql);
        std::string why_v, why_c;
        const bool ok_v = SameRows(*ref, *Run(con, verus), why_v);
        const bool ok_c = SameRows(*ref, *Run(con, cpp), why_c);
        std::string why_s;
        const bool ok_s = SameRows(*ref, *Run(con, simple), why_s);
        if (!ok_s) {
            std::cerr << "MISMATCH verus_simple offset " << off << ": " << why_s << "\n";
        }
        all_ok = all_ok && ok_s;
        if (!ok_v) {
            std::cerr << "MISMATCH verus offset " << off << ": " << why_v << "\n";
        }
        if (!ok_c) {
            std::cerr << "MISMATCH cpp offset " << off << ": " << why_c << "\n";
        }
        all_ok = all_ok && ok_v;

        auto emit = [&](const char *sys, const std::string &thr, const Stats &s, const char *match) {
            std::printf("%d,%s,%s,%.3f,%.3f,%.3f,%s\n", off, sys, thr.c_str(), s.median, s.mean, s.min, match);
            std::fflush(stdout);
        };
        const char *only = std::getenv("Q1_BENCH_ONLY");
        if (only && std::string(only) == "verus") {
            emit("accel_verus", std::to_string(threads), Time(con, verus, warmup, iters), ok_v ? "true" : "false");
            continue;
        }
        emit("duckdb_sql", std::to_string(threads), Time(con, sql, warmup, iters), "ref");
        emit("accel_cpp", std::to_string(threads), Time(con, cpp, warmup, iters), ok_c ? "true" : "false");
        emit("accel_verus", std::to_string(threads), Time(con, verus, warmup, iters), ok_v ? "true" : "false");
        emit("accel_verus_simple", std::to_string(threads), Time(con, simple, warmup, iters), ok_s ? "true" : "false");
        Run(con, "PRAGMA threads=" + default_threads);
        emit("duckdb_sql", default_threads, Time(con, sql, warmup, iters), "ref");
#ifdef ACCEL_ENABLE_PARALLEL_TPCH
        const std::string cpp_par = "SELECT * FROM accel_tpch_q1_parallel(" + std::to_string(off) + ")";
        const std::string verus_par = "SELECT * FROM accel_tpch_q1_verus_parallel(" + std::to_string(off) + ")";
        std::string why_cp, why_vp;
        const bool ok_cp = SameRows(*ref, *Run(con, cpp_par), why_cp);
        const bool ok_vp = SameRows(*ref, *Run(con, verus_par), why_vp);
        if (!ok_vp) {
            std::cerr << "MISMATCH verus_parallel offset " << off << ": " << why_vp << "\n";
        }
        all_ok = all_ok && ok_vp;
        emit("accel_cpp_parallel", default_threads, Time(con, cpp_par, warmup, iters), ok_cp ? "true" : "false");
        emit("accel_verus_parallel", default_threads, Time(con, verus_par, warmup, iters), ok_vp ? "true" : "false");
#endif
    }
    return all_ok ? 0 : 1;
}
