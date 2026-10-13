// Parameter sweep of the SEC-EDGAR Q3 aux table function against plain SQL on a
// real database: every combination of fy in {2021..2024, 2019, 2025, 0, NULL,
// extremes} and uom in {USD, shares, pure, CAD, CNY, EUR, unknown, case / empty
// variants, NULL}, with the first-read (cohort build) and cached read latency.
//   check_sec_edgar_q3_params <db_copy> [threads]
// Exit status 0 when every answer matches q3.sql with :fy / :uom substituted.
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {
using Clock = std::chrono::steady_clock;

std::string Quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("''") : std::string(1, c);
  return out + "'";
}

std::string Replace(std::string sql, const std::string &from, const std::string &to) {
  const auto at = sql.find(from);
  if (at == std::string::npos) {
    std::cerr << "base_sql shape changed: " << from << "\n";
    std::exit(2);
  }
  sql.replace(at, from.size(), to);
  return sql;
}

std::string Sql(const std::string &base, const std::string &fy, const std::string &uom) {
  std::string sql = base;
  sql = Replace(sql, "s.fy = 2022", "s.fy = " + fy);
  sql = Replace(sql, "s2.fy = 2022", "s2.fy = " + fy);
  sql = Replace(sql, "n.uom = 'USD'", "n.uom = " + uom);
  sql = Replace(sql, "n2.uom = 'USD'", "n2.uom = " + uom);
  return sql;
}

double Us(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  namespace catalog = accelerator_codegen::aux_state::engine;
  duckdb::DuckDB db(argv[1]);
  duckdb::Connection con(db);
  accelerator_codegen::RegisterAcceleratorFunctions(con);
  con.Query("PRAGMA threads=" + std::string(argc > 2 ? argv[2] : "12"));
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 3);
  catalog::duckdb::DuckdbRuntime runtime(con, entry, 10.0);
  runtime.Build();
  const std::vector<std::string> uoms = {"USD", "shares", "pure", "CAD", "CNY", "EUR",
                                         "nonexistent", "usd", "", "USD ", "us'd", "\x01NULL"};
  const std::vector<std::string> fys = {"2021", "2022", "2023", "2024", "2019", "2025", "0",
                                        "-1", "2147483647", "-2147483648",
                                        "9223372036854775807", "CAST(NULL AS INTEGER)"};
  int checked = 0, failed = 0;
  double first_us = 0, cached_us = 0, plain_us = 0;
  int firsts = 0, cacheds = 0;
  const auto check = [&](const std::string &fy, const std::string &uom) {
    const bool null_uom = uom == "\x01NULL";
    const std::string call = "SELECT * FROM accel_sec_edgar_q3_aux(" + fy + ", " +
                             (null_uom ? "NULL" : Quote(uom)) + ")";
    const auto t0 = Clock::now();
    auto got = con.Query(call);
    const auto t1 = Clock::now();
    auto again = con.Query(call);
    const auto t2 = Clock::now();
    auto want = con.Query(Sql(entry.base_sql, fy == "CAST(NULL AS INTEGER)" ? "NULL" : fy,
                              null_uom ? "NULL" : Quote(uom)));
    const auto t3 = Clock::now();
    first_us += Us(t0, t1); ++firsts;
    cached_us += Us(t1, t2); ++cacheds;
    plain_us += Us(t2, t3);
    ++checked;
    bool ok = got && !got->HasError() && want && !want->HasError() &&
              got->RowCount() == want->RowCount() && again && !again->HasError() &&
              again->RowCount() == want->RowCount();
    for (duckdb::idx_t i = 0; ok && i < want->RowCount(); ++i)
      for (duckdb::idx_t c = 0; ok && c < 3; ++c) {
        const auto a = got->GetValue(c, i), b = want->GetValue(c, i);
        if (a.IsNull() || b.IsNull()) {
          ok = a.IsNull() == b.IsNull();
        } else if (c == 2) {
          const double x = a.GetValue<double>(), y = b.GetValue<double>();
          ok = std::fabs(x - y) <= 1e-9 * std::max(1.0, std::fabs(y));
        } else {
          ok = a.ToString() == b.ToString();
        }
      }
    if (!ok) {
      ++failed;
      std::cout << "MISMATCH fy=" << fy << " uom=" << uom << " rows aux="
                << (got && !got->HasError() ? std::to_string(got->RowCount()) : got->GetError())
                << " sql=" << (want && !want->HasError() ? std::to_string(want->RowCount()) : want->GetError())
                << "\n";
    }
  };
  for (const auto &u : uoms)
    for (const auto &f : fys) check(f, u);
  std::cout << "checked=" << checked << " failed=" << failed
            << " avg_first_read_us=" << first_us / firsts
            << " avg_repeat_read_us=" << cached_us / cacheds
            << " avg_plain_sql_us=" << plain_us / checked << "\n";
  return failed == 0 ? 0 : 1;
}
