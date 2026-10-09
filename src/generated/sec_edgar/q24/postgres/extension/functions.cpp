// GENERATED BACKEND — PostgreSQL SQL function for SEC-EDGAR Q24 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ24);
Datum AcceleratorCodegenAuxSecEdgarQ24(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "sec_edgar", 24,
      accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux,
      fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_postgres_register_queries(). Same SQL name as DuckDB.
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_sec_edgar_q24_aux CASCADE;
CREATE FUNCTION accel_sec_edgar_q24_aux()
RETURNS TABLE (
  tag text,
  version text,
  cnt bigint,
  total double precision
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxSecEdgarQ24'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"sec_edgar", 24, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
