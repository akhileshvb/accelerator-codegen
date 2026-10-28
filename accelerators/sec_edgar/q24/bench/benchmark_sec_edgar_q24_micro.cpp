// Micro-benchmark for the SEC EDGAR Q24 aux accelerator: build, module-level
// read, SQL table-function read, and the write/maintenance path split into
// "SQL only", "aux maintenance only" and the combined DuckdbRuntime::Apply.
//
// usage: benchmark_sec_edgar_q24_micro <db_path> [reads] [writes]
// Reads cycle through the parameter sets drawn from the parameter source
// (ACCEL_PARAM_SEED / ACCEL_PARAM_SETS / ACCEL_PARAM_FILE); writes use the first set
// that has an invalidating probe.
// env:   ACCEL_BENCH_THREADS (default 1), ACCEL_AUX_PARALLEL=1 selects the
//        parallel module when built with ACCEL_ENABLE_PARALLEL_AUX.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "catalog/query_catalog.hpp"
#include "eval/harness/param_sets.hpp"
#include "duckdb.hpp"

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;

namespace {

double Us(const Clock::time_point &start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

int64_t RssKb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream in(line.substr(6));
      int64_t kb = 0;
      in >> kb;
      return kb;
    }
  }
  return 0;
}

struct Stats {
  double p50 = 0, p99 = 0, mean = 0;
};

Stats Summarize(std::vector<double> v) {
  Stats s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.p50 = v[v.size() / 2];
  s.p99 = v[std::min(v.size() - 1, static_cast<std::size_t>(v.size() * 0.99))];
  for (double x : v) s.mean += x;
  s.mean /= static_cast<double>(v.size());
  return s;
}

void Report(const char *name, const Stats &s) {
  std::cout << std::left << std::setw(34) << name << std::right << std::fixed
            << std::setprecision(2) << " p50=" << std::setw(11) << s.p50
            << " us  mean=" << std::setw(11) << s.mean << " us  p99="
            << std::setw(11) << s.p99 << " us\n";
}

