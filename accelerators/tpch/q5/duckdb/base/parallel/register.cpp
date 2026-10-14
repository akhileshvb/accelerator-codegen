// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ5ParallelFunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ5ParallelFunctionSet{&CreateTpchQ5ParallelFunctionSet};
} // namespace
} // namespace accelerator_codegen
