// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ3ParallelFunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ3ParallelFunctionSet{&CreateTpchQ3ParallelFunctionSet};
} // namespace
} // namespace accelerator_codegen
