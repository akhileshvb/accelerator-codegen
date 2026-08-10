// SCAFFOLDING
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

#include "accelerator_codegen/runtime/postgres/kernel_executor.hpp"
#include "postgres/runtime.hpp"

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ1);
Datum AcceleratorCodegenBaseTpchQ1(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ1(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ1);
Datum AcceleratorCodegenAuxTpchQ1(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresTpchQ1(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ3);
Datum AcceleratorCodegenBaseTpchQ3(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ3(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ3);
Datum AcceleratorCodegenAuxTpchQ3(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresTpchQ3(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ6);
Datum AcceleratorCodegenBaseTpchQ6(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ6(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ6);
Datum AcceleratorCodegenAuxTpchQ6(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresTpchQ6(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ10);
Datum AcceleratorCodegenBaseTpchQ10(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ10(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ10);
Datum AcceleratorCodegenAuxTpchQ10(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresTpchQ10(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ12);
Datum AcceleratorCodegenBaseTpchQ12(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ12(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ12);
Datum AcceleratorCodegenAuxTpchQ12(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresTpchQ12(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ14);
Datum AcceleratorCodegenBaseTpchQ14(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ14(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ2);
Datum AcceleratorCodegenBaseTpchQ2(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ2(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ4);
Datum AcceleratorCodegenBaseTpchQ4(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ4(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ5);
Datum AcceleratorCodegenBaseTpchQ5(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ5(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ7);
Datum AcceleratorCodegenBaseTpchQ7(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ7(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ8);
Datum AcceleratorCodegenBaseTpchQ8(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ8(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ9);
Datum AcceleratorCodegenBaseTpchQ9(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ9(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ11);
Datum AcceleratorCodegenBaseTpchQ11(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ11(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ13);
Datum AcceleratorCodegenBaseTpchQ13(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ13(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ15);
Datum AcceleratorCodegenBaseTpchQ15(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ15(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ16);
Datum AcceleratorCodegenBaseTpchQ16(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ16(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ17);
Datum AcceleratorCodegenBaseTpchQ17(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ17(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ18);
Datum AcceleratorCodegenBaseTpchQ18(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ18(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ19);
Datum AcceleratorCodegenBaseTpchQ19(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ19(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ20);
Datum AcceleratorCodegenBaseTpchQ20(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ20(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ21);
Datum AcceleratorCodegenBaseTpchQ21(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ21(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseTpchQ22);
Datum AcceleratorCodegenBaseTpchQ22(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseTpchQ22(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxTpchQ14);
Datum AcceleratorCodegenAuxTpchQ14(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresTpchQ14(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseSecEdgarQ1);
Datum AcceleratorCodegenBaseSecEdgarQ1(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseSecEdgarQ1(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ1);
Datum AcceleratorCodegenAuxSecEdgarQ1(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresSecEdgarQ1(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseSecEdgarQ2);
Datum AcceleratorCodegenBaseSecEdgarQ2(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseSecEdgarQ2(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ2);
Datum AcceleratorCodegenAuxSecEdgarQ2(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresSecEdgarQ2(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseSecEdgarQ3);
Datum AcceleratorCodegenBaseSecEdgarQ3(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseSecEdgarQ3(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ3);
Datum AcceleratorCodegenAuxSecEdgarQ3(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresSecEdgarQ3(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseSecEdgarQ4);
Datum AcceleratorCodegenBaseSecEdgarQ4(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseSecEdgarQ4(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ4);
Datum AcceleratorCodegenAuxSecEdgarQ4(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresSecEdgarQ4(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseSecEdgarQ6);
Datum AcceleratorCodegenBaseSecEdgarQ6(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseSecEdgarQ6(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ6);
Datum AcceleratorCodegenAuxSecEdgarQ6(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresSecEdgarQ6(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenBaseSecEdgarQ24);
Datum AcceleratorCodegenBaseSecEdgarQ24(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresBaseSecEdgarQ24(fcinfo);
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(AcceleratorCodegenAuxSecEdgarQ24);
Datum AcceleratorCodegenAuxSecEdgarQ24(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecutePostgresSecEdgarQ24(fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

#endif
