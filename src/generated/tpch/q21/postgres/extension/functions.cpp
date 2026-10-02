// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q21 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ21);
Datum AcceleratorCodegenBaseTpchQ21(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 21, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantBase, fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ21);
Datum AcceleratorCodegenAuxTpchQ21(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 21, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

#if defined(ACCEL_HAVE_Q21_VERUS)
PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ21Verus);
Datum AcceleratorCodegenAuxTpchQ21Verus(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 21, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAuxVerus, fcinfo);
  PG_RETURN_NULL();
}
#endif

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_tpch_q21 CASCADE;
CREATE FUNCTION accel_tpch_q21(text)
RETURNS TABLE (
  s_name text,
  numwait bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ21'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q21_aux CASCADE;
CREATE FUNCTION accel_tpch_q21_aux(text)
RETURNS TABLE (
  s_name text,
  numwait bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ21'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 21, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
