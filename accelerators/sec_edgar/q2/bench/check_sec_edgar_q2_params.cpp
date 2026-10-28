// Parameter sweep of the SEC-EDGAR Q2 aux table function against the query template on a
// real database: the drawn parameter sets (eval/harness/param_sets), every unit of measure
// at the first drawn fiscal year, the largest units at every fiscal year plus edge values.
//   check_sec_edgar_q2_params <db_copy> [threads]
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "eval/harness/param_sets.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace harness = accelerator_codegen::aux_state::harness;

namespace {

core::QueryArguments Arguments(const std::optional<int64_t> &fy, const std::optional<std::string> &uom) {
  core::QueryArguments args;
  args.values.push_back(fy ? core::AuxValue::Of(core::AuxType::Int64, *fy) : core::AuxValue::Null(core::AuxType::Int64));
  args.values.push_back(uom ? core::AuxValue::Of(core::AuxType::String, *uom)
                            : core::AuxValue::Null(core::AuxType::String));
  return args;
}

std::vector<std::string> Column(duckdb::Connection &con, const std::string &sql) {
  std::vector<std::string> out;
  auto r = con.Query(sql);
  for (duckdb::idx_t i = 0; r && !r->HasError() && i < r->RowCount(); ++i) out.push_back(r->GetValue(0, i).ToString());
  return out;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  con.Query("PRAGMA threads=" + std::string(argc > 2 ? argv[2] : "12"));
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 2);
  catalog::duckdb::DuckdbRuntime runtime(con, entry, 10.0);
  runtime.Build();
  const auto sets = harness::ArgumentSets(entry);
  const int64_t drawn_fy = std::get<int64_t>(sets.front().values[0].payload);
  // Every unit in the data and a few that are not, the largest units, the fiscal years in the data and edge values.
  std::vector<std::string> uoms = Column(con, "SELECT DISTINCT uom FROM num WHERE uom IS NOT NULL ORDER BY 1");
  const auto large = Column(con, "SELECT uom FROM num WHERE uom IS NOT NULL GROUP BY uom ORDER BY count(*) DESC LIMIT 6");
  uoms.push_back("nonexistent");
  uoms.push_back("");
  uoms.push_back("us'd");
  std::vector<std::optional<int64_t>> fys = {0, -1, 2147483647, -2147483647 - 1, INT64_MAX, std::nullopt};
  for (const auto &fy : Column(con, "SELECT DISTINCT fy FROM sub WHERE fy IS NOT NULL ORDER BY 1")) fys.push_back(std::stoll(fy));
  int checked = 0, failed = 0;
  const auto check = [&](const core::QueryArguments &args) {
    auto got = con.Query(catalog::BindArguments(entry.duckdb_aux_sql, args));
    auto want = con.Query(catalog::BindArguments(entry.base_sql, args));
    ++checked;
    bool ok = got && !got->HasError() && want && !want->HasError() && got->RowCount() == want->RowCount();
    for (duckdb::idx_t i = 0; ok && i < want->RowCount(); ++i)
      for (duckdb::idx_t c = 0; ok && c < 3; ++c) {
        const auto a = got->GetValue(c, i), b = want->GetValue(c, i);
        ok = a.IsNull() == b.IsNull() && (a.IsNull() || a.ToString() == b.ToString());
      }
    if (!ok) {
      ++failed;
      std::cout << "MISMATCH (" << harness::ArgumentLiterals(args) << ") "
                << (got && got->HasError() ? got->GetError() : "") << "\n";
    }
  };
  for (const auto &args : sets) check(args);
  for (const auto &u : uoms) check(Arguments(drawn_fy, u));
  for (const auto &u : large)
    for (const auto &f : fys) check(Arguments(f, u));
  check(Arguments(drawn_fy, std::nullopt));
  std::cout << "checked=" << checked << " failed=" << failed << "\n";
  return failed == 0 ? 0 : 1;
}
