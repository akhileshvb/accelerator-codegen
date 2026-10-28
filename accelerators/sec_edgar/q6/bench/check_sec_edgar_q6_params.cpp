// Parameter sweep of the SEC-EDGAR Q6 aux table function against plain SQL on a
// real database: the drawn parameter sets (eval/harness/param_sets.hpp), and each
// of them with one argument replaced by an edge value (fiscal years outside the
// domain and the INTEGER range, unknown / look-alike / quoted stmt and uom, NULLs).
// Also reports build time and the aux read latency. The number of sets is
// ACCEL_PARAM_SETS (every combination of the domain is a large enough number).
//   check_sec_edgar_q6_params <db_copy> [threads]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/harness/param_sets.hpp"

namespace {
namespace core = accelerator_codegen::aux_state::core;
namespace harness = accelerator_codegen::aux_state::harness;

core::AuxValue Fy(int64_t v) { return core::AuxValue::Of(core::AuxType::Int64, v); }
core::AuxValue Text(const std::string &v) { return core::AuxValue::Of(core::AuxType::String, v); }

std::vector<core::AuxValue> FyEdges() {
  return {Fy(0), Fy(-1), Fy(2147483647), Fy(-2147483648), Fy(9223372036854775807LL),
          core::AuxValue::Null(core::AuxType::Int64)};
}

std::vector<core::AuxValue> TextEdges(const core::AuxValue &drawn) {
  const auto &name = std::get<std::string>(drawn.payload);
  std::string lower = name;
  for (auto &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return {Text("nonexistent"), Text(lower), Text(name + " "), Text(name + "'"), Text(""),
          core::AuxValue::Null(core::AuxType::String)};
}

// Each drawn set, then with one argument replaced by an edge value.
std::vector<core::QueryArguments> Sweep(const std::vector<core::QueryArguments> &sets) {
  std::vector<core::QueryArguments> out = sets;
  for (const auto &set : sets) {
    for (const auto &fy : FyEdges()) out.push_back({{fy, set.values[1], set.values[2]}});
    for (const auto &stmt : TextEdges(set.values[1])) out.push_back({{set.values[0], stmt, set.values[2]}});
    for (const auto &uom : TextEdges(set.values[2])) out.push_back({{set.values[0], set.values[1], uom}});
  }
  return out;
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
  const auto &entry = catalog::LookupSharedQuery("sec_edgar", 6);
  catalog::duckdb::DuckdbRuntime runtime(con, entry, 10.0);
  auto t = std::chrono::steady_clock::now();
  runtime.Build();
  std::cout << "build_s=" << Ms(t) / 1e3 << "\n";

  const auto cases = Sweep(harness::ArgumentSets(entry));

  int checked = 0, failed = 0, nonempty = 0;
  std::vector<double> aux_ms, base_ms;
  for (const auto &args : cases) {
    const std::string call = catalog::BindArguments(entry.duckdb_aux_sql, args);
    t = std::chrono::steady_clock::now();
    auto got = con.Query(call);
    aux_ms.push_back(Ms(t));
    t = std::chrono::steady_clock::now();
    auto want = con.Query(catalog::BindArguments(entry.base_sql, args));
    base_ms.push_back(Ms(t));
    ++checked;
    bool ok = got && !got->HasError() && want && !want->HasError() && got->RowCount() == want->RowCount();
    if (ok && want->RowCount() > 0) ++nonempty;
    if (ok) ok = TolerantEqual(*got, *want);
    if (!ok) {
      ++failed;
      std::cout << "MISMATCH (" << harness::ArgumentLiterals(args) << ") got_rows="
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
