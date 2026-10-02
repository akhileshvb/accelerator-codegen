// GENERATED QUERY: registers TPC-H Q1's Verus base kernel table functions.
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet CreateTpchQ1VerusFunctionSet();
duckdb::TableFunctionSet CreateTpchQ1VerusSimpleFunctionSet();
#ifdef ACCEL_ENABLE_PARALLEL_TPCH
duckdb::TableFunctionSet CreateTpchQ1VerusParallelFunctionSet();
#endif
namespace {
const BaseFunctionSetRegistrar kCreateTpchQ1VerusFunctionSet{&CreateTpchQ1VerusFunctionSet};
const BaseFunctionSetRegistrar kCreateTpchQ1VerusSimpleFunctionSet{&CreateTpchQ1VerusSimpleFunctionSet};
#ifdef ACCEL_ENABLE_PARALLEL_TPCH
const BaseFunctionSetRegistrar kCreateTpchQ1VerusParallelFunctionSet{&CreateTpchQ1VerusParallelFunctionSet};
#endif
} // namespace
} // namespace accelerator_codegen
