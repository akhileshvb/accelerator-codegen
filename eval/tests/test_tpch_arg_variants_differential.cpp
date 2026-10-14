#include "eval/harness/differential.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "eval/bench/tpch/single-threaded/tpch_arg_variants.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace aux_sweep = accelerator_codegen::benchmarks::aux_sweep;
using catalog::duckdb::DuckdbRuntime;

constexpr int kDefaultSamples = 8;
constexpr int kQueries[] = {1, 3, 6, 9, 10, 11, 12, 14, 16, 17, 18, 21};

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      throw std::runtime_error(
          "usage: test_tpch_arg_variants_differential <tpch.duckdb> [samples]");
    }
    const std::string db_path = argv[1];
    const int samples =
        argc >= 3 ? std::max(1, std::stoi(argv[2])) : kDefaultSamples;

    setenv("ACCEL_BENCH_VARY_ARGS", "1", 0);
    setenv("ACCEL_BENCH_ARG_SEED", "42", 0);

    duckdb::DuckDB database(db_path.c_str());
    duckdb::Connection connection(database);
    Require(connection.Query("PRAGMA threads=1") != nullptr, "pragma failed");

    for (const int query_id : kQueries) {
      const auto &entry = catalog::LookupSharedQuery("tpch", query_id);
      auto pool = aux_sweep::BuildTpchArgPool(query_id, entry);
      if (static_cast<int>(pool.size()) > samples) {
        pool.resize(static_cast<std::size_t>(samples));
      }

      std::cerr << "arg-variant differential: tpch Q" << query_id
                << " samples=" << pool.size() << '\n'
                << std::flush;

      DuckdbRuntime runtime(connection, entry, 10.0);
      runtime.Build();

      harness::RequireSharedMatchesBase(connection, runtime, entry);

      for (std::size_t i = 0; i < pool.size(); ++i) {
        const auto &variant = pool[i];
        try {
          harness::CompareAuxRowsToSql(connection, entry, variant.base_sql,
                                       runtime.ExecuteCollect(variant.arguments));
        } catch (const std::exception &ex) {
          throw std::runtime_error(std::string(ex.what()) + " (Q" +
                                   std::to_string(query_id) + " variant=" +
                                   std::to_string(i) + " sql=" +
                                   variant.kernel_sql + ")");
        }
      }
      std::cerr << "PASS tpch Q" << query_id << '\n' << std::flush;
    }

    std::cerr << "PASS all arg-variant differentials\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "test_tpch_arg_variants_differential failed: " << ex.what()
              << '\n';
    return 1;
  }
}
