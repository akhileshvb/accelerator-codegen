// GENERATED BACKEND — PostgreSQL SQL functions for SEC-EDGAR Q4 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ4);
Datum AcceleratorCodegenAuxSecEdgarQ4(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "sec_edgar", 4, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_sec_edgar_q4_aux CASCADE;
CREATE FUNCTION accel_sec_edgar_q4_aux()
RETURNS TABLE (
  sic integer,
  tlabel text,
  stmt text,
  num_companies bigint,
  total_value double precision,
  avg_value double precision
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxSecEdgarQ4'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"sec_edgar", 4, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
