// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ16FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ16FunctionSet{&CreateTpchQ16FunctionSet};
} // namespace
} // namespace accelerator_codegen
