// GENERATED BACKEND — PostgreSQL SQL functions for TPC-H Q10 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "backends/postgres/api/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ10);
Datum AcceleratorCodegenAuxTpchQ10(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "tpch", 10, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_tpch_q10_aux CASCADE;
CREATE FUNCTION accel_tpch_q10_aux(date)
RETURNS TABLE (
  c_custkey bigint,
  c_name text,
  revenue numeric,
  c_acctbal numeric,
  n_name text,
  c_address text,
  c_phone text,
  c_comment text
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxTpchQ10'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"tpch", 10, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
