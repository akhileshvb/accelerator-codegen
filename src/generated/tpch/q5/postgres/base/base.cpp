// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q5 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ5(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 5, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 5, kPostgresVariantBase, &ExecutePostgresBaseTpchQ5});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
