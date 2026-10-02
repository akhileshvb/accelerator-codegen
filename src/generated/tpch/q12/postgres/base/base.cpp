// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q12 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ12(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 12, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 12, kPostgresVariantBase, &ExecutePostgresBaseTpchQ12});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
