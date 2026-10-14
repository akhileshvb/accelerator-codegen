// Parameter sweep of the SEC-EDGAR Q2 aux table function against plain SQL on a
// real database: every unit of measure at fy 2022, the large units at every
// fiscal year plus edge values.
//   check_sec_edgar_q2_params <db_copy> [threads]
#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {
std::string Quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("''") : std::string(1, c);
  return out + "'";
}
std::string Sql(const std::string &fy, const std::string &uom) {
  return "SELECT s.name, n.tag, n.value FROM num n JOIN sub s ON n.adsh = s.adsh "
         "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num WHERE uom = " + uom +
         " AND value IS NOT NULL GROUP BY adsh, tag) m ON n.adsh = m.adsh AND n.tag = m.tag "
         "AND n.value = m.max_value WHERE n.uom = " + uom + " AND s.fy = " + fy +
         " AND n.value IS NOT NULL ORDER BY n.value DESC, s.name, n.tag LIMIT 100";
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  namespace catalog = accelerator_codegen::aux_state::engine;
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  con.Query("PRAGMA threads=" + std::string(argc > 2 ? argv[2] : "12"));
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 2);
  catalog::duckdb::DuckdbRuntime runtime(con, entry, 10.0);
  runtime.Build();
  std::vector<std::string> uoms;
  auto r = con.Query("SELECT DISTINCT uom FROM num WHERE uom IS NOT NULL ORDER BY 1");
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) uoms.push_back(r->GetValue(0, i).ToString());
  uoms.push_back("nonexistent");
  uoms.push_back("");
  uoms.push_back("us'd");
  const std::vector<std::string> large = {"USD", "shares", "EUR", "CNY", "pure", "CAD"};
  const std::vector<std::string> fys = {"2012", "2016", "2020", "2021", "2022", "2023", "2024", "2025",
                                        "0", "-1", "2147483647", "-2147483648", "9223372036854775807",
                                        "CAST(NULL AS INTEGER)"};
  int checked = 0, failed = 0;
  const auto check = [&](const std::string &fy, const std::string &uom) {
    const std::string call = "SELECT * FROM accel_sec_edgar_q2_aux(" + fy + ", " +
                             (uom == "\x01NULL" ? "NULL" : Quote(uom)) + ")";
    auto got = con.Query(call);
    auto want = con.Query(Sql(fy == "CAST(NULL AS INTEGER)" ? "NULL" : fy, Quote(uom)));
    ++checked;
    bool ok = got && !got->HasError() && want && !want->HasError() && got->RowCount() == want->RowCount();
    for (duckdb::idx_t i = 0; ok && i < want->RowCount(); ++i)
      for (duckdb::idx_t c = 0; ok && c < 3; ++c) {
        const auto a = got->GetValue(c, i), b = want->GetValue(c, i);
        ok = a.IsNull() == b.IsNull() && (a.IsNull() || a.ToString() == b.ToString());
      }
    if (!ok) {
      ++failed;
      std::cout << "MISMATCH fy=" << fy << " uom=" << uom << " "
                << (got && got->HasError() ? got->GetError() : "") << "\n";
    }
  };
  for (const auto &u : uoms) check("2022", u);
  for (const auto &u : large)
    for (const auto &f : fys) check(f, u);
  std::cout << "checked=" << checked << " failed=" << failed << "\n";
  return failed == 0 ? 0 : 1;
}
