// GENERATED BACKEND
//
// PostgreSQL runtime backend: query registry, generic dispatch and SQL
// function registration.  Names no query; see runtime.hpp for the layout of
// the per-query code that registers itself here.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/api/runtime.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

extern "C" {
#include "executor/spi.h"
#include "utils/builtins.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Function-local storage: safe against static-initialisation order, and a
// deque keeps entries at stable addresses.
std::deque<PostgresQueryExecutor> &Executors() {
  static std::deque<PostgresQueryExecutor> executors;
  return executors;
}

std::deque<PostgresQuerySql> &SqlEntries() {
  static std::deque<PostgresQuerySql> entries;
  return entries;
}

} // namespace

void RegisterPostgresQueryExecutor(PostgresQueryExecutor entry) {
  Executors().push_back(entry);
}

void RegisterPostgresQuerySql(PostgresQuerySql entry) {
  SqlEntries().push_back(entry);
}

void ExecutePostgresQuery(const char *workload, int query_id,
                          const char *variant, FunctionCallInfo fcinfo) {
  for (const auto &entry : Executors()) {
    if (entry.query_id == query_id && std::strcmp(entry.workload, workload) == 0 &&
        std::strcmp(entry.variant, variant) == 0) {
      entry.execute(fcinfo);
      return;
    }
  }
  ereport(ERROR, (errmsg("no %s executor registered for %s Q%d", variant,
                         workload, query_id)));
}

} // namespace accelerator_codegen::aux_state::engine::postgres

extern "C" {

// accel_postgres_register_queries() -> number of registered query SQL entries.
// Runs every registered query's CREATE FUNCTION text (ordered by workload and
// query id), so the SQL-visible functions are exactly those the loaded module
// carries.
PG_FUNCTION_INFO_V1(accel_postgres_register_queries);
Datum accel_postgres_register_queries(PG_FUNCTION_ARGS) {
  using namespace accelerator_codegen::aux_state::engine::postgres;
  std::vector<PostgresQuerySql> entries(SqlEntries().begin(), SqlEntries().end());
  std::sort(entries.begin(), entries.end(),
            [](const PostgresQuerySql &a, const PostgresQuerySql &b) {
              const int c = std::strcmp(a.workload, b.workload);
              return c != 0 ? c < 0 : a.query_id < b.query_id;
            });
  if (SPI_connect() != SPI_OK_CONNECT) {
    ereport(ERROR, (errmsg("SPI_connect failed")));
  }
  for (const auto &entry : entries) {
    if (SPI_execute(entry.sql, false, 0) < 0) {
      ereport(ERROR, (errmsg("registering %s Q%d failed", entry.workload,
                             entry.query_id)));
    }
  }
  SPI_finish();
  PG_RETURN_INT32(static_cast<int32>(entries.size()));
}

} // extern "C"

#endif
