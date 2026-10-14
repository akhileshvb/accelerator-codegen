// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ5FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ5FunctionSet{&CreateTpchQ5FunctionSet};
} // namespace
} // namespace accelerator_codegen
