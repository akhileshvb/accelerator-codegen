// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ2FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ2FunctionSet{&CreateTpchQ2FunctionSet};
} // namespace
} // namespace accelerator_codegen
