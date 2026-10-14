// GENERATED BACKEND
//
// PostgreSQL runtime backend: query registry and generic dispatch.
//
// This header is the whole contract between the engine backend and the
// per-query code.  The backend names no query.  Everything query-specific
// lives under
//
//   accelerators/<bench>/q<N>/postgres/
//     aux/catalog.cpp          libaccelerator_codegen: catalog registrar
//     extension/scan.cpp       MODULE accelerator_codegen_pg: SPI scans, Runtime,
//                              argument decoding, "aux" executor registrar
//     extension/functions.cpp  MODULE: PG_FUNCTION_INFO_V1 shims + the
//                              CREATE FUNCTION text (SQL registrar)
//     base/base.cpp            MODULE: "base" executor registrar
//
// and plugs in through static registrars.  Registries are plain functions
// defined in runtime.cpp (function-local storage), never header-only template
// singletons.  Engine-level helpers (SPI scan helpers, text normalisation,
// the per-backend Runtime template) are in postgres/engine_helpers.hpp and
// postgres/engine_runtime.hpp.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {

// Executor variants.
inline constexpr const char *kPostgresVariantAux = "aux";
inline constexpr const char *kPostgresVariantBase = "base";

// An executor: decodes the SQL function arguments from fcinfo and writes its
// result rows into the function's materialized result set.
using PostgresExecuteFn = void (*)(FunctionCallInfo fcinfo);

struct PostgresQueryExecutor {
  const char *workload;  // e.g. "tpch"
  int query_id;
  const char *variant;   // one of kPostgresVariant*
  PostgresExecuteFn execute;
};

// DDL that creates the query's SQL-visible functions (and row types), as one
// or more `;`-separated statements, each preceded by its own DROP ... IF
// EXISTS.  Run by accel_postgres_register_queries().
struct PostgresQuerySql {
  const char *workload;
  int query_id;
  const char *sql;
};

void RegisterPostgresQueryExecutor(PostgresQueryExecutor entry);
void RegisterPostgresQuerySql(PostgresQuerySql entry);

struct PostgresQueryExecutorRegistrar {
  explicit PostgresQueryExecutorRegistrar(PostgresQueryExecutor entry) {
    RegisterPostgresQueryExecutor(entry);
  }
};
struct PostgresQuerySqlRegistrar {
  explicit PostgresQuerySqlRegistrar(PostgresQuerySql entry) {
    RegisterPostgresQuerySql(entry);
  }
};

// Generic entry point used by the per-query PG_FUNCTION shims.  Raises a
// PostgreSQL ERROR if no such executor is registered.
void ExecutePostgresQuery(const char *workload, int query_id,
                          const char *variant, FunctionCallInfo fcinfo);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
