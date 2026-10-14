// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ7FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ7FunctionSet{&CreateTpchQ7FunctionSet};
} // namespace
} // namespace accelerator_codegen
