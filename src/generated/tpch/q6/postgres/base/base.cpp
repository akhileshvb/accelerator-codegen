// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q6 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ6(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 6, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 6, kPostgresVariantBase, &ExecutePostgresBaseTpchQ6});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
