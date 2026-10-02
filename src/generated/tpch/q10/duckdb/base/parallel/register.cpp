// GENERATED QUERY: registers this query's DuckDB base kernel table function(s).
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ10ParallelFunctionSet();
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ10ParallelFunctionSet{&CreateTpchQ10ParallelFunctionSet};
} // namespace
} // namespace accelerator_codegen
