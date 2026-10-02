// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q18 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ18);
Datum AcceleratorCodegenBaseTpchQ18(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 18, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantBase, fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ18);
Datum AcceleratorCodegenAuxTpchQ18(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 18, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_tpch_q18 CASCADE;
CREATE FUNCTION accel_tpch_q18(bigint)
RETURNS TABLE (
  c_name text,
  c_custkey bigint,
  o_orderkey bigint,
  o_orderdate date,
  o_totalprice numeric,
  "sum(l_quantity)" numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenBaseTpchQ18'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_tpch_q18_aux CASCADE;
CREATE FUNCTION accel_tpch_q18_aux(bigint)
RETURNS TABLE (
  c_name text,
  c_custkey bigint,
  o_orderkey bigint,
  o_orderdate date,
  o_totalprice numeric,
  "sum(l_quantity)" numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ18'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 18, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
