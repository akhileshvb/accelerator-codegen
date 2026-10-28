// Live-heap (glibc mallinfo2) footprint of the SEC EDGAR Q6 aux state.
// Warms the DuckDB buffer pool with the base query first so the delta around
// Build() is dominated by aux state, then reports in-use heap before/after.
// footprint_mb is the state's own accounting (SecEdgarQ6State::Bytes) of the
// state a fresh module builds from the same scan. The reads run the drawn
// parameter sets (eval/harness/param_sets.hpp).
//
// usage: measure_q6_aux_heap <db_path> [threads]
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "duckdb.hpp"
#include "eval/harness/param_sets.hpp"
#include "sec_edgar/q6/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q6/duckdb/aux/module.hpp"
#include "eval/tests/test_support/module_runtime.hpp"

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
  const auto sets = accelerator_codegen::aux_state::harness::ArgumentSets(entry);
  auto warm = connection.Query(catalog::BindArguments(entry.base_sql, sets.front())); // populate the buffer pool
  warm.reset();
  const double before = HeapMb();
  {
    DuckdbRuntime runtime(connection, entry, 10.0);
    const auto t0 = std::chrono::steady_clock::now();
    runtime.Build();
    const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double after = HeapMb();
    const auto rows = runtime.ExecuteCollect(sets.front()).size();
    namespace q = accelerator_codegen::aux_state::query;
    q::SecEdgarQ6DuckdbScan scan(connection);
    const auto module = std::make_shared<q::SecEdgarQ6Module>(q::SecEdgarQ6Config{1, 2, 3}, &scan);
    accelerator_codegen::aux_state::test::ModuleRuntime module_runtime(connection, module);
    const auto state = module->BuildState(module_runtime.Backend());
    const double footprint = static_cast<double>(state->Bytes());
    {
      // pure state read: the output rows from the top cache, set by set
      std::vector<double> us;
      for (int i = 0; i < 2000; ++i) {
        const auto &args = sets[static_cast<std::size_t>(i) % sets.size()];
        const auto t = std::chrono::steady_clock::now();
        const auto r = q::SecEdgarQ6TopRows(*state, std::get<int64_t>(args.values[0].payload),
                                            std::get<std::string>(args.values[1].payload),
                                            std::get<std::string>(args.values[2].payload));
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count());
      }
      std::sort(us.begin(), us.end());
      std::cout << "toprows_p50_us=" << us[us.size() / 2] << "\n";
    }
    const auto &st = state->structure;
    const auto mb = [](size_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };
    std::cout << "breakdown_mb adsh=" << mb(st.adshs.Bytes()) << " name=" << mb(st.names.Bytes())
              << " tag=" << mb(st.tags.Bytes()) << " version=" << mb(st.versions.Bytes()) << " tv_index=" << mb(st.tv_index->memory_bytes())
              << " plabel=" << mb(st.plabels.Bytes()) << " runs=" << mb(st.runs.MemoryBytes())
              << " groups=" << mb(st.groups.MemoryBytes() + st.group_combo.MemoryBytes()) << " group_index=" << mb(st.group_index->memory_bytes())
              << " agg=" << mb(state->aggs.Bytes())
              << " stmts=" << st.stmts.Count() << " uoms=" << st.uoms.Count() << " combos=" << st.ComboCount()
              << " counts: adsh=" << st.adshs.Count() << " tv=" << st.TvCount() << " plabel="
              << st.plabels.Count() << " groups=" << st.GroupCount() << "\n";
    std::cout << "heap_before_mb=" << before << " heap_after_build_mb=" << after
              << " aux_heap_mb=" << (after - before)
              << " footprint_mb=" << footprint / (1024.0 * 1024.0)
              << " build_s=" << build_s << " rows=" << rows << "\n";
  }
  return 0;
}
