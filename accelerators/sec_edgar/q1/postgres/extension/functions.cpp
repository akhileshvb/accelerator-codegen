// PostgreSQL SQL function for SEC EDGAR Q1 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "backends/postgres/api/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ1);
Datum AcceleratorCodegenAuxSecEdgarQ1(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresQuery(
      "sec_edgar", 1, accelerator_codegen::aux_state::engine::postgres::kPostgresVariantAux, fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Executed by accel_register_queries().
constexpr char kFunctionsSql[] = R"SQL(
DROP FUNCTION IF EXISTS accel_sec_edgar_q1_aux CASCADE;
CREATE FUNCTION accel_sec_edgar_q1_aux(stmt text)
RETURNS TABLE (
  stmt text,
  rfile text,
  cnt bigint,
  num_filings bigint,
  avg_line_num numeric
)
AS 'accelerator_codegen_pg', 'AcceleratorCodegenAuxSecEdgarQ1'
LANGUAGE C;
)SQL";

const PostgresQuerySqlRegistrar kSqlRegistrar(PostgresQuerySql{"sec_edgar", 1, kFunctionsSql});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
