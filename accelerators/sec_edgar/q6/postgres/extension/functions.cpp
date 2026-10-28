// GENERATED BACKEND — PostgreSQL SQL functions for SEC EDGAR Q6 (module only).
//   accel_sec_edgar_q6_aux(fy bigint, stmt text, uom text)
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "backends/postgres/api/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ6);
Datum AcceleratorCodegenAuxSecEdgarQ6(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "sec_edgar", 6,
      accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP TYPE IF EXISTS accel_sec_edgar_q6_row CASCADE;
CREATE TYPE accel_sec_edgar_q6_row AS (
  name text, stmt text, tag text, plabel text,
  total_value double precision, cnt bigint);

DROP FUNCTION IF EXISTS accel_sec_edgar_q6_aux() CASCADE;
DROP FUNCTION IF EXISTS accel_sec_edgar_q6_aux(bigint, text, text) CASCADE;
CREATE FUNCTION accel_sec_edgar_q6_aux(bigint, text, text)
RETURNS SETOF accel_sec_edgar_q6_row
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxSecEdgarQ6'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(
    PostgresQuerySql{"sec_edgar", 6, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
