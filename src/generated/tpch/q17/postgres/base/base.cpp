// GENERATED BACKEND — PostgreSQL base kernel for TPC-H Q17 (module only).
// Base kernels alias the aux Execute (SPI InitialScan over native PG
// tables).  No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

void ExecutePostgresBaseTpchQ17(FunctionCallInfo fcinfo) {
  ExecutePostgresQuery("tpch", 17, kPostgresVariantAux, fcinfo);
}

const PostgresQueryExecutorRegistrar kBaseRegistrar(PostgresQueryExecutor{
    "tpch", 17, kPostgresVariantBase, &ExecutePostgresBaseTpchQ17});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
