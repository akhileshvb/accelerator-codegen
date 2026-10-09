// Micro-benchmark for the SEC EDGAR Q1 aux accelerator: build, read (module and
// SQL table function) and per-mutation maintenance latency, without the cost of
// the DuckDB write itself (mutations are applied to the aux layer only).
//
// usage: benchmark_sec_edgar_q1_micro <db_path> [single|parallel] [threads=1]
// The database is opened read-only. Run from the repository root.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "catalog/query_catalog.hpp"
#include "sec_edgar/q1/common/module.hpp"
#include "sec_edgar/q1/duckdb/aux/native_scan.hpp"
#include "duckdb.hpp"

namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

namespace {

double Us(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

core::AuxRow Row(const char *stmt, const char *rfile, const std::string &adsh, int64_t line) {
  core::AuxRow row;
  row.values.push_back(core::AuxValue::Of(core::AuxType::String, std::string(stmt)));
  row.values.push_back(core::AuxValue::Of(core::AuxType::String, std::string(rfile)));
  row.values.push_back(core::AuxValue::Of(core::AuxType::String, adsh));
  row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, line));
  return row;
}

void Report(const std::string &name, std::vector<double> samples) {
  std::cout << std::left << std::setw(46) << name << std::right << std::fixed << std::setprecision(2)
            << "p50_us=" << std::setw(12) << Median(samples) << "  n=" << samples.size() << "\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: benchmark_sec_edgar_q1_micro <db_path> [single|parallel] [threads]\n";
    return 2;
  }
  const bool parallel = argc > 2 && std::string(argv[2]) == "parallel";
  const int threads = argc > 3 ? std::atoi(argv[3]) : 1;
  duckdb::DBConfig config;
  config.options.access_mode = duckdb::AccessMode::READ_ONLY;
  duckdb::DuckDB db(argv[1], &config);
  duckdb::Connection c(db);
  accelerator_codegen::RegisterAcceleratorFunctions(c);
  c.Query("PRAGMA threads=" + std::to_string(threads));
  setenv("ACCEL_AUX_THREADS", std::to_string(threads).c_str(), 1);
#ifdef ACCEL_ENABLE_PARALLEL_AUX
  const auto &entry = parallel ? catalog::LookupSharedQueryParallel("sec_edgar", 1)
                               : catalog::LookupSharedQuery("sec_edgar", 1);
#else
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 1);
  (void)parallel;
