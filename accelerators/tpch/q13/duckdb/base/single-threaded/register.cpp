// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ13FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ13FunctionSet{&CreateTpchQ13FunctionSet};
} // namespace
} // namespace accelerator_codegen
