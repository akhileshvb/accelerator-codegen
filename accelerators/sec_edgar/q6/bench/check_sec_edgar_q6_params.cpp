// Parameter sweep of the SEC-EDGAR Q6 aux table function against plain SQL on a
// real database: every combination of the benchmark domain
//   fy in {2021..2024}, stmt in {BS, IS, CF, EQ, CI}, uom in {USD, shares, pure, CAD, CNY, EUR}
// plus edge values (fy 2019 / 2025 / 0 / NULL, unknown stmt / uom, NULL stmt /
// uom, look-alike case). Also reports build time and the aux read latency.
//   check_sec_edgar_q6_params <db_copy> [threads] [domain_sample_percent=100]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
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
std::string Lit(const std::string &v) { return v == "\x01NULL" ? "NULL" : Quote(v); }
std::string Sql(const std::string &fy, const std::string &stmt, const std::string &uom) {
  return "SELECT s.name, p.stmt, n.tag, p.plabel, SUM(n.value) AS total_value, COUNT(*) AS cnt "
         "FROM num n JOIN sub s ON n.adsh = s.adsh "
         "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
         "WHERE n.uom = " + Lit(uom) + " AND p.stmt = " + Lit(stmt) + " AND s.fy = " + fy +
         " AND n.value IS NOT NULL GROUP BY s.name, p.stmt, n.tag, p.plabel "
         "ORDER BY total_value DESC, s.name, n.tag, p.plabel LIMIT 200";
}
double Ms(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
// Plain SUM(DOUBLE) depends on the summation order (the aux sums are
// order-independent double-doubles), so groups whose sums agree to 1e-9
// relative may be ordered differently. Outside such tie clusters rows must
// agree; inside one the multiset of (name, stmt, tag, plabel, cnt) must agree
// (the cluster cut by LIMIT is only checked for its sums); sums to 1e-9.
bool Close(double x, double y) {
  return x == y || std::fabs(x - y) <= 1e-9 * std::max(std::fabs(x), std::fabs(y));
}
bool TolerantEqual(duckdb::MaterializedQueryResult &got, duckdb::MaterializedQueryResult &want) {
  const duckdb::idx_t n = want.RowCount();
  auto key = [](duckdb::MaterializedQueryResult &r, duckdb::idx_t i) {
    std::string k;
    for (duckdb::idx_t c : {0, 1, 2, 3, 5}) {
      const auto v = r.GetValue(c, i);
      k += v.IsNull() ? std::string("\x01NULL") : v.ToString();
      k += '\x02';
    }
    return k;
  };
  for (duckdb::idx_t i = 0; i < n;) {
    const double base = want.GetValue(4, i).GetValue<double>();
    duckdb::idx_t j = i + 1;
    while (j < n && Close(want.GetValue(4, j).GetValue<double>(), base)) ++j;
    std::vector<std::string> ka, kb;
    for (duckdb::idx_t k = i; k < j; ++k) {
      if (!Close(got.GetValue(4, k).GetValue<double>(), want.GetValue(4, k).GetValue<double>())) return false;
      ka.push_back(key(got, k));
      kb.push_back(key(want, k));
    }
    if (j < n || n < 200) {
      std::sort(ka.begin(), ka.end());
      std::sort(kb.begin(), kb.end());
      if (ka != kb) return false;
    }
    i = j;
  }
  return true;
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  namespace catalog = accelerator_codegen::aux_state::engine;
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  con.Query("PRAGMA threads=" + std::string(argc > 2 ? argv[2] : "12"));
  const int sample = argc > 3 ? std::atoi(argv[3]) : 100;
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 6);
  catalog::duckdb::DuckdbRuntime runtime(con, entry, 10.0);
  auto t = std::chrono::steady_clock::now();
  runtime.Build();
  std::cout << "build_s=" << Ms(t) / 1e3 << "\n";

  const std::vector<std::string> fys = {"2021", "2022", "2023", "2024"};
  const std::vector<std::string> stmts = {"BS", "IS", "CF", "EQ", "CI"};
  const std::vector<std::string> uoms = {"USD", "shares", "pure", "CAD", "CNY", "EUR"};
  std::vector<std::vector<std::string>> cases;
  for (const auto &f : fys)
    for (const auto &s : stmts)
      for (const auto &u : uoms) cases.push_back({f, s, u});
  std::mt19937_64 rng(7);
  if (sample < 100) {
    std::shuffle(cases.begin(), cases.end(), rng);
    cases.resize(std::max<size_t>(1, cases.size() * sample / 100));
  }
  for (const auto &f : {"2019", "2025", "0", "-1", "2147483647", "9223372036854775807", "NULL"})
    cases.push_back({f, "IS", "USD"});
  cases.push_back({"2023", "ZZ", "USD"});
  cases.push_back({"2023", "is", "USD"});
  cases.push_back({"2023", "\x01NULL", "USD"});
  cases.push_back({"2023", "IS", "zz"});
  cases.push_back({"2023", "IS", "usd"});
  cases.push_back({"2023", "IS", "\x01NULL"});
  cases.push_back({"2022", "BS", "us'd"});

  int checked = 0, failed = 0, nonempty = 0;
  std::vector<double> aux_ms, base_ms;
  for (const auto &c : cases) {
    const std::string call = "SELECT * FROM accel_sec_edgar_q6_aux(" +
                             (c[0] == "NULL" ? std::string("CAST(NULL AS BIGINT)") : c[0]) + ", " + Lit(c[1]) +
                             ", " + Lit(c[2]) + ")";
    t = std::chrono::steady_clock::now();
    auto got = con.Query(call);
    aux_ms.push_back(Ms(t));
    t = std::chrono::steady_clock::now();
    auto want = con.Query(Sql(c[0], c[1], c[2]));
    base_ms.push_back(Ms(t));
    ++checked;
    bool ok = got && !got->HasError() && want && !want->HasError() && got->RowCount() == want->RowCount();
    if (ok && want->RowCount() > 0) ++nonempty;
    if (ok) ok = TolerantEqual(*got, *want);
    if (!ok) {
      ++failed;
      std::cout << "MISMATCH fy=" << c[0] << " stmt=" << c[1] << " uom=" << c[2] << " got_rows="
                << (got && !got->HasError() ? std::to_string(got->RowCount()) : got ? got->GetError() : "null")
                << " want_rows=" << (want && !want->HasError() ? std::to_string(want->RowCount()) : "err") << "\n";
    }
  }
  std::sort(aux_ms.begin(), aux_ms.end());
  std::sort(base_ms.begin(), base_ms.end());
  std::cout << "checked=" << checked << " nonempty=" << nonempty << " failed=" << failed
            << " aux_p50_ms=" << aux_ms[aux_ms.size() / 2] << " aux_max_ms=" << aux_ms.back()
            << " base_p50_ms=" << base_ms[base_ms.size() / 2] << "\n";
  return failed == 0 ? 0 : 1;
}
