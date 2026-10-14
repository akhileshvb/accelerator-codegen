// Latency of sub/pre writes through the full runtime path (SQL write + capture
// + stage + publish). Each case flips a value and back and reports the p50 of
// the Apply() calls. Set ACCEL_FORCE_MAINTENANCE_ACTION=RebuildAll to measure
// the rebuild path. Differential check against the base query at the end.
// usage: bench_q6_structural <db_path> [iterations] [threads]
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include "eval/harness/differential.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "sec_edgar/q6/duckdb/aux/module.hpp"
namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
using catalog::duckdb::DuckdbRuntime;
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
static double P50(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
static std::string Lit(const duckdb::Value &v) {
  if (v.IsNull()) return "NULL";
  if (v.type().id() == duckdb::LogicalTypeId::VARCHAR) {
    std::string o = "'"; for (char c : v.ToString()) { if (c == '\'') o += "''"; else o += c; } return o + "'";
  }
  return v.ToString();
}
// The row as the module receives it: only the columns it projects
// (SecEdgarQ6Columns::k{Sub,Pre}Attnums), in that order.
static core::AuxRow ToRow(duckdb::Connection &con, const std::string &sql, const std::vector<int32_t> &attnums) {
  auto r = con.Query(sql);
  if (r->HasError() || r->RowCount() != 1) throw std::runtime_error("row fetch failed: " + sql + r->GetError());
  core::AuxRow row;
  for (const auto attnum : attnums) {
    auto v = r->GetValue(static_cast<duckdb::idx_t>(attnum), 0);
    if (v.IsNull()) row.values.push_back(core::AuxValue::Null(core::AuxType::String));
    else if (v.type().id() == duckdb::LogicalTypeId::VARCHAR) row.values.push_back(core::AuxValue::Of(core::AuxType::String, v.ToString()));
    else row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, v.GetValue<int64_t>()));
  }
  return row;
}
int main(int argc, char **argv) {
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  const int iters = argc > 2 ? std::stoi(argv[2]) : 3;
  con.Query(std::string("PRAGMA threads=") + (argc > 3 ? argv[3] : "1"));
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 6);
  DuckdbRuntime rt(con, entry, 10.0);
  rt.Build();
  // target: a cohort adsh with IS pre rows and USD facts
  auto t = con.Query("SELECT s.adsh, p.rowid FROM sub s JOIN pre p ON p.adsh = s.adsh WHERE s.fy = 2023 AND p.stmt = 'IS' AND p.plabel IS NOT NULL ORDER BY s.adsh LIMIT 1 OFFSET 7000");
  const std::string adsh = t->GetValue(0, 0).ToString();
  const std::string prow = t->GetValue(1, 0).ToString();
  const std::string sel_sub = "SELECT * FROM sub WHERE adsh = '" + adsh + "'";
  const std::string sel_pre = "SELECT * FROM pre WHERE rowid = " + prow;
  using Cols = accelerator_codegen::aux_state::query::SecEdgarQ6Columns;
  const std::vector<int32_t> sub_attnums(std::begin(Cols::kSubAttnums), std::end(Cols::kSubAttnums));
  const std::vector<int32_t> pre_attnums(std::begin(Cols::kPreAttnums), std::end(Cols::kPreAttnums));
  struct Case { std::string name; int table; std::vector<uint32_t> cols; std::vector<int32_t> attnums; std::string sel; std::string fwd, back; };
  std::vector<Case> cases = {
    {"pre_plabel_update", 3, {4}, pre_attnums, sel_pre, "UPDATE pre SET plabel = plabel || ' x' WHERE rowid = " + prow, "UPDATE pre SET plabel = substr(plabel, 1, length(plabel) - 2) WHERE rowid = " + prow},
    {"pre_is_to_bs", 3, {1}, pre_attnums, sel_pre, "UPDATE pre SET stmt = 'BS' WHERE rowid = " + prow, "UPDATE pre SET stmt = 'IS' WHERE rowid = " + prow},
    {"sub_name_update", 2, {1}, sub_attnums, sel_sub, "UPDATE sub SET name = name || ' x' WHERE adsh = '" + adsh + "'", "UPDATE sub SET name = substr(name, 1, length(name) - 2) WHERE adsh = '" + adsh + "'"},
    {"sub_cohort_exit_enter", 2, {2}, sub_attnums, sel_sub, "UPDATE sub SET fy = 2022 WHERE adsh = '" + adsh + "'", "UPDATE sub SET fy = 2023 WHERE adsh = '" + adsh + "'"},
  };
  for (auto &c : cases) {
    std::vector<double> times;
    for (int i = 0; i < iters; ++i) {
      for (int dir = 0; dir < 2; ++dir) {
        catalog::SharedProbe probe;
        probe.table = c.table;
        probe.kind = core::MutationKind::Update;
        probe.updated_columns = c.cols;
        probe.old_row = ToRow(con, c.sel, c.attnums);
        probe.update_sql = dir == 0 ? c.fwd : c.back;
        con.Query(probe.update_sql);       // compute new row, then undo to let Apply do the write
        probe.new_row = ToRow(con, c.sel, c.attnums);
        con.Query(dir == 0 ? c.back : c.fwd);
        auto start = Clock::now();
        rt.Apply(probe);
        times.push_back(Ms(start));
      }
    }
    std::cout << c.name << " apply_p50_ms=" << P50(times) << " (n=" << times.size() << ")\n";
  }
  auto start = Clock::now();
  accelerator_codegen::aux_state::harness::RequireSharedMatchesBase(con, rt, entry);
  std::cout << "differential ok; state_bytes=" << rt.AuxStateFootprintBytes() << "\n";
}
