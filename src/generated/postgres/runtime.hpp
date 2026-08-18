// GENERATED BACKEND
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {

void ExecutePostgresTpchQ1(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ3(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ6(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ9(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ10(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ11(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ12(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ14(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ17(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ18(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ21(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ1(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ2(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ3(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ4(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ6(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ24(FunctionCallInfo fcinfo);
#ifdef ACCEL_ENABLE_PARALLEL_AUX
void ExecutePostgresSecEdgarQ1Parallel(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ2Parallel(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ3Parallel(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ4Parallel(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ6Parallel(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ24Parallel(FunctionCallInfo fcinfo);
#endif

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
