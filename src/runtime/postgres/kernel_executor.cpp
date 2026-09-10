// Base kernels alias aux Execute (SPI InitialScan over native PG tables).
// No DuckDB DataChunk IR — base tables stay in Postgres storage.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "accelerator_codegen/runtime/postgres/kernel_executor.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

void ExecutePostgresBaseTpchQ1(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ1(fcinfo);
}
void ExecutePostgresBaseTpchQ2(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ2(fcinfo);
}
void ExecutePostgresBaseTpchQ3(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ3(fcinfo);
}
void ExecutePostgresBaseTpchQ4(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ4(fcinfo);
}
void ExecutePostgresBaseTpchQ5(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ5(fcinfo);
}
void ExecutePostgresBaseTpchQ6(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ6(fcinfo);
}
void ExecutePostgresBaseTpchQ7(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ7(fcinfo);
}
void ExecutePostgresBaseTpchQ8(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ8(fcinfo);
}
void ExecutePostgresBaseTpchQ9(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ9(fcinfo);
}
void ExecutePostgresBaseTpchQ10(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ10(fcinfo);
}
void ExecutePostgresBaseTpchQ11(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ11(fcinfo);
}
void ExecutePostgresBaseTpchQ12(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ12(fcinfo);
}
void ExecutePostgresBaseTpchQ13(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ13(fcinfo);
}
void ExecutePostgresBaseTpchQ14(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ14(fcinfo);
}
void ExecutePostgresBaseTpchQ15(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ15(fcinfo);
}
void ExecutePostgresBaseTpchQ16(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ16(fcinfo);
}
void ExecutePostgresBaseTpchQ17(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ17(fcinfo);
}
void ExecutePostgresBaseTpchQ18(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ18(fcinfo);
}
void ExecutePostgresBaseTpchQ19(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ19(fcinfo);
}
void ExecutePostgresBaseTpchQ20(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ20(fcinfo);
}
void ExecutePostgresBaseTpchQ21(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ21(fcinfo);
}
void ExecutePostgresBaseTpchQ22(FunctionCallInfo fcinfo) {
  ExecutePostgresTpchQ22(fcinfo);
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
