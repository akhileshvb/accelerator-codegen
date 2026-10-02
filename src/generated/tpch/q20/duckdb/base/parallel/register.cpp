// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ20ParallelFunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ20ParallelFunctionSet{&CreateTpchQ20ParallelFunctionSet};
} // namespace
} // namespace accelerator_codegen
