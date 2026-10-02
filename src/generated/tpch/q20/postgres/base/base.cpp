// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q20 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ20(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 20, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 20, kPostgresVariantBase, &ExecutePostgresBaseTpchQ20});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
