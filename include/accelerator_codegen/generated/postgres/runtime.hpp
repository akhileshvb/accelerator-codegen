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
void ExecutePostgresTpchQ10(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ12(FunctionCallInfo fcinfo);
void ExecutePostgresTpchQ14(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ1(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ2(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ3(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ4(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ6(FunctionCallInfo fcinfo);
void ExecutePostgresSecEdgarQ24(FunctionCallInfo fcinfo);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
