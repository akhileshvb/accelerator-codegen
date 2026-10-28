// GENERATED BACKEND — PostgreSQL SQL functions for SEC-EDGAR Q3 (module only).
//   accel_sec_edgar_q3_aux(fy bigint, uom text)
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "backends/postgres/api/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ3);
Datum AcceleratorCodegenAuxSecEdgarQ3(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "sec_edgar", 3, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_sec_edgar_q3_aux() CASCADE;
DROP FUNCTION IF EXISTS accel_sec_edgar_q3_aux(bigint, text) CASCADE;
CREATE FUNCTION accel_sec_edgar_q3_aux(bigint, text)
RETURNS TABLE (
  name text,
  cik integer,
  total_value double precision
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxSecEdgarQ3'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"sec_edgar", 3, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
