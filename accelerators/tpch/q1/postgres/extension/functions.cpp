// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q1 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "backends/postgres/api/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ1);
Datum AcceleratorCodegenAuxTpchQ1(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 1, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
-- Stateful auxiliary kernels (shared AuxQueryModule path)
DROP FUNCTION IF EXISTS accel_tpch_q1_aux CASCADE;
CREATE FUNCTION accel_tpch_q1_aux(integer)
RETURNS TABLE (
  l_returnflag text,
  l_linestatus text,
  sum_qty numeric,
  sum_base_price numeric,
  sum_disc_price numeric,
  sum_charge numeric,
  avg_qty numeric,
  avg_price numeric,
  avg_disc numeric,
  count_order bigint
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ1'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 1, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