#endif
  std::cout << std::unitbuf << "module=" << entry.accelerator_id << " threads=" << threads << "\n";

  DuckdbRuntime runtime(c, entry, 10.0);
  {
    std::vector<double> s;
    for (int i = 0; i < 3; ++i) {
      DuckdbRuntime r(c, entry, 10.0);
      const auto t = Clock::now();
      r.Build();
      s.push_back(Us(t));
    }
    Report("Build()", s);
  }
  runtime.Build();
  std::cout << "footprint_bytes=" << runtime.AuxStateFootprintBytes() << "\n";
  {
    // Size of the state as a serialized image, and its heap size.
    namespace q1 = accelerator_codegen::aux_state::query::q1;
    q1::Builder builder;
    accelerator_codegen::aux_state::query::ScanSecEdgarQ1PreNative(
        c, [&](duckdb::DataChunk &chunk) { builder.ConsumeChunk(chunk); });
    const auto state = builder.Finish();
    std::cout << "encoded_state_bytes=" << q1::Encode(state).size() << " heap_bytes=" << state.MemoryBytes() << "\n";
  }

  {
    std::vector<double> s;
    for (int i = 0; i < 2000; ++i) {
      const auto t = Clock::now();
      auto rows = runtime.ExecuteCollect();
      s.push_back(Us(t));
    }
    Report("ExecuteCollect() [module read]", s);
  }
  {
    std::vector<double> s;
    for (int i = 0; i < 300; ++i) {
      const auto t = Clock::now();
      auto r = c.Query(entry.duckdb_aux_sql);
      if (r->HasError()) {
        std::cerr << r->GetError() << "\n";
        return 1;
      }
      s.push_back(Us(t));
    }
    Report("SELECT * FROM accel_*_aux() [SQL read]", s);
  }

  if (const char *loops = std::getenv("Q1_MICRO_SQL_LOOP")) { // for perf: hammer the SQL read path
    for (int i = 0, n = std::atoi(loops); i < n; ++i) {
      auto r = c.Query(entry.duckdb_aux_sql);
    }
    return 0;
  }
  if (const char *loops = std::getenv("Q1_MICRO_BULK_LOOP")) { // for perf: bulk maintenance
    std::vector<core::AuxRow> rows;
    for (int i = 0; i < 10000; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%010d-22-%06d", 1000000 + (i % 4000), i % 7);
      rows.push_back(Row(i % 3 ? "BS" : "IS", "H", buf, i % 300));
    }
    for (int i = 0, n = std::atoi(loops); i < n; ++i) {
      core::MutationBatch m;
      m.table = 1;
      m.kind = core::MutationKind::Insert;
      m.new_rows = rows;
      runtime.ApplyMutationBatch(m);
    }
    return 0;
  }
  auto apply = [&](const std::string &name, core::MutationKind kind, auto make, int rows_per_batch, int reps) {
    std::vector<double> s;
    for (int i = 0; i < reps; ++i) {
      core::MutationBatch m;
      m.table = 1;
      m.kind = kind;
      m.updated_columns = {3};
      for (int k = 0; k < rows_per_batch; ++k) {
        make(m, i * rows_per_batch + k);
      }
      const auto t = Clock::now();
      runtime.ApplyMutationBatch(m);
      s.push_back(Us(t));
    }
    Report(name, s);
  };
  // A real accession present in BS/H so refcounts stay consistent with the data.
  auto seed = c.Query("SELECT adsh FROM pre WHERE stmt = 'BS' AND rfile = 'H' LIMIT 1");
  const std::string existing = seed->GetValue(0, 0).ToString();
  apply("update line only (1 row)", core::MutationKind::Update,
        [&](core::MutationBatch &m, int) {
          m.old_rows.push_back(Row("BS", "H", existing, 10));
          m.new_rows.push_back(Row("BS", "H", existing, 1010));
        },
        1, 500);
  apply("insert 1 row existing adsh", core::MutationKind::Insert,
        [&](core::MutationBatch &m, int) { m.new_rows.push_back(Row("BS", "H", existing, 5)); }, 1, 500);
  apply("delete 1 row (same adsh)", core::MutationKind::Delete,
        [&](core::MutationBatch &m, int) { m.old_rows.push_back(Row("BS", "H", existing, 5)); }, 1, 500);
  apply("insert 1 row NEW adsh", core::MutationKind::Insert,
        [&](core::MutationBatch &m, int i) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%010d-23-%06d", 5000000 + i, 1);
          m.new_rows.push_back(Row("IS", "H", buf, 3));
        },
        1, 500);
  apply("update group move BS->CF (1 row)", core::MutationKind::Update,
        [&](core::MutationBatch &m, int i) { // alternate so refcounts never underflow
          const char *from = i % 2 ? "CF" : "BS";
          const char *to = i % 2 ? "BS" : "CF";
          m.old_rows.push_back(Row(from, "H", existing, 10));
          m.new_rows.push_back(Row(to, "H", existing, 10));
        },
        1, 500);
  apply("insert batch 100 rows (existing adsh)", core::MutationKind::Insert,
        [&](core::MutationBatch &m, int i) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%010d-22-%06d", 1000000 + (i % 400), i % 7);
          m.new_rows.push_back(Row("BS", "H", buf, i % 300));
        },
        100, 100);
  apply("insert batch 10000 rows", core::MutationKind::Insert,
        [&](core::MutationBatch &m, int i) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%010d-22-%06d", 1000000 + (i % 4000), i % 7);
          m.new_rows.push_back(Row("BS", "H", buf, i % 300));
        },
        10000, 10);
  {
    std::vector<double> s;
    for (int i = 0; i < 300; ++i) {
      auto rows = runtime.ExecuteCollect();
      const auto t = Clock::now();
      rows = runtime.ExecuteCollect();
      s.push_back(Us(t));
    }
    Report("ExecuteCollect() after writes", s);
  }
  return 0;
}
