// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ7ParallelFunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ7ParallelFunctionSet{&CreateTpchQ7ParallelFunctionSet};
} // namespace
} // namespace accelerator_codegen
