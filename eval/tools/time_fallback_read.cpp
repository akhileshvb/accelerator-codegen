// What a read costs when the state cannot serve it: the state build (what the
// modules' own fallbacks used to run) against the SQL fallback and a state hit.
//   time_fallback_read <tpch.duckdb> <query id>...
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point from) {
  return std::chrono::duration<double>(Clock::now() - from).count();
}

duckdb::idx_t Run(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  if (!result || result->HasError()) {
    std::cerr << sql << ": " << (result ? result->GetError() : "null") << "\n";
    std::exit(1);
  }
  return result->RowCount();
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: time_fallback_read <tpch.duckdb> <query id>...\n";
    return 2;
  }
  namespace catalog = accelerator_codegen::aux_state::engine;
  duckdb::DuckDB database(argv[1]);
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  for (int i = 2; i < argc; ++i) {
    const auto &entry = catalog::LookupSharedQuery("tpch", std::atoi(argv[i]));
    catalog::duckdb::DuckdbRuntime runtime(connection, entry, 10.0);
    const auto build_start = Clock::now();
    runtime.Build();
    const double build = Seconds(build_start);
    const std::string call = entry.duckdb_aux_sql;
    const auto hit_start = Clock::now();
    const auto rows = Run(connection, call);
    const double hit = Seconds(hit_start);
    setenv("ACCEL_AUX_FORCE_FALLBACK", "1", 1);
    const auto sql_start = Clock::now();
    const auto sql_rows = Run(connection, call);
    const double sql = Seconds(sql_start);
    unsetenv("ACCEL_AUX_FORCE_FALLBACK");
    std::cout << "Q" << argv[i] << " build_s=" << build << " state_read_s=" << hit << " sql_fallback_s=" << sql
              << " rows=" << rows << "/" << sql_rows << "\n";
  }
  return 0;
}