void Exec(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    std::cerr << "SQL failed: " << sql << " -> " << (r ? r->GetError() : "null")
              << "\n";
    std::exit(1);
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: benchmark_sec_edgar_q24_micro <db> [reads] [writes]\n";
    return 2;
  }
  const int reads = argc > 2 ? std::atoi(argv[2]) : 2000;
  const int writes = argc > 3 ? std::atoi(argv[3]) : 200;
  const char *threads_env = std::getenv("ACCEL_BENCH_THREADS");
  const int threads = threads_env ? std::max(1, std::atoi(threads_env)) : 1;
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  Exec(con, "PRAGMA threads=" + std::to_string(threads));
  setenv("ACCEL_AUX_THREADS", std::to_string(threads).c_str(), 1);
  const auto &base_entry = catalog::LookupSharedQuery("sec_edgar", 24);
  catalog::SharedQueryCatalogEntry entry = base_entry;
  const bool use_parallel = std::getenv("ACCEL_AUX_PARALLEL") != nullptr;
  if (use_parallel) {
    entry = catalog::LookupSharedQueryParallel("sec_edgar", 24);
  }

  const auto rss0 = RssKb();
  DuckdbRuntime runtime(con, entry, 10.0);
  runtime.UseManualCapture(); // changes are hand-built below
  auto t0 = Clock::now();
  runtime.Build();
  const double build_us = Us(t0);
  const auto rss1 = RssKb();
  std::cout << "build_us=" << std::fixed << std::setprecision(0) << build_us
            << " rss_delta_mb=" << (rss1 - rss0) / 1024.0
            << " footprint_mb=" << runtime.AuxStateFootprintBytes() / 1048576.0
            << " threads=" << threads << (use_parallel ? " parallel" : "") << "\n";

  if (reads <= 0 && writes <= 0) return 0; // build-only (for profiling)
  // Reads.
  const auto sets = harness::ArgumentSets(entry);
  std::vector<std::string> aux_sqls;
  for (const auto &args : sets) aux_sqls.push_back(catalog::BindArguments(entry.duckdb_aux_sql, args));
  std::vector<double> mod, tf;
  for (int i = 0; i < 20; ++i) (void)runtime.ExecuteCollect(sets[i % sets.size()]);
  for (int i = 0; i < reads; ++i) {
    t0 = Clock::now();
    auto rows = runtime.ExecuteCollect(sets[i % sets.size()]);
    mod.push_back(Us(t0));
  }
  for (int i = 0; i < 20; ++i) Exec(con, aux_sqls[i % sets.size()]);
  for (int i = 0; i < reads; ++i) {
    t0 = Clock::now();
    auto r = con.Query(aux_sqls[i % sets.size()]);
    if (!r || r->HasError()) return 1;
    for (duckdb::idx_t row = 0; row < r->RowCount(); ++row)
      for (duckdb::idx_t c = 0; c < r->ColumnCount(); ++c) (void)r->GetValue(c, row);
    tf.push_back(Us(t0));
  }
  Report("read: module Execute", Summarize(mod));
  Report("read: SQL table function", Summarize(tf));

  if (writes > 0) {
    accelerator_codegen::aux_state::core::QueryArguments write_args;
    catalog::SharedProbe probe;
    bool found = false;
    for (const auto &args : sets) {
      try {
        probe = catalog::FindInvalidatingSharedProbe(con, entry, args);
        write_args = args;
        found = true;
        break;
      } catch (const std::exception &) {
      }
    }
    if (!found) {
      std::cerr << "no drawn parameter set has an invalidating probe\n";
      return 1;
    }
    std::vector<double> sql_only, aux_only, both, both_then_read;
    for (int i = 0; i < writes; ++i) {
      t0 = Clock::now();
      con.BeginTransaction();
      Exec(con, probe.update_sql);
      con.Commit();
      sql_only.push_back(Us(t0));
      // aux maintenance only (state is advanced as if the SQL had happened).
      t0 = Clock::now();
      runtime.ApplyMutationBatch(catalog::BuildSharedMutation(entry, probe));
      aux_only.push_back(Us(t0));
      probe.current_value += probe.write_delta;
      // The probe SQL is relative (value + delta); keep batch rows in sync.
      // (After the loop the data is restored below.)
      auto next = probe;
      (void)next;
    }
    Exec(con, probe.restore_sql);
    // Aux maintenance in isolation (no SQL): many point-update batches.
    {
      std::vector<double> iso;
      const auto mutation = catalog::BuildSharedMutation(entry, probe);
      const int iso_n = std::getenv("Q24_ISO_WRITES") ? std::atoi(std::getenv("Q24_ISO_WRITES")) : 5000;
      for (int i = 0; i < iso_n; ++i) {
        t0 = Clock::now();
        runtime.ApplyMutationBatch(mutation);
        iso.push_back(Us(t0));
      }
      Report("write: aux maintenance isolated", Summarize(iso));
    }
    // Re-sync aux with the restored table, then time the combined path.
    runtime.Build();
    auto probe2 = catalog::FindInvalidatingSharedProbe(con, entry, write_args);
    for (int i = 0; i < writes; ++i) {
      t0 = Clock::now();
      runtime.Apply(probe2);
      both.push_back(Us(t0));
    }
    Exec(con, probe2.restore_sql);
    runtime.Build();
    Report("write: SQL UPDATE only", Summarize(sql_only));
    Report("write: aux maintenance only", Summarize(aux_only));
    Report("write: Apply (SQL + aux)", Summarize(both));
    // Read after a write: first read pays lazy top-k recompute.
    auto probe3 = catalog::FindInvalidatingSharedProbe(con, entry, write_args);
    std::vector<double> rw;
    for (int i = 0; i < std::min(writes, 200); ++i) {
      runtime.Apply(probe3);
      t0 = Clock::now();
      (void)runtime.ExecuteCollect(write_args);
      rw.push_back(Us(t0));
    }
    Exec(con, probe3.restore_sql);
    Report("read right after write (module)", Summarize(rw));
  }

  // Pre DELETE / re-INSERT of one existing row (incremental: the triple is
  // resolved by a targeted table lookup at Stage). Times aux maintenance only.
  if (writes > 0) {
    const int pre_n = std::getenv("Q24_PRE_WRITES") ? std::atoi(std::getenv("Q24_PRE_WRITES")) : 20;
    std::vector<double> del, ins;
    for (int i = 0; i < pre_n; ++i) {
      auto r = con.Query("SELECT rowid, adsh, tag, version FROM pre OFFSET " +
                         std::to_string(1000 + i * 99991) + " LIMIT 1");
      if (!r || r->HasError() || r->RowCount() == 0) break;
      const auto rowid = r->GetValue(0, 0).ToString();
      const auto a = r->GetValue(1, 0).ToString();
      const auto t = r->GetValue(2, 0).ToString();
      const auto v = r->GetValue(3, 0).ToString();
      auto q = [](const std::string &x) {
        std::string o = "'";
        for (char c : x) o += c == '\'' ? std::string("''") : std::string(1, c);
        return o + "'";
      };
      accelerator_codegen::aux_state::core::AuxRow aux{
          {accelerator_codegen::aux_state::core::AuxValue::Of(accelerator_codegen::aux_state::core::AuxType::String, a),
           accelerator_codegen::aux_state::core::AuxValue::Of(accelerator_codegen::aux_state::core::AuxType::String, t),
           accelerator_codegen::aux_state::core::AuxValue::Of(accelerator_codegen::aux_state::core::AuxType::String, v)}};
      Exec(con, "DELETE FROM pre WHERE rowid = " + rowid);
      accelerator_codegen::aux_state::core::MutationBatch m;
      m.table = 102;
      m.kind = accelerator_codegen::aux_state::core::MutationKind::Delete;
      m.old_rows = {aux};
      t0 = Clock::now();
      runtime.ApplyMutationBatch(m);
      del.push_back(Us(t0));
      Exec(con, "INSERT INTO pre (adsh, tag, version) VALUES (" + q(a) + "," + q(t) + "," + q(v) + ")");
      m.kind = accelerator_codegen::aux_state::core::MutationKind::Insert;
      m.old_rows.clear();
      m.new_rows = {aux};
      t0 = Clock::now();
      runtime.ApplyMutationBatch(m);
      ins.push_back(Us(t0));
    }
    Report("write: pre DELETE aux maint.", Summarize(del));
    Report("write: pre INSERT aux maint.", Summarize(ins));
  }
  return 0;
}
