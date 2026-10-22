// Shared DuckDB TPC-H aux bench: any query, one binary.
// Usage: benchmark_duckdb_tpch_aux --query N
// DB from ACCEL_TPCH_DB; substitution parameters are drawn per run
// (ACCEL_BENCH_ARG_SEED, ACCEL_BENCH_VARY_ARGS); write schedule from
// ACCEL_AUX_WRITE_FREQUENCIES / ACCEL_AUX_WRITE_INTERVALS, else ACCEL_Q<N>_WRITE_FREQUENCY_PCT.
#include "backends/duckdb/bench/aux_benchmark_sweep_session.hpp"
#include "eval/bench/driver/bench_cli.hpp"
#include "eval/tests/test_support/db_paths.hpp"

#include <cstdlib>
#include <iostream>

namespace driver = accelerator_codegen::benchmarks::driver;
namespace aux_sweep = accelerator_codegen::benchmarks::aux_sweep;

int main(int argc, char **argv) {
  try {
    const driver::Args args(argc, argv);
    const int query = args.query;
    if (!std::getenv("ACCEL_AUX_WRITE_INTERVALS") && !std::getenv("ACCEL_AUX_WRITE_FREQUENCIES")) {
      setenv("ACCEL_AUX_WRITE_FREQUENCIES",
             std::to_string(driver::EnvInt(driver::QueryEnvName(query, "WRITE_FREQUENCY_PCT").c_str(), 0)).c_str(), 1);
    }
    aux_sweep::RunConfiguredMultiSweep(
        query, accelerator_codegen::test_support::TpchDbPath(),
        driver::EnvString(driver::QueryEnvName(query, "BENCH_SUITE").c_str(), "lineitem_only"),
        driver::EnvString(driver::QueryEnvName(query, "WRITE_LOCALITY").c_str(), "global"),
        driver::EnvInt("ACCEL_BENCH_WARMUP", 10), driver::EnvInt("ACCEL_BENCH_QUERIES", 100),
        driver::EnvInt("ACCEL_BENCH_REF_SAMPLES", 5), driver::EnvInt("ACCEL_BENCH_BASE_WRITE_SAMPLES", 3));
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "benchmark_duckdb_tpch_aux failed: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
