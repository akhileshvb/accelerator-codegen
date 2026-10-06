// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q11 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ11);
Datum AcceleratorCodegenAuxTpchQ11(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 11, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ11);
Datum AcceleratorCodegenBaseTpchQ11(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 11, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantBase, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_tpch_q11(text, double precision) CASCADE;
DROP FUNCTION IF EXISTS accel_tpch_q11(text, numeric) CASCADE;
-- The fraction may be passed as numeric (exact decimal) or float8 (its shortest
-- round-trip decimal is used); both reach the same C entry point.
CREATE FUNCTION accel_tpch_q11(text, numeric)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ11'
LANGUAGE C;
CREATE FUNCTION accel_tpch_q11(text, double precision)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ11'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q11_aux(text, double precision) CASCADE;
DROP FUNCTION IF EXISTS accel_tpch_q11_aux(text, numeric) CASCADE;
CREATE FUNCTION accel_tpch_q11_aux(text, numeric)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ11'
LANGUAGE C;
CREATE FUNCTION accel_tpch_q11_aux(text, double precision)
RETURNS TABLE (
  ps_partkey bigint,
  value numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ11'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 11, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
