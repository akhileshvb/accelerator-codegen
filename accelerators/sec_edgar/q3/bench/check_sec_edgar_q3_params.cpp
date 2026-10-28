// Parameter sweep of the SEC-EDGAR Q3 aux table function against plain SQL on a
// real database: the drawn parameter sets (eval/harness/param_sets.hpp), and each
// of them with one argument replaced by an edge value (fiscal years outside the
// domain and the INTEGER range, NULL, unknown / case-variant / quoted / empty uom),
// plus every edge pair, with the first-read (cohort build) and cached read latency.
//   check_sec_edgar_q3_params <db_copy> [threads]
// Exit status 0 when every answer matches the query template bound to the same arguments.
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
using Clock = std::chrono::steady_clock;
namespace core = accelerator_codegen::aux_state::core;
namespace harness = accelerator_codegen::aux_state::harness;

core::AuxValue Fy(int64_t v) { return core::AuxValue::Of(core::AuxType::Int64, v); }
core::AuxValue Uom(const std::string &v) { return core::AuxValue::Of(core::AuxType::String, v); }

std::vector<core::AuxValue> FyEdges() {
  return {Fy(0), Fy(-1), Fy(2147483647), Fy(-2147483648), Fy(9223372036854775807LL),
          core::AuxValue::Null(core::AuxType::Int64)};
}

std::vector<core::AuxValue> UomEdges(const core::AuxValue &drawn) {
  const auto &name = std::get<std::string>(drawn.payload);
  std::string lower = name;
  for (auto &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return {Uom("nonexistent"), Uom(lower), Uom(name + " "), Uom(name + "'"), Uom(""),
          core::AuxValue::Null(core::AuxType::String)};
}

// Each drawn set, then with one argument an edge value, then every edge pair.
std::vector<core::QueryArguments> Sweep(const std::vector<core::QueryArguments> &sets) {
  std::vector<core::QueryArguments> out = sets;
  for (const auto &set : sets) {
    for (const auto &fy : FyEdges()) out.push_back({{fy, set.values[1]}});
    for (const auto &uom : UomEdges(set.values[1])) out.push_back({{set.values[0], uom}});
  }
  for (const auto &fy : FyEdges())
    for (const auto &uom : UomEdges(sets.front().values[1])) out.push_back({{fy, uom}});
  return out;
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
  int checked = 0, failed = 0;
  double first_us = 0, cached_us = 0, plain_us = 0;
  int firsts = 0, cacheds = 0;
  const auto check = [&](const core::QueryArguments &args) {
    const std::string call = catalog::BindArguments(entry.duckdb_aux_sql, args);
    const auto t0 = Clock::now();
    auto got = con.Query(call);
    const auto t1 = Clock::now();
    auto again = con.Query(call);
    const auto t2 = Clock::now();
    auto want = con.Query(catalog::BindArguments(entry.base_sql, args));
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
      std::cout << "MISMATCH (" << harness::ArgumentLiterals(args) << ") rows aux="
                << (got && !got->HasError() ? std::to_string(got->RowCount()) : got->GetError())
                << " sql=" << (want && !want->HasError() ? std::to_string(want->RowCount()) : want->GetError())
                << "\n";
    }
  };
  for (const auto &args : Sweep(harness::ArgumentSets(entry)))
    check(args);
  std::cout << "checked=" << checked << " failed=" << failed
            << " avg_first_read_us=" << first_us / firsts
            << " avg_repeat_read_us=" << cached_us / cacheds
            << " avg_plain_sql_us=" << plain_us / checked << "\n";
  return failed == 0 ? 0 : 1;
}
