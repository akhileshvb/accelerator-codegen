// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q3 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ3);
Datum AcceleratorCodegenBaseTpchQ3(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 3, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantBase, fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ3);
Datum AcceleratorCodegenAuxTpchQ3(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 3, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_tpch_q3 CASCADE;
CREATE FUNCTION accel_tpch_q3(text, date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ3'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q3_aux CASCADE;
CREATE FUNCTION accel_tpch_q3_aux(text, date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ3'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 3, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
