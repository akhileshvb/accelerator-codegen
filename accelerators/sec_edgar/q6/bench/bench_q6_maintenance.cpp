// Pure aux-side cost of Q6 maintenance and reads (no SQL write): applies the
// catalog's value-update probe as a MutationBatch through the coordinator and
// times ApplyMutationBatch (stage + publish) and ExecuteCollect.
// usage: bench_q6_maintenance <db_path> [iterations] [threads]
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "sec_edgar/q6/duckdb/aux/state.hpp"
namespace catalog = accelerator_codegen::aux_state::engine;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;
static double Us(Clock::time_point t) { return std::chrono::duration<double, std::micro>(Clock::now() - t).count(); }
static double P50(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
int main(int argc, char **argv) {
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  con.Query(std::string("PRAGMA threads=") + (argc > 3 ? argv[3] : "4"));
  const int iters = argc > 2 ? std::stoi(argv[2]) : 2000;
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 6);
  auto probe = catalog::FindSharedProbe(con, entry);
  DuckdbRuntime rt(con, entry, 10.0);
  rt.UseManualCapture(); // changes are hand-built below
  rt.Build();
  std::vector<double> w, r;
  auto m = catalog::BuildSharedMutation(entry, probe);
  for (int i = 0; i < iters; ++i) {
    auto t = Clock::now();
    rt.ApplyMutationBatch(m);
    w.push_back(Us(t));
    t = Clock::now();
    rt.ExecuteCollect();
    r.push_back(Us(t));
    std::swap(m.old_rows, m.new_rows); // flip back and forth
  }
  // bulk: insert then delete 200k real USD cohort facts
  {
    auto res = con.Query("SELECT adsh,tag,version,ddate,qtrs,uom,coreg,value FROM num WHERE uom='USD' AND value IS NOT NULL AND adsh IN (SELECT adsh FROM sub WHERE fy=2023) LIMIT 200000");
    namespace core = accelerator_codegen::aux_state::core;
    core::MutationBatch ins; ins.table = 1; ins.kind = core::MutationKind::Insert;
    for (duckdb::idx_t i = 0; i < res->RowCount(); ++i) {
      core::AuxRow row;
      // The module's projected num columns: adsh, tag, version, uom, value.
      for (int c : {0, 1, 2, 5, 7}) {
        auto v = res->GetValue(c, i);
        if (v.IsNull()) row.values.push_back(core::AuxValue::Null(core::AuxType::String));
        else if (c == 7) row.values.push_back(core::AuxValue::Of(core::AuxType::Double, v.GetValue<double>()));
        else row.values.push_back(core::AuxValue::Of(core::AuxType::String, v.ToString()));
      }
      ins.new_rows.push_back(std::move(row));
    }
    core::MutationBatch del = ins; del.kind = core::MutationKind::Delete; del.old_rows = ins.new_rows; del.new_rows.clear();
    for (int mode = 0; mode < 2; ++mode) {
      accelerator_codegen::aux_state::query::SecEdgarQ6ForceScalar(mode == 0);
      std::vector<double> b;
      for (int it = 0; it < 5; ++it) {
        auto t = Clock::now(); rt.ApplyMutationBatch(ins); b.push_back(Us(t));
        t = Clock::now(); rt.ApplyMutationBatch(del); b.push_back(Us(t));
      }
      std::cout << (mode == 0 ? "scalar" : "avx2") << " bulk_" << ins.new_rows.size() << "_rows_p50_us=" << P50(b) << "\n";
    }
    accelerator_codegen::aux_state::query::SecEdgarQ6ForceScalar(false);
  }
  std::cout << "maintenance_p50_us=" << P50(w) << " read_p50_us=" << P50(r) << "\n";
}
