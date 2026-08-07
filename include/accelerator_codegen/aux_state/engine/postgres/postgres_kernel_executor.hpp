#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "fmgr.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {

void ExecutePostgresBaseTpchQ1(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ2(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ3(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ4(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ5(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ6(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ7(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ8(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ9(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ10(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ11(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ12(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ13(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ14(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ15(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ16(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ17(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ18(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ19(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ20(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ21(FunctionCallInfo fcinfo);
void ExecutePostgresBaseTpchQ22(FunctionCallInfo fcinfo);
void ExecutePostgresBaseSecEdgarQ1(FunctionCallInfo fcinfo);
void ExecutePostgresBaseSecEdgarQ2(FunctionCallInfo fcinfo);
void ExecutePostgresBaseSecEdgarQ3(FunctionCallInfo fcinfo);
void ExecutePostgresBaseSecEdgarQ4(FunctionCallInfo fcinfo);
void ExecutePostgresBaseSecEdgarQ6(FunctionCallInfo fcinfo);
void ExecutePostgresBaseSecEdgarQ24(FunctionCallInfo fcinfo);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
