// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q11 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ11(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 11, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 11, kPostgresVariantBase, &ExecutePostgresBaseTpchQ11});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
