// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q6 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "backends/postgres/api/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ6);
Datum AcceleratorCodegenAuxTpchQ6(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 6, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP TYPE IF EXISTS accel_tpch_q6_row CASCADE;
CREATE TYPE accel_tpch_q6_row AS (revenue numeric);

DROP FUNCTION IF EXISTS accel_tpch_q6_aux CASCADE;
CREATE FUNCTION accel_tpch_q6_aux(date, double precision, double precision)
RETURNS SETOF accel_tpch_q6_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ6'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 6, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
