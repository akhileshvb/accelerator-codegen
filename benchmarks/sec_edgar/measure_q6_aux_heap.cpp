// Live-heap (glibc mallinfo2) footprint of the SEC EDGAR Q6 aux state.
// Warms the DuckDB buffer pool with the base query first so the delta around
// Build() is dominated by aux state, then reports in-use heap before/after.
// footprint_mb is the state's own accounting (SecEdgarQ6State::Bytes) of the
// state a fresh module builds from the same scan.
//
// usage: measure_q6_aux_heap <db_path> [threads]
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "sec_edgar/q6/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q6/duckdb/aux/module.hpp"

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
    namespace q = accelerator_codegen::aux_state::query;
    q::SecEdgarQ6DuckdbScan scan(connection);
    q::SecEdgarQ6Module module(q::SecEdgarQ6Config{1, 2, 3}, &scan);
    const auto state = module.BuildState();
    const double footprint = static_cast<double>(state->Bytes());
    {
      // pure state read: the 200 output rows from the top cache
      std::vector<double> us;
      for (int i = 0; i < 2000; ++i) {
        const auto t = std::chrono::steady_clock::now();
        const auto r = q::SecEdgarQ6TopRows(*state, 2023, "IS", "USD");
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count());
        if (r.size() != 200) return 1;
      }
      std::sort(us.begin(), us.end());
      std::cout << "toprows_p50_us=" << us[us.size() / 2] << "\n";
    }
    const auto &st = state->structure;
    const auto mb = [](size_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };
    std::cout << "breakdown_mb adsh=" << mb(st.adshs.Bytes()) << " name=" << mb(st.names.Bytes())
              << " tag=" << mb(st.tags.Bytes()) << " tv=" << mb(st.tvs.Bytes())
              << " plabel=" << mb(st.plabels.Bytes()) << " runs=" << mb(st.runs.MemoryBytes())
              << " groups=" << mb(st.groups.MemoryBytes() + st.group_combo.MemoryBytes()) << " group_index=" << mb(st.group_index.memory_bytes())
              << " agg=" << mb(state->sum_hi.MemoryBytes() + state->sum_lo.MemoryBytes() + state->cnt.MemoryBytes())
              << " stmts=" << st.stmts.Count() << " uoms=" << st.uoms.Count() << " combos=" << st.ComboCount()
              << " counts: adsh=" << st.adshs.Count() << " tv=" << st.tvs.Count() << " plabel="
              << st.plabels.Count() << " groups=" << st.GroupCount() << "\n";
    std::cout << "heap_before_mb=" << before << " heap_after_build_mb=" << after
              << " aux_heap_mb=" << (after - before)
              << " footprint_mb=" << footprint / (1024.0 * 1024.0)
              << " build_s=" << build_s << " rows=" << rows << "\n";
  }
  return 0;
}
