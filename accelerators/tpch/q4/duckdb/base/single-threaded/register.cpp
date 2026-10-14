// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ4FunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ4FunctionSet{&CreateTpchQ4FunctionSet};
} // namespace
} // namespace accelerator_codegen
