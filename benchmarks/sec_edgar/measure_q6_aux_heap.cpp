// Live-heap (glibc mallinfo2) footprint of the SEC EDGAR Q6 aux state.
// Warms the DuckDB buffer pool with the base query first so the delta around
// Build() is dominated by aux state, then reports in-use heap before/after.
// Uses only the public DuckdbRuntime API so it can be linked against either
// the old or the new aux implementation.
//
// usage: measure_q6_aux_heap <db_path> [threads]
#include <malloc.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace catalog = accelerator_codegen::aux_state::engine;
using catalog::duckdb::DuckdbRuntime;

static double HeapMb() {
  malloc_trim(0);
  return static_cast<double>(mallinfo2().uordblks) / (1024.0 * 1024.0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: measure_q6_aux_heap <db_path> [threads]\n";
    return 2;
  }
  duckdb::DuckDB database(argv[1]);
  duckdb::Connection connection(database);
  connection.Query(std::string("PRAGMA threads=") + (argc > 2 ? argv[2] : "1"));
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 6);
  auto warm = connection.Query(entry.base_sql); // populate the buffer pool
  warm.reset();
  const double before = HeapMb();
  {
    DuckdbRuntime runtime(connection, entry, 10.0);
    const auto t0 = std::chrono::steady_clock::now();
    runtime.Build();
    const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double after = HeapMb();
    const auto rows = runtime.ExecuteCollect().size();
    std::cout << "heap_before_mb=" << before << " heap_after_build_mb=" << after
              << " aux_heap_mb=" << (after - before)
              << " footprint_mb=" << runtime.AuxStateFootprintBytes() / (1024.0 * 1024.0)
              << " build_s=" << build_s << " rows=" << rows << "\n";
  }
  return 0;
}
