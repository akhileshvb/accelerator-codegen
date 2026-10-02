// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q8 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ8(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 8, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 8, kPostgresVariantBase, &ExecutePostgresBaseTpchQ8});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
