// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q16 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ16);
Datum AcceleratorCodegenAuxTpchQ16(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 16, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_tpch_q16_aux CASCADE;
CREATE FUNCTION accel_tpch_q16_aux(text, text, integer, integer, integer, integer, integer, integer, integer, integer)
RETURNS TABLE (
  p_brand text,
  p_type text,
  p_size integer,
  supplier_cnt bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ16'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 16, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
