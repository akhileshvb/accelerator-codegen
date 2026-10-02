// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ10FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ10FunctionSet{&CreateTpchQ10FunctionSet};
} // namespace
} // namespace accelerator_codegen
