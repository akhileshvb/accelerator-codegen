// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q2 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ2);
Datum AcceleratorCodegenBaseTpchQ2(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 2, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantBase, fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ2);
Datum AcceleratorCodegenAuxTpchQ2(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 2, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_tpch_q2 CASCADE;
CREATE FUNCTION accel_tpch_q2(integer, text, text)
RETURNS TABLE (
  s_acctbal numeric,
  s_name text,
  n_name text,
  p_partkey bigint,
  p_mfgr text,
  s_address text,
  s_phone text,
  s_comment text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ2'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q2_aux CASCADE;
CREATE FUNCTION accel_tpch_q2_aux(integer, text, text)
RETURNS TABLE (
  s_acctbal numeric,
  s_name text,
  n_name text,
  p_partkey bigint,
  p_mfgr text,
  s_address text,
  s_phone text,
  s_comment text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ2'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 2, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
