// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q14 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "backends/postgres/api/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ14);
Datum AcceleratorCodegenAuxTpchQ14(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 14, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP TYPE IF EXISTS accel_tpch_q14_row CASCADE;
CREATE TYPE accel_tpch_q14_row AS (promo_revenue double precision);

DROP FUNCTION IF EXISTS accel_tpch_q14_aux CASCADE;
CREATE FUNCTION accel_tpch_q14_aux(date)
RETURNS SETOF accel_tpch_q14_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ14'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 14, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
