// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q1 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ1(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 1, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 1, kPostgresVariantBase, &ExecutePostgresBaseTpchQ1});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
