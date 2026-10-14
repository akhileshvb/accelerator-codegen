// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ22FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ22FunctionSet{&CreateTpchQ22FunctionSet};
} // namespace
} // namespace accelerator_codegen
