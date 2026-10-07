// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q17 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ17);
Datum AcceleratorCodegenAuxTpchQ17(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 17, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP TYPE IF EXISTS accel_tpch_q17_row CASCADE;
CREATE TYPE accel_tpch_q17_row AS (avg_yearly double precision);

DROP FUNCTION IF EXISTS accel_tpch_q17_aux CASCADE;
CREATE FUNCTION accel_tpch_q17_aux(text, text)
RETURNS SETOF accel_tpch_q17_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ17'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 17, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
